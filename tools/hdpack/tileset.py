# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Location tile sets: redraw the city tiles one location uses, in context.

A location (data/locations/<dir>/<name>.json, format pthd.location) draws its
map with tile ids of the classic 256-tile atlas gfx/landsym (16x16 original
pixels per tile; id = column * 16 + row). Its own art goes into a LOCATION
atlas, <pack>/locations/<dir>/gfx/landsym.png, never into the shared one.

  python tileset.py plan <export> <location.json> <job>
        [--district d-village-square] [--descriptions tiles.json] [--scale 12]
        [--global-seed 2026122500]
      For every tile id the location uses (or only those used in the given
      districts): a 3x3 neighbourhood from the location's own grid (the most
      common real arrangement around that id; a uniform surround of the id
      itself when it never has eight neighbours), scaled with MMPX to
      --scale, written as <job>/in/tiles/<dir>/tHH.png with a sidecar, and
      the redraw.py items in <job>/items.json. Seeds are per district group:
      every id belongs to the district where it is used most, and all ids of
      one group share the seed asset_seed(global, workflow, "<dir>:<district>").

  (GPU, by whoever holds the lease) depthmaps.py <job>/in <job>/out/maps, then
  redraw.py <job>/in <job>/out/maps <job>/out/redraw --items-json <job>/items.json

  python tileset.py assemble <export> <classic HD landsym.png> <location.json>
        <job>/plan.json <job>/out/redraw/<denoise> <dest> [--max-seam 3]
      Cuts the centre tile out of every redrawn neighbourhood, checks each
      seam against the neighbours it really has on the map (edge_match, CIE
      Delta E; at most --max-seam worse than the same seam in the 1994 art),
      and writes the full 256-id location atlas: ids not redrawn,
      and ids whose seams fail, stay the classic HD picture byte for byte.
        <dest>/atlas/locations/<dir>/gfx/landsym.png   (copy atlas/* into the pack)
        <dest>/tiles/<dir>/tHH.png                     every cut tile, accepted or not
        <dest>/seams.json, <dest>/compare.png          seam report, classic vs new map
      Every PNG gets a provenance sidecar.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections import Counter
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from io import BytesIO
from pathlib import Path
from typing import Any

import numpy as np
import numpy.typing as npt
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hdpack  # noqa: E402
import pixelart  # noqa: E402
import provenance  # noqa: E402

TILE = 16  # original pixels per tile side
PER_ROW = 16  # tiles per atlas column/row
ATLAS = TILE * PER_ROW
DEFAULT_SCALE = 12
DEFAULT_GLOBAL_SEED = 2026122500
MAX_SEAM = 3.0
SEAM_STRIP = 2
PNG_COMPRESS_LEVEL = 9
PLAN_SCHEMA = "plegacy.tileset-plan/1"
SEAMS_SCHEMA = "plegacy.tileset-seams/1"
# The redraw graph the seeds are tied to (redraw.py: SDXL img2img with a
# depth ControlNet). A different graph or model gives different seeds.
REDRAW_WORKFLOW = {
    "tool": "redraw.py",
    "graph": "sdxl-img2img-depth-controlnet",
    "checkpoint": "juggernautXL_v9.safetensors",
    "controlnet": "diffusers_xl_depth_full.safetensors",
}
TILE_STYLE = (
    "top-down oblique view, one square tile of a 1990s city-builder map, hand-painted cartoon art, "
    "snowy North Pole village, warm winter light from the top left, crisp clean details, "
    "the picture fills the whole frame and continues past every edge"
)
TILE_NEGATIVE = (
    "photo, photorealistic, text, watermark, signature, frame, border, vignette, black outline, "
    "blurry, noisy, jpeg artifacts, deformed, isometric, perspective change, people, cars, "
    "pixel art, pixelated, blocky, mosaic, grid lines"
)

U8 = npt.NDArray[np.uint8]
F64 = npt.NDArray[np.float64]


class TilesetError(hdpack.HdPackError):
    """A location, atlas or job folder the user has to fix."""


# ----------------------------------------------------------------- location


@dataclass(frozen=True)
class Location:
    key: str
    folder: str  # the atlas folder name, e.g. "north-pole"
    width: int
    height: int
    grid: tuple[tuple[int, ...], ...]  # [y][x] tile ids
    districts: tuple[str, ...]  # district ids, in index order
    cell_w: int
    cell_h: int
    cells: tuple[tuple[int, ...], ...]  # [row][col] district index
    sha256: str

    def district_at(self, x: int, y: int) -> str:
        return self.districts[self.cells[y // self.cell_h][x // self.cell_w]]

    def at(self, x: int, y: int) -> int | None:
        if 0 <= x < self.width and 0 <= y < self.height:
            return self.grid[y][x]
        return None


def _hex_rows(rows: Sequence[str], width: int, where: str) -> list[list[int]]:
    out: list[list[int]] = []
    for y, row in enumerate(rows):
        if not isinstance(row, str) or len(row) != width * 2:
            raise TilesetError(f"{where} row {y}: expected {width * 2} hex digits")
        try:
            out.append([int(row[2 * x : 2 * x + 2], 16) for x in range(width)])
        except ValueError as exc:
            raise TilesetError(f"{where} row {y}: {exc}") from exc
    return out


def load_location(path: Path, folder: str | None = None) -> Location:
    try:
        raw = path.read_bytes()
        doc = json.loads(raw.decode("utf-8"))
        grid = doc["grid"]
        width, height = int(grid["width"]), int(grid["height"])
        cols, rows = int(grid["district_cell_columns"]), int(grid["district_cell_rows"])
        if grid.get("tile_encoding") != "hex-rows-v1":
            raise TilesetError(f"{path}: tile_encoding {grid.get('tile_encoding')!r} is not hex-rows-v1")
        tiles = _hex_rows(doc["tiles"], width, "tiles")
        cells = [[v & 0x0F for v in r] for r in _hex_rows(doc["district_cells"], cols, "district_cells")]
        districts = tuple(str(d["id"]) for d in doc["districts"])
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise TilesetError(f"{path}: not a usable location ({exc})") from exc
    if len(tiles) != height or len(cells) != rows or width % cols or height % rows:
        raise TilesetError(f"{path}: grid {width}x{height} does not match its rows or district cells")
    if any(v >= len(districts) for r in cells for v in r):
        raise TilesetError(f"{path}: a district cell names a district that does not exist")
    return Location(
        key=str(doc.get("location_key", path.stem)),
        folder=folder or path.parent.name,
        width=width,
        height=height,
        grid=tuple(tuple(r) for r in tiles),
        districts=districts,
        cell_w=width // cols,
        cell_h=height // rows,
        cells=tuple(tuple(r) for r in cells),
        sha256=provenance.sha256_bytes(raw),
    )


# -------------------------------------------------------------------- atlas


def tile_origin(tile_id: int, size: int = TILE) -> tuple[int, int]:
    """Top-left of a tile in the atlas (engine: landsym_source_rect)."""
    if not 0 <= tile_id < PER_ROW * PER_ROW:
        raise TilesetError(f"tile id {tile_id} is outside the atlas")
    return (tile_id // PER_ROW) * size, (tile_id % PER_ROW) * size


def tile_of(atlas: U8, tile_id: int, size: int) -> U8:
    x, y = tile_origin(tile_id, size)
    return atlas[y : y + size, x : x + size]


def read_rgba(path: Path) -> U8:
    with Image.open(path) as im:
        return np.asarray(im.convert("RGBA"), dtype=np.uint8)


def encode_png(rgba: U8) -> bytes:
    buf = BytesIO()
    Image.fromarray(rgba, "RGBA").save(buf, "PNG", optimize=False, compress_level=PNG_COMPRESS_LEVEL)
    return buf.getvalue()


def neighbourhood_original(atlas: U8, ids: Sequence[Sequence[int]]) -> U8:
    """The 3x3 neighbourhood at original size (48x48), ids[row][col]."""
    out = np.zeros((3 * TILE, 3 * TILE, 4), dtype=np.uint8)
    for r in range(3):
        for c in range(3):
            out[r * TILE : (r + 1) * TILE, c * TILE : (c + 1) * TILE] = tile_of(atlas, ids[r][c], TILE)
    return out


def mmpx_passes(scale: int) -> int:
    return max(1, math.ceil(math.log2(scale)))


def neighbourhood_hd(atlas: U8, ids: Sequence[Sequence[int]], scale: int) -> U8:
    """The neighbourhood scaled with MMPX (pixelart.upscale) to scale x."""
    small = Image.fromarray(neighbourhood_original(atlas, ids), "RGBA")
    size = (3 * TILE * scale, 3 * TILE * scale)
    return np.asarray(pixelart.upscale(small, size, mmpx_passes(scale)), dtype=np.uint8)


def centre(crop: U8) -> U8:
    """The middle tile of a 3x3 neighbourhood picture."""
    h, w = crop.shape[:2]
    if h != w or h % 3:
        raise TilesetError(f"a neighbourhood must be square with a side divisible by 3, not {w}x{h}")
    t = h // 3
    return crop[t : 2 * t, t : 2 * t]


# --------------------------------------------------------------------- plan


@dataclass(frozen=True)
class Neighbourhood:
    ids: tuple[tuple[int, ...], ...]  # 3x3, [row][col]
    source: str  # "grid" or "uniform"
    at: tuple[int, int] | None  # grid x, y of the centre (first occurrence of this arrangement)
    occurrences: int


def choose_neighbourhood(loc: Location, tile_id: int) -> Neighbourhood:
    """The most common real 3x3 arrangement around tile_id (ties: the one
    seen first in reading order); a uniform surround when tile_id never has
    eight neighbours inside the grid."""
    counts: Counter[tuple[tuple[int, ...], ...]] = Counter()
    first: dict[tuple[tuple[int, ...], ...], tuple[int, int]] = {}
    for y in range(1, loc.height - 1):
        row = loc.grid[y]
        for x in range(1, loc.width - 1):
            if row[x] != tile_id:
                continue
            ids = tuple(tuple(loc.grid[y + dy][x - 1 : x + 2]) for dy in (-1, 0, 1))
            counts[ids] += 1
            first.setdefault(ids, (x, y))
    if not counts:
        return Neighbourhood(tuple(tuple([tile_id] * 3) for _ in range(3)), "uniform", None, 0)
    best = max(counts, key=lambda k: (counts[k], -first[k][1], -first[k][0]))
    return Neighbourhood(best, "grid", first[best], counts[best])


def usage(loc: Location) -> dict[int, Counter[str]]:
    """tile id -> how often each district uses it."""
    out: dict[int, Counter[str]] = {}
    for y in range(loc.height):
        for x in range(loc.width):
            out.setdefault(loc.grid[y][x], Counter())[loc.district_at(x, y)] += 1
    return out


def group_of(loc: Location, counts: Counter[str]) -> str:
    """The district that uses the id most (ties: the lower district index)."""
    district = max(counts, key=lambda d: (counts[d], -loc.districts.index(d)))
    return f"{loc.folder}:{district}"


def group_seed(global_seed: int, group: str) -> int:
    return provenance.asset_seed(global_seed, provenance.workflow_id(REDRAW_WORKFLOW), group)


def tile_rel(loc: Location, tile_id: int) -> str:
    return f"tiles/{loc.folder}/t{tile_id:02x}.png"


@dataclass
class PlanEntry:
    tile_id: int
    file: str
    group: str
    seed: int
    uses: dict[str, int]
    neighbourhood: Neighbourhood
    description: str

    def to_json(self) -> dict[str, Any]:
        n = self.neighbourhood
        return {
            "tile_id": f"{self.tile_id:02x}",
            "file": self.file,
            "group": self.group,
            "seed": self.seed,
            "uses": dict(sorted(self.uses.items())),
            "neighbourhood": [[f"{v:02x}" for v in row] for row in n.ids],
            "neighbourhood_source": n.source,
            "neighbourhood_at": list(n.at) if n.at else None,
            "neighbourhood_occurrences": n.occurrences,
            "description": self.description,
        }


def load_descriptions(path: Path | None) -> dict[int, str]:
    if path is None:
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        tiles = data["tiles"]
        return {int(k, 16): str(v) for k, v in tiles.items() if not k.startswith("_")}
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        raise TilesetError(f'{path}: expected {{"tiles": {{"<hex id>": "description"}}}} ({exc})') from exc


def make_plan(
    loc: Location,
    districts: Sequence[str] = (),
    descriptions: Mapping[int, str] | None = None,
    global_seed: int = DEFAULT_GLOBAL_SEED,
) -> list[PlanEntry]:
    for d in districts:
        if d not in loc.districts:
            raise TilesetError(f"unknown district {d!r}; the location has {', '.join(loc.districts)}")
    descriptions = descriptions or {}
    entries: list[PlanEntry] = []
    for tile_id, counts in sorted(usage(loc).items()):
        if districts and not any(counts[d] for d in districts):
            continue
        group = group_of(loc, counts)
        entries.append(
            PlanEntry(
                tile_id,
                tile_rel(loc, tile_id),
                group,
                group_seed(global_seed, group),
                dict(counts),
                choose_neighbourhood(loc, tile_id),
                descriptions.get(tile_id, f"map tile {tile_id:02x} of a snowy North Pole village"),
            )
        )
    return entries


def write_plan(
    export: Path,
    loc_path: Path,
    job: Path,
    districts: Sequence[str] = (),
    descriptions_path: Path | None = None,
    scale: int = DEFAULT_SCALE,
    global_seed: int = DEFAULT_GLOBAL_SEED,
    folder: str | None = None,
) -> list[PlanEntry]:
    loc = load_location(loc_path, folder)
    atlas_path = hdpack.contained_path(export, "gfx/landsym.png")
    atlas = read_rgba(atlas_path)
    if atlas.shape[:2] != (ATLAS, ATLAS):
        raise TilesetError(f"{atlas_path}: expected {ATLAS}x{ATLAS}, got {atlas.shape[1]}x{atlas.shape[0]}")
    atlas_sha = provenance.sha256_file(atlas_path)
    entries = make_plan(loc, districts, load_descriptions(descriptions_path), global_seed)
    inp = job / "in"
    wid = provenance.workflow_id(REDRAW_WORKFLOW)
    for e in entries:
        target = hdpack.contained_path(inp, e.file)
        provenance.write_bytes_atomic(target, encode_png(neighbourhood_hd(atlas, e.neighbourhood.ids, scale)))
        provenance.write(
            inp,
            e.file,
            provenance.Record(
                stage="tileset-crop",
                asset={"file": e.file, "id": provenance.asset_id(e.file), "tile_id": f"{e.tile_id:02x}"},
                input_file="gfx/landsym.png",
                input_sha256=atlas_sha,
                outputs={e.file: provenance.sha256_file(target)},
                seed=e.seed,
                seed_inputs={"global_seed": global_seed, "workflow_id": wid, "group": e.group},
                parameters={
                    "location": loc.key,
                    "location_sha256": loc.sha256,
                    "scale": scale,
                    "upscaler": f"MMPX x{2 ** mmpx_passes(scale)} then area average",
                }
                | {k: v for k, v in e.to_json().items() if k.startswith("neighbourhood")},
            ),
        )
    items = [
        {"file": e.file, "description": e.description, "seed": e.seed, "tile_id": f"{e.tile_id:02x}"} for e in entries
    ]
    plan = {
        "schema": PLAN_SCHEMA,
        "location": loc.key,
        "location_file": loc_path.name,
        "location_sha256": loc.sha256,
        "atlas_folder": f"locations/{loc.folder}/gfx/landsym.png",
        "export_landsym_sha256": atlas_sha,
        "scale": scale,
        "global_seed": global_seed,
        "workflow": REDRAW_WORKFLOW,
        "workflow_id": wid,
        "districts": list(districts) or list(loc.districts),
        "style": TILE_STYLE,
        "negative": TILE_NEGATIVE,
        "tiles": [e.to_json() for e in entries],
    }
    provenance.write_bytes_atomic(job / "items.json", (json.dumps(items, indent=1) + "\n").encode("utf-8"))
    provenance.write_bytes_atomic(job / "plan.json", (json.dumps(plan, indent=1) + "\n").encode("utf-8"))
    return entries


# --------------------------------------------------------------------- seams


def srgb_to_lab(rgb: npt.NDArray[Any]) -> F64:
    """sRGB (0..255) to CIELAB (D65)."""
    c = np.asarray(rgb, dtype=np.float64) / 255.0
    lin = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    m = np.array(
        [[0.4124564, 0.3575761, 0.1804375], [0.2126729, 0.7151522, 0.0721750], [0.0193339, 0.1191920, 0.9503041]]
    )
    xyz = lin @ m.T / np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > (6 / 29) ** 3, np.cbrt(xyz), xyz / (3 * (6 / 29) ** 2) + 4 / 29)
    lab = np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], axis=-1)
    return np.asarray(lab, dtype=np.float64)


def edge_match(first: U8, second: U8, side: str, strip: int = SEAM_STRIP) -> float:
    """Mean CIE76 Delta E across the seam where `second` touches `first` on
    `side` ("right": second is right of first; "below": second is under it).
    Each side of the seam is the mean of its last `strip` texels."""
    if side == "right":
        a = first[:, -strip:, :3].mean(axis=1)
        b = second[:, :strip, :3].mean(axis=1)
    elif side == "below":
        a = first[-strip:, :, :3].mean(axis=0)
        b = second[:strip, :, :3].mean(axis=0)
    else:
        raise TilesetError(f"side must be 'right' or 'below', not {side!r}")
    return float(np.linalg.norm(srgb_to_lab(a) - srgb_to_lab(b), axis=-1).mean())


def seam_pairs(loc: Location) -> Counter[tuple[int, int, str]]:
    """Every (first, second, side) pair the map really has, with counts."""
    pairs: Counter[tuple[int, int, str]] = Counter()
    for y in range(loc.height):
        for x in range(loc.width):
            here = loc.grid[y][x]
            right, below = loc.at(x + 1, y), loc.at(x, y + 1)
            if right is not None:
                pairs[(here, right, "right")] += 1
            if below is not None:
                pairs[(here, below, "below")] += 1
    return pairs


# ------------------------------------------------------------------ assemble


@dataclass
class TileResult:
    tile_id: int
    file: str
    sha256: str
    accepted: bool = True
    seams: list[dict[str, Any]] = field(default_factory=list)
    reason: str = ""


@dataclass
class AssembleReport:
    atlas: str = ""
    atlas_sha256: str = ""
    replaced: list[int] = field(default_factory=list)
    rejected: list[int] = field(default_factory=list)
    missing: list[int] = field(default_factory=list)
    tiles: dict[int, TileResult] = field(default_factory=dict)

    def to_json(self) -> dict[str, Any]:
        return {
            "schema": SEAMS_SCHEMA,
            "atlas": self.atlas,
            "atlas_sha256": self.atlas_sha256,
            "replaced": [f"{t:02x}" for t in self.replaced],
            "rejected": [f"{t:02x}" for t in self.rejected],
            "missing": [f"{t:02x}" for t in self.missing],
            "tiles": {
                f"{t:02x}": {
                    "file": r.file,
                    "sha256": r.sha256,
                    "accepted": r.accepted,
                    "reason": r.reason,
                    "seams": r.seams,
                }
                for t, r in sorted(self.tiles.items())
            },
        }


def build_atlas(classic: U8, tiles: Mapping[int, U8]) -> U8:
    """The classic HD atlas with the given tiles pasted in; every other
    texel is the classic one, unchanged."""
    size = classic.shape[0] // PER_ROW
    out = classic.copy()
    for tile_id, tile in tiles.items():
        if tile.shape[:2] != (size, size):
            raise TilesetError(f"tile {tile_id:02x} is {tile.shape[1]}x{tile.shape[0]}, the atlas needs {size}")
        x, y = tile_origin(tile_id, size)
        out[y : y + size, x : x + size] = tile
    return out


def check_seams(
    tile_id: int,
    atlas: U8,
    reference: U8,
    pairs: Counter[tuple[int, int, str]],
    max_seam: float,
    mode: str,
    redrawn: frozenset[int] | None = None,
) -> tuple[bool, list[dict[str, Any]]]:
    """Every seam of tile_id on the map, in the new atlas against the same
    seam in the reference atlas (by default the 1994 art scaled up without
    smoothing). relative: the new seam may be at most max_seam Delta E worse
    than the reference seam (a kerb or a roof edge is a hard edge in the
    original too); absolute: at most max_seam.

    With `redrawn`, only seams between two redrawn tiles decide; a seam to
    a tile that is still classic is reported ("decides": false) but does not
    reject: the batch would otherwise unravel, each rejected tile turning
    its neighbours' seams into seams against classic art. Those seams are
    settled when the neighbour's own group is redrawn."""
    size = atlas.shape[0] // PER_ROW
    results: list[dict[str, Any]] = []
    ok = True
    for (first, second, side), count in sorted(pairs.items()):
        if tile_id not in (first, second):
            continue
        new = edge_match(tile_of(atlas, first, size), tile_of(atlas, second, size), side)
        old = edge_match(tile_of(reference, first, size), tile_of(reference, second, size), side)
        excess = new - old if mode == "relative" else new
        passed = excess <= max_seam
        decides = redrawn is None or (first in redrawn and second in redrawn)
        ok = ok and (passed or not decides)
        results.append(
            {
                "pair": f"{first:02x}|{second:02x}" if side == "right" else f"{first:02x}/{second:02x}",
                "side": side,
                "count": count,
                "delta_e": round(new, 3),
                "reference_delta_e": round(old, 3),
                "excess": round(excess, 3),
                "pass": passed,
                "decides": decides,
            }
        )
    return ok, results


def assemble(
    export: Path,
    classic_path: Path,
    loc_path: Path,
    plan_path: Path,
    redrawn: Path,
    dest: Path,
    max_seam: float = MAX_SEAM,
    mode: str = "relative",
    keep_failed: bool = False,
    reference: str = "original",
    strict: bool = False,
    log: Any = print,
) -> AssembleReport:
    try:
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        if plan.get("schema") != PLAN_SCHEMA:
            raise TilesetError(f"{plan_path}: expected schema {PLAN_SCHEMA!r}")
        entries = [(int(t["tile_id"], 16), str(t["file"]), str(t["group"]), int(t["seed"])) for t in plan["tiles"]]
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise TilesetError(f"{plan_path}: not a tileset plan ({exc})") from exc
    loc = load_location(loc_path, Path(plan["atlas_folder"]).parts[1] if "atlas_folder" in plan else None)
    if loc.sha256 != plan.get("location_sha256"):
        log(f"warning: {loc_path.name} changed since the plan (sha256 {loc.sha256[:12]})")
    classic = read_rgba(classic_path)
    if classic.shape[0] != classic.shape[1] or classic.shape[0] % ATLAS:
        raise TilesetError(f"{classic_path}: expected a square atlas of 256 x k texels")
    size = classic.shape[0] // PER_ROW
    classic_sha = provenance.sha256_file(classic_path)
    export_path = hdpack.contained_path(export, "gfx/landsym.png")
    export_sha = provenance.sha256_file(export_path)
    if plan.get("export_landsym_sha256") not in (None, export_sha):
        raise TilesetError(f"{export_path} is not the atlas the plan was made from (sha256 {export_sha[:12]})")
    if reference == "original":
        ref = np.asarray(
            Image.fromarray(read_rgba(export_path), "RGBA").resize(classic.shape[1::-1], Image.Resampling.NEAREST),
            dtype=np.uint8,
        )
    elif reference == "classic":
        ref = classic
    else:
        raise TilesetError(f"seam reference must be 'original' or 'classic', not {reference!r}")
    report = AssembleReport()
    cut: dict[int, U8] = {}
    sources: dict[int, tuple[str, str]] = {}
    for tile_id, rel, _group, _seed in entries:
        try:
            src = hdpack.contained_path(redrawn, rel)
        except hdpack.UnsafePathError as exc:
            raise TilesetError(f"{rel}: {exc}") from exc
        if not src.is_file():
            report.missing.append(tile_id)
            continue
        crop = read_rgba(src)
        if crop.shape[:2] != (3 * size, 3 * size):
            crop = np.asarray(
                Image.fromarray(crop, "RGBA").resize((3 * size, 3 * size), Image.Resampling.LANCZOS), dtype=np.uint8
            )
        tile = centre(crop).copy()
        tile[..., 3] = 255  # map tiles are opaque
        cut[tile_id] = tile
        sources[tile_id] = (rel, provenance.sha256_file(src))
    # Seams: while any kept tile fails, put back the classic picture of the
    # one with the worst seam (ties: the lower id) and check again. One at a
    # time, because a bad tile also fails the seams of its good neighbours.
    pairs = seam_pairs(loc)
    kept = dict(cut)
    while kept and not keep_failed:
        atlas = build_atlas(classic, kept)
        worst: tuple[float, int] | None = None
        for tile_id in sorted(kept):
            ok, seams = check_seams(tile_id, atlas, ref, pairs, max_seam, mode, None if strict else frozenset(kept))
            if not ok:
                score = max(s["excess"] for s in seams if s["decides"] and not s["pass"])
                if worst is None or score > worst[0]:
                    worst = (score, tile_id)
        if worst is None:
            break
        del kept[worst[1]]
    atlas = build_atlas(classic, kept)
    tiles_root = dest / "tiles"
    for tile_id in sorted(cut):
        trial = kept | {tile_id: cut[tile_id]}
        ok, seams = check_seams(
            tile_id, build_atlas(classic, trial), ref, pairs, max_seam, mode, None if strict else frozenset(trial)
        )
        rel = f"{loc.folder}/t{tile_id:02x}.png"
        target = hdpack.contained_path(tiles_root, rel)
        data = encode_png(cut[tile_id])
        provenance.write_bytes_atomic(target, data)
        accepted = tile_id in kept
        result = TileResult(
            tile_id,
            f"tiles/{rel}",
            provenance.sha256_bytes(data),
            accepted,
            seams,
            "" if accepted else "seam check failed; the classic tile stays",
        )
        provenance.write(
            tiles_root,
            rel,
            provenance.Record(
                stage="tileset-tile",
                asset={"file": rel, "id": provenance.asset_id(rel), "tile_id": f"{tile_id:02x}", "accepted": accepted},
                input_file=sources[tile_id][0],
                input_sha256=sources[tile_id][1],
                outputs={rel: result.sha256},
                inputs=[
                    {"role": "redrawn-neighbourhood", "file": sources[tile_id][0], "sha256": sources[tile_id][1]},
                    {"role": "classic-atlas", "file": classic_path.name, "sha256": classic_sha},
                ],
                seed=next(s for t, _, _, s in entries if t == tile_id),
                seed_inputs={
                    "global_seed": plan.get("global_seed"),
                    "workflow_id": plan.get("workflow_id"),
                    "group": next(g for t, _, g, _ in entries if t == tile_id),
                },
                parameters={
                    "max_seam": max_seam,
                    "seam_mode": mode,
                    "seam_reference": reference,
                    "seam_strict": strict,
                    "strip": SEAM_STRIP,
                    "seams": seams,
                },
            ),
        )
        report.tiles[tile_id] = result
        (report.replaced if accepted else report.rejected).append(tile_id)
        log(f"t{tile_id:02x} {'ok' if accepted else 'REJECTED'} {result.sha256[:12]}")
    atlas_root = dest / "atlas"
    atlas_rel = f"locations/{loc.folder}/gfx/landsym.png"
    atlas_data = encode_png(atlas)
    provenance.write_bytes_atomic(hdpack.contained_path(atlas_root, atlas_rel), atlas_data)
    report.atlas, report.atlas_sha256 = atlas_rel, provenance.sha256_bytes(atlas_data)
    provenance.write(
        atlas_root,
        atlas_rel,
        provenance.Record(
            stage="tileset-atlas",
            asset={"file": atlas_rel, "id": provenance.asset_id(atlas_rel), "location": loc.key},
            input_file=classic_path.name,
            input_sha256=classic_sha,
            outputs={atlas_rel: report.atlas_sha256},
            inputs=[
                {"role": "classic-atlas", "file": classic_path.name, "sha256": classic_sha},
                {"role": "export-atlas", "file": "gfx/landsym.png", "sha256": export_sha},
                {"role": "plan", "file": plan_path.name, "sha256": provenance.sha256_file(plan_path)},
            ]
            + [
                {"role": f"tile {t:02x}", "file": report.tiles[t].file, "sha256": report.tiles[t].sha256}
                for t in sorted(kept)
            ],
            seed_scheme="per tile (see the tile sidecars)",
            parameters={
                "replaced": [f"{t:02x}" for t in sorted(kept)],
                "classic_ids": PER_ROW * PER_ROW - len(kept),
                "max_seam": max_seam,
                "seam_mode": mode,
                "seam_reference": reference,
                "seam_strict": strict,
            },
        ),
    )
    provenance.write_bytes_atomic(dest / "seams.json", (json.dumps(report.to_json(), indent=1) + "\n").encode())
    compare_png = compare_sheet(loc, classic, atlas, list(plan.get("districts", [])))
    buf = BytesIO()
    compare_png.save(buf, "PNG")
    provenance.write_bytes_atomic(dest / "compare.png", buf.getvalue())
    return report


def render_map(loc: Location, atlas: U8, box: tuple[int, int, int, int], px: int) -> Image.Image:
    """Tiles x0..x1, y0..y1 of the map drawn with atlas, px texels per tile."""
    x0, y0, x1, y1 = box
    size = atlas.shape[0] // PER_ROW
    img = Image.new("RGB", ((x1 - x0) * px, (y1 - y0) * px))
    cache: dict[int, Image.Image] = {}
    for y in range(y0, y1):
        for x in range(x0, x1):
            t = loc.grid[y][x]
            if t not in cache:
                cache[t] = Image.fromarray(tile_of(atlas, t, size)[..., :3], "RGB").resize(
                    (px, px), Image.Resampling.BOX
                )
            img.paste(cache[t], ((x - x0) * px, (y - y0) * px))
    return img


def district_box(loc: Location, districts: Sequence[str]) -> tuple[int, int, int, int]:
    idx = {loc.districts.index(d) for d in districts if d in loc.districts} or set(range(len(loc.districts)))
    cells = [(c, r) for r, row in enumerate(loc.cells) for c, v in enumerate(row) if v in idx]
    x0 = min(c for c, _ in cells) * loc.cell_w
    y0 = min(r for _, r in cells) * loc.cell_h
    x1 = (max(c for c, _ in cells) + 1) * loc.cell_w
    y1 = (max(r for _, r in cells) + 1) * loc.cell_h
    return x0, y0, x1, y1


def compare_sheet(loc: Location, classic: U8, atlas: U8, districts: Sequence[str], width: int = 1600) -> Image.Image:
    """The planned districts' part of the map, classic atlas above, new below."""
    box = district_box(loc, districts)
    px = max(4, min(atlas.shape[0] // PER_ROW, width // (box[2] - box[0])))
    a, b = render_map(loc, classic, box, px), render_map(loc, atlas, box, px)
    sheet = Image.new("RGB", (a.width, a.height * 2 + 32), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    d.text((4, 2), "classic HD atlas", fill=(230, 230, 230))
    sheet.paste(a, (0, 16))
    d.text((4, a.height + 18), f"location atlas locations/{loc.folder}", fill=(230, 230, 230))
    sheet.paste(b, (0, a.height + 32))
    return sheet


# ---------------------------------------------------------------------- cli


def cmd_plan(args: argparse.Namespace) -> int:
    entries = write_plan(
        Path(args.export),
        Path(args.location),
        Path(args.job),
        args.district,
        Path(args.descriptions) if args.descriptions else None,
        args.scale,
        args.global_seed,
        args.folder,
    )
    groups = Counter(e.group for e in entries)
    uniform = sum(1 for e in entries if e.neighbourhood.source == "uniform")
    print(f"{len(entries)} tiles ({uniform} with a uniform surround), groups: {dict(groups)}", file=sys.stderr)
    return 0


def cmd_assemble(args: argparse.Namespace) -> int:
    report = assemble(
        Path(args.export),
        Path(args.classic),
        Path(args.location),
        Path(args.plan),
        Path(args.redrawn),
        Path(args.dest),
        args.max_seam,
        args.seam_mode,
        args.keep_failed,
        args.seam_reference,
        args.strict_seams,
    )
    print(
        f"{len(report.replaced)} tiles in the atlas, {len(report.rejected)} rejected, {len(report.missing)} missing; "
        f"{report.atlas} {report.atlas_sha256}",
        file=sys.stderr,
    )
    return 1 if report.missing else 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="tileset", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = p.add_subparsers(dest="command", required=True)
    pl = sub.add_parser("plan", help="neighbourhood crops and redraw items (CPU)")
    pl.add_argument("export", help="folder written by plegacy --export-assets (has gfx/landsym.png)")
    pl.add_argument("location", help="the location JSON")
    pl.add_argument("job", help="job folder: writes in/, items.json and plan.json")
    pl.add_argument("--district", action="append", default=[], help="only ids used in this district (repeatable)")
    pl.add_argument("--descriptions", default=None, help='{"tiles": {"<hex id>": "description"}}')
    pl.add_argument("--scale", type=int, default=DEFAULT_SCALE)
    pl.add_argument("--global-seed", type=int, default=DEFAULT_GLOBAL_SEED)
    pl.add_argument("--folder", default=None, help="atlas folder name (default: the location file's folder)")
    pl.set_defaults(func=cmd_plan)
    asm = sub.add_parser("assemble", help="cut, check seams and build the location atlas (CPU)")
    asm.add_argument("export")
    asm.add_argument("classic", help="the classic HD gfx/landsym.png of the pack")
    asm.add_argument("location")
    asm.add_argument("plan", help="plan.json from the plan step")
    asm.add_argument("redrawn", help="redraw.py output for one denoise, e.g. out/redraw/0.60")
    asm.add_argument("dest")
    asm.add_argument("--max-seam", type=float, default=MAX_SEAM)
    asm.add_argument("--seam-mode", choices=("relative", "absolute"), default="relative")
    asm.add_argument(
        "--seam-reference",
        choices=("original", "classic"),
        default="original",
        help="seams are compared with the 1994 art scaled up as it is (default) or with the classic HD atlas",
    )
    asm.add_argument(
        "--strict-seams", action="store_true", help="seams to tiles that stay classic also reject (default: reported)"
    )
    asm.add_argument("--keep-failed", action="store_true", help="keep tiles whose seams fail (review only)")
    asm.set_defaults(func=cmd_assemble)
    return p


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return int(args.func(args))
    except hdpack.HdPackError as exc:
        print("error:", exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
