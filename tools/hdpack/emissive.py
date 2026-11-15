# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""The emissive stage: glow masks for lit windows, lamps, signs and flames.

Spec: docs/studio/notes/hdpack-maps-layout.md ("Proposed stage: emissive
masks"). CPU only (Pillow and numpy), deterministic, no model, no GPU, so it
needs no broker grant.

  python emissive.py plan  <export> --out keys.proposed.json [--swatches sheet.png]
  python emissive.py run   <export> <pack> <maps> [--keys emissive_keys.json] [--only 'gfx/k*']
  python emissive.py sheet <pack> <maps> --out contact.png [--only ...]

plan counts every palette colour over the exported originals and proposes
the ones that look like light. It only proposes: the curator copies the
colours to keep into emissive_keys.json, and only that file decides.

run writes <maps>/<asset id>/emissive.png (RGBA 8-bit, straight alpha) for
every picture with key texels, and its sidecar under
<maps>/_provenance/<asset id>/emissive/provenance.json, beside the
depth+normal sidecar of the same picture.
"""

from __future__ import annotations

import argparse
import colorsys
import fnmatch
import json
import re
import sys
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
import provenance  # noqa: E402

HERE = Path(__file__).resolve().parent
DEFAULT_KEYS = HERE / "emissive_keys.json"
KEYS_SCHEMA = "plegacy.emissive-keys/1"
PROPOSAL_SCHEMA = "plegacy.emissive-keys-proposal/1"
STAGE = "emissive"
EMISSIVE_NAME = "emissive.png"
PNG_COMPRESS_LEVEL = 9

# Classes in the order a colour is tried: the narrow hue bands first, so a
# yellow pane is a window and not a sign.
CLASSES = ("window", "flame", "lamp", "sign")
STRENGTHS = {"window": 0.85, "lamp": 1.0, "sign": 0.9, "flame": 0.8}
LUMA_RATIO = 0.55
MAX_SHARE = 0.20
EXAMPLES = 3
_HEX = re.compile(r"^#[0-9a-fA-F]{6}$")

U8 = npt.NDArray[np.uint8]
I32 = npt.NDArray[np.int32]
B = npt.NDArray[np.bool_]


class EmissiveError(hdpack.HdPackError):
    """A key file or input the user has to fix."""


# ------------------------------------------------------------------ colours


def hex_colour(rgb: Sequence[int]) -> str:
    return "#{:02x}{:02x}{:02x}".format(*(int(c) for c in rgb[:3]))


def parse_hex(text: str) -> tuple[int, int, int]:
    if not _HEX.match(text):
        raise EmissiveError(f"{text!r} is not a #rrggbb colour")
    return int(text[1:3], 16), int(text[3:5], 16), int(text[5:7], 16)


def classify(rgb: Sequence[int]) -> str | None:
    """The light class a colour looks like (the spec's HSV table), or None."""
    h, s, v = colorsys.rgb_to_hsv(rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0)
    hue = h * 360.0
    if 38.0 <= hue <= 62.0 and s >= 0.45 and v >= 0.70:
        return "window"
    if 10.0 <= hue < 38.0 and s >= 0.55 and v >= 0.75:
        return "flame"
    if s <= 0.20 and v >= 0.92:
        return "lamp"
    if s >= 0.60 and v >= 0.85:
        return "sign"
    return None


def luma(rgb: npt.NDArray[Any]) -> npt.NDArray[np.int32]:
    """Integer Rec. 601 luma times 1000, so every test is exact."""
    r = rgb[..., 0].astype(np.int32)
    g = rgb[..., 1].astype(np.int32)
    b = rgb[..., 2].astype(np.int32)
    return np.asarray(299 * r + 587 * g + 114 * b, dtype=np.int32)


def pack_rgb(rgb: npt.NDArray[Any]) -> I32:
    return np.asarray(
        (rgb[..., 0].astype(np.int32) << 16) | (rgb[..., 1].astype(np.int32) << 8) | rgb[..., 2].astype(np.int32),
        dtype=np.int32,
    )


# ------------------------------------------------------------------- export


@dataclass(frozen=True)
class Picture:
    file: str  # e.g. "gfx/k0.png", the same path in the export and the pack
    key: str
    palette: str


def load_export(export: Path, only: Sequence[str] = ()) -> list[Picture]:
    try:
        data = json.loads((export / "manifest.json").read_text(encoding="utf-8"))
        images = data["images"]
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise EmissiveError(f"{export}: not an asset export ({exc})") from exc
    out: list[Picture] = []
    for img in images:
        file = str(img["file"])
        if only and not any(fnmatch.fnmatch(file, pat) for pat in only):
            continue
        hdpack.check_relative(file)
        out.append(Picture(file, str(img.get("key", provenance.asset_id(file))), str(img.get("palette", "none"))))
    return sorted(out, key=lambda p: p.file)


def read_rgba(path: Path) -> U8:
    with Image.open(path) as im:
        return np.asarray(im.convert("RGBA"), dtype=np.uint8)


# --------------------------------------------------------------------- plan


@dataclass
class _Tally:
    texels: int = 0
    pictures: list[str] = field(default_factory=list)
    picture_texels: int = 0  # opaque texels of the pictures that use the colour


def propose(export: Path, max_share: float = MAX_SHARE, only: Sequence[str] = ()) -> dict[str, Any]:
    """Count every opaque palette colour per palette and propose the light ones."""
    tallies: dict[str, dict[int, _Tally]] = {}
    for pic in load_export(export, only):
        rgba = read_rgba(hdpack.contained_path(export, pic.file))
        opaque = rgba[..., 3] == 255
        total = int(np.count_nonzero(opaque))
        if total == 0:
            continue
        values, counts = np.unique(pack_rgb(rgba[..., :3])[opaque], return_counts=True)
        per = tallies.setdefault(pic.palette, {})
        for value, count in zip(values.tolist(), counts.tolist(), strict=True):
            t = per.setdefault(int(value), _Tally())
            t.texels += int(count)
            t.picture_texels += total
            t.pictures.append(provenance.asset_id(pic.file))
    palettes: dict[str, Any] = {}
    for pid in sorted(tallies, key=_palette_order):
        classes: dict[str, list[dict[str, Any]]] = {c: [] for c in CLASSES}
        excluded: list[dict[str, Any]] = []
        for value, t in sorted(tallies[pid].items()):
            rgb = ((value >> 16) & 255, (value >> 8) & 255, value & 255)
            cls = classify(rgb)
            if cls is None:
                continue
            share = t.texels / t.picture_texels
            entry = {
                "colour": hex_colour(rgb),
                "texels": t.texels,
                "share": round(share, 4),
                "pictures": len(t.pictures),
                "examples": t.pictures[:EXAMPLES],
            }
            if share > max_share:
                excluded.append(entry | {"class": cls, "reason": f"covers {share:.0%} of its pictures"})
            else:
                classes[cls].append(entry)
        kept = {c: v for c, v in classes.items() if v}
        if kept or excluded:
            palettes[pid] = {"proposed": kept, "excluded": excluded}
    return {
        "schema": PROPOSAL_SCHEMA,
        "note": "A proposal only. Copy the colours to keep into emissive_keys.json; only that file decides.",
        "parameters": {"max_share": max_share, "classes": list(CLASSES)},
        "palettes": palettes,
    }


def _palette_order(pid: str) -> tuple[int, str]:
    return (int(pid), pid) if pid.isdigit() else (1 << 30, pid)


def swatch_sheet(proposal: Mapping[str, Any]) -> Image.Image:
    """One row per palette: a swatch per proposed colour, class initial below."""
    rows = [(pid, p["proposed"]) for pid, p in proposal["palettes"].items() if p["proposed"]]
    cell, label = 28, 64
    width = label + cell * max([sum(len(v) for v in c.values()) for _, c in rows] + [1])
    sheet = Image.new("RGB", (width, cell * max(1, len(rows)) + 4), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for row, (pid, classes) in enumerate(rows):
        y = row * cell + 2
        draw.text((4, y + 8), f"pal {pid}", fill=(220, 220, 220))
        x = label
        for cls in CLASSES:
            for entry in classes.get(cls, []):
                draw.rectangle((x, y, x + cell - 3, y + cell - 12), fill=parse_hex(entry["colour"]))
                draw.text((x + 8, y + cell - 12), cls[0], fill=(200, 200, 200))
                x += cell
    return sheet


# --------------------------------------------------------------------- keys


@dataclass(frozen=True)
class Scope:
    """Key colours that apply only to the pictures of one palette whose asset
    id matches one of the globs (a palette colour is often a light in one
    sprite and wood or brass in the next)."""

    palette: str
    only: tuple[str, ...]
    table: dict[int, str]
    note: str = ""

    def applies(self, palette: str, aid: str) -> bool:
        return palette == self.palette and any(fnmatch.fnmatchcase(aid, pat) for pat in self.only)


@dataclass(frozen=True)
class Keys:
    palettes: dict[str, dict[int, str]]  # palette id -> packed RGB -> class, for every picture
    skip: frozenset[str]  # asset ids or globs
    sha256: str
    scopes: tuple[Scope, ...] = ()

    def skipped(self, aid: str) -> bool:
        return any(fnmatch.fnmatchcase(aid, pat) for pat in self.skip)

    def table_for(self, palette: str, aid: str) -> dict[int, str]:
        """The keys for one picture: its palette's, plus every matching scope's."""
        table = dict(self.palettes.get(palette, {}))
        for scope in self.scopes:
            if not scope.applies(palette, aid):
                continue
            for value, cls in scope.table.items():
                if table.get(value, cls) != cls:
                    raise EmissiveError(f"{aid}: {hex_colour(_unpack(value))} is keyed as {table[value]} and {cls}")
                table[value] = cls
        return table


def _unpack(value: int) -> tuple[int, int, int]:
    return (value >> 16) & 255, (value >> 8) & 255, value & 255


def _class_table(path: Path, where: str, classes: Mapping[str, Any]) -> dict[int, str]:
    table: dict[int, str] = {}
    for cls, colours in classes.items():
        if cls.startswith("_") or cls in ("palette", "only", "note"):
            continue  # comments and scope fields
        if cls not in STRENGTHS:
            raise EmissiveError(f"{path}: {where}: unknown class {cls!r} (use {', '.join(CLASSES)})")
        if not isinstance(colours, list):
            raise EmissiveError(f"{path}: {where}: {cls} must be a list of colours")
        for text in colours:
            r, g, b = parse_hex(str(text))
            value = (r << 16) | (g << 8) | b
            if table.get(value, cls) != cls:
                raise EmissiveError(f"{path}: {where}: {text} is in two classes")
            table[value] = cls
    return table


def load_keys(path: Path) -> Keys:
    """The key file (schema plegacy.emissive-keys/1):

    palettes: {"<palette id>": {"window": ["#rrggbb", ...], "lamp": [...], ...}}
        keys for every picture drawn with that palette;
    scopes: [{"palette": "<id>", "only": ["bob/moebel/010[1-4]", ...], "lamp": [...], "note": "..."}]
        keys for the matching pictures only (globs on asset ids);
    assets: {"<asset id or glob>": {"skip": true}}
        pictures that never get a mask.
    Keys starting with "_" are comments."""
    try:
        raw = path.read_bytes()
        data = json.loads(raw.decode("utf-8"))
    except (OSError, ValueError) as exc:
        raise EmissiveError(f"{path}: cannot read the key file ({exc})") from exc
    if not isinstance(data, dict) or data.get("schema") != KEYS_SCHEMA:
        raise EmissiveError(f"{path}: expected schema {KEYS_SCHEMA!r}")
    palettes: dict[str, dict[int, str]] = {}
    for pid, classes in (data.get("palettes") or {}).items():
        if str(pid).startswith("_"):
            continue
        if not isinstance(classes, dict):
            raise EmissiveError(f"{path}: palette {pid}: expected an object of classes")
        palettes[str(pid)] = _class_table(path, f"palette {pid}", classes)
    scopes: list[Scope] = []
    for n, raw_scope in enumerate(data.get("scopes") or []):
        if not isinstance(raw_scope, dict) or "palette" not in raw_scope or not raw_scope.get("only"):
            raise EmissiveError(f'{path}: scope {n}: needs "palette" and a non-empty "only" list')
        only = raw_scope["only"]
        if not isinstance(only, list) or not all(isinstance(g, str) for g in only):
            raise EmissiveError(f'{path}: scope {n}: "only" must be a list of globs')
        scopes.append(
            Scope(
                str(raw_scope["palette"]),
                tuple(only),
                _class_table(path, f"scope {n}", raw_scope),
                str(raw_scope.get("note", "")),
            )
        )
    skip = frozenset(
        str(aid) for aid, opts in (data.get("assets") or {}).items() if isinstance(opts, dict) and opts.get("skip")
    )
    return Keys(palettes, skip, provenance.sha256_bytes(raw), tuple(scopes))


# ---------------------------------------------------------------------- run


@dataclass(frozen=True)
class Params:
    luma_ratio: float = LUMA_RATIO
    radius: int | None = None  # None: max(1, round(0.5 * k))
    k: int | None = None  # None: from the size ratio
    strengths: Mapping[str, float] = field(default_factory=lambda: dict(STRENGTHS))
    max_share: float = MAX_SHARE

    def radius_for(self, k: int) -> int:
        return self.radius if self.radius is not None else max(1, round(0.5 * k))


def _shift(a: npt.NDArray[Any], dy: int, dx: int, fill: int = 0) -> npt.NDArray[Any]:
    """a moved by (dy, dx), edges filled."""
    out = np.full_like(a, fill)
    h, w = a.shape[:2]
    ys, yd = (slice(0, h - dy), slice(dy, h)) if dy >= 0 else (slice(-dy, h), slice(0, h + dy))
    xs, xd = (slice(0, w - dx), slice(dx, w)) if dx >= 0 else (slice(-dx, w), slice(0, w + dx))
    out[yd, xd] = a[ys, xs]
    return out


def _dilate(strength: npt.NDArray[np.int32], colour: U8, radius: int) -> tuple[I32, U8]:
    """Grey dilation of strength with a (2r+1)^2 square; each texel takes the
    colour of the source that set its maximum. Offsets are visited nearest
    first in a fixed order and only a strictly larger value wins, so a tie
    goes to the nearest source, the same way every run."""
    best = strength.copy()
    best_col = colour.copy()
    offsets = sorted(
        ((dy, dx) for dy in range(-radius, radius + 1) for dx in range(-radius, radius + 1) if dy or dx),
        key=lambda o: (o[0] * o[0] + o[1] * o[1], o),
    )
    for dy, dx in offsets:
        cand = _shift(strength, dy, dx)
        win = cand > best
        if win.any():
            best[win] = cand[win]
            best_col[win] = _shift(colour, dy, dx)[win]
    return best, best_col


def _box3(a: npt.NDArray[np.int32]) -> npt.NDArray[np.int32]:
    """3x3 box blur, integer, rounded, zero outside."""
    total = np.zeros_like(a)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            total += _shift(a, dy, dx)
    return np.asarray((total + 4) // 9, dtype=np.int32)


def scale_of(orig_shape: tuple[int, ...], hd_shape: tuple[int, ...], k: int | None) -> int:
    h, w = orig_shape[:2]
    hh, hw = hd_shape[:2]
    if k is not None:
        if (hh, hw) != (h * k, w * k):
            raise EmissiveError(f"HD picture is {hw}x{hh}, not {k} x {w}x{h}")
        return k
    return max(1, round(hw / w))


def make_mask(orig: U8, hd: U8, table: Mapping[int, str], params: Params) -> tuple[U8 | None, dict[str, int], int]:
    """The emissive RGBA for one picture (None when it has no key texel),
    the key texels per class in the original and the scale k used."""
    k = scale_of(orig.shape, hd.shape, params.k)
    h, w = orig.shape[:2]
    hh, hw = hd.shape[:2]
    packed = pack_rgb(orig[..., :3])
    opaque = orig[..., 3] == 255
    cls_index = np.zeros((h, w), dtype=np.int32)  # 0 = none, else 1 + CLASSES index
    counts: dict[str, int] = {}
    for value, cls in sorted(table.items()):
        hit = opaque & (packed == value)
        n = int(np.count_nonzero(hit))
        if n:
            cls_index[hit] = 1 + CLASSES.index(cls)
            counts[cls] = counts.get(cls, 0) + n
    if not counts:
        return None, counts, k
    # 1. key mask, nearest up to the HD size (k x k per pixel when exact).
    ys = (np.arange(hh) * h) // hh
    xs = (np.arange(hw) * w) // hw
    hd_cls = cls_index[ys[:, None], xs[None, :]]
    key_luma = luma(orig[..., :3])[ys[:, None], xs[None, :]]
    # 2. follow the HD art: bright enough and visible.
    ratio_milli = round(params.luma_ratio * 1000)
    kept = (hd_cls > 0) & (luma(hd[..., :3]) * 1000 >= key_luma * ratio_milli) & (hd[..., 3] >= 128)
    if not kept.any():
        return None, counts, k
    # 3. strength per class (0..255), dilate, feather.
    lut = np.array([0] + [round(255 * params.strengths[c]) for c in CLASSES], dtype=np.int32)
    strength = np.where(kept, lut[hd_cls], 0).astype(np.int32)
    colour = np.where(kept[..., None], hd[..., :3], 0).astype(np.uint8)
    radius = params.radius_for(k)
    dilated, _ = _dilate(strength, colour, radius)
    # Colour reaches one texel further than the strength, so the feather's
    # fringe carries the light's colour and not black.
    _, spread = _dilate(np.where(kept, 1, 0).astype(np.int32), colour, radius + 1)
    alpha = _box3(dilated)
    # 4. colour scaled so its largest channel is 255.
    peak = spread.max(axis=2).astype(np.int32)
    safe = np.maximum(peak, 1)
    rgb = (spread.astype(np.int32) * 255 + safe[..., None] // 2) // safe[..., None]
    rgb = np.where((peak == 0)[..., None], 255, np.minimum(rgb, 255))
    out = np.zeros((hh, hw, 4), dtype=np.uint8)
    visible = alpha > 0
    out[..., :3] = np.where(visible[..., None], rgb, 0).astype(np.uint8)
    out[..., 3] = np.clip(alpha, 0, 255).astype(np.uint8)
    return out, counts, k


def encode_png(rgba: U8) -> bytes:
    buf = BytesIO()
    Image.fromarray(rgba, "RGBA").save(buf, "PNG", optimize=False, compress_level=PNG_COMPRESS_LEVEL)
    return buf.getvalue()


def out_rel(file: str) -> str:
    return f"{provenance.asset_id(file)}/{EMISSIVE_NAME}"


def _parameters(params: Params, keys: Keys, k: int) -> dict[str, Any]:
    return {
        "k": k,
        "luma_ratio": params.luma_ratio,
        "radius": params.radius_for(k),
        "feather": "box3",
        "strengths": dict(sorted(params.strengths.items())),
        "max_share": params.max_share,
        "keys_sha256": keys.sha256,
        "png_compress_level": PNG_COMPRESS_LEVEL,
    }


def _up_to_date(maps: Path, rel: str, inputs: list[dict[str, str]], parameters: Mapping[str, Any]) -> bool:
    if not provenance.matches(maps, rel, [rel]):
        return False
    try:
        side = json.loads(provenance.sidecar_path(maps, rel).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return bool(side.get("inputs") == inputs and side.get("parameters") == dict(parameters))


@dataclass
class RunReport:
    written: list[tuple[str, str]] = field(default_factory=list)  # rel, sha256
    unchanged: list[str] = field(default_factory=list)
    no_keys: list[str] = field(default_factory=list)
    skipped: list[str] = field(default_factory=list)
    removed: list[str] = field(default_factory=list)
    failed: list[str] = field(default_factory=list)
    classes: dict[str, int] = field(default_factory=dict)

    def to_json(self) -> dict[str, Any]:
        return {
            "written": [{"file": f, "sha256": s} for f, s in self.written],
            "unchanged": self.unchanged,
            "no_keys": len(self.no_keys),
            "skipped": self.skipped,
            "removed": self.removed,
            "failed": self.failed,
            "key_texels_by_class": dict(sorted(self.classes.items())),
        }


def run(
    export: Path,
    pack: Path,
    maps: Path,
    keys: Keys,
    params: Params | None = None,
    only: Sequence[str] = (),
    force: bool = False,
    log: Any = print,
) -> RunReport:
    params = params or Params()
    report = RunReport()
    for pic in load_export(export, only):
        aid = provenance.asset_id(pic.file)
        rel = out_rel(pic.file)
        try:
            target = hdpack.contained_path(maps, rel)
            if keys.skipped(aid):
                report.skipped.append(aid)
                _remove_stale(maps, rel, target, report)
                continue
            table = keys.table_for(pic.palette, aid)
            src = hdpack.contained_path(export, pic.file)
            hd_path = hdpack.contained_path(pack, pic.file)
            if not table:
                report.no_keys.append(aid)
                _remove_stale(maps, rel, target, report)
                continue
            if not hd_path.is_file():
                report.failed.append(f"{pic.file}: not in the pack")
                continue
            orig, hd = read_rgba(src), read_rgba(hd_path)
            rgba, counts, k = make_mask(orig, hd, table, params)
            if rgba is None:
                report.no_keys.append(aid)
                _remove_stale(maps, rel, target, report)
                continue
            for cls, n in counts.items():
                report.classes[cls] = report.classes.get(cls, 0) + n
            inputs = [
                {"role": "export", "file": pic.file, "sha256": provenance.sha256_file(src)},
                {"role": "pack", "file": pic.file, "sha256": provenance.sha256_file(hd_path)},
            ]
            parameters = _parameters(params, keys, k)
            if not force and _up_to_date(maps, rel, inputs, parameters):
                report.unchanged.append(rel)
                continue
            data = encode_png(rgba)
            provenance.write_bytes_atomic(target, data)
            digest = provenance.sha256_bytes(data)
            provenance.write(
                maps,
                rel,
                provenance.Record(
                    stage=STAGE,
                    asset={"file": pic.file, "id": aid, "palette": pic.palette, "key_texels": counts},
                    input_file=pic.file,
                    input_sha256=inputs[1]["sha256"],
                    outputs={rel: digest},
                    inputs=inputs,
                    seed_scheme="none",
                    parameters=parameters,
                ),
            )
            report.written.append((rel, digest))
            log(f"{rel} {digest[:12]} {counts}")
        except (OSError, ValueError, hdpack.HdPackError, provenance.ProvenanceError) as exc:
            report.failed.append(f"{pic.file}: {exc}")
            log(f"{pic.file} FAILED: {exc}")
    return report


def _remove_stale(maps: Path, rel: str, target: Path, report: RunReport) -> None:
    """A picture that no longer gets a mask loses the one this stage made."""
    side = provenance.sidecar_path(maps, rel)
    if target.exists() and side.exists():
        target.unlink()
        side.unlink()
        report.removed.append(rel)


# -------------------------------------------------------------------- sheet


def contact_sheet(pack: Path, maps: Path, only: Sequence[str] = (), cell: int = 256) -> Image.Image:
    """Per emissive mask: the HD picture, the mask on black, and the picture
    with the glow added (a night preview at full strength), side by side."""
    rels = sorted(p.relative_to(maps).as_posix() for p in maps.rglob(EMISSIVE_NAME))
    rels = [r for r in rels if r.split("/", 1)[0] != provenance.FOLDER]
    pics = [r[: -len(EMISSIVE_NAME) - 1] + ".png" for r in rels]
    if only:
        keep = [i for i, f in enumerate(pics) if any(fnmatch.fnmatch(f, pat) for pat in only)]
        rels, pics = [rels[i] for i in keep], [pics[i] for i in keep]
    label = 14
    sheet = Image.new("RGB", (cell * 3, max(1, len(rels)) * (cell + label)), (32, 32, 32))
    draw = ImageDraw.Draw(sheet)
    for row, (rel, file) in enumerate(zip(rels, pics, strict=True)):
        hd = read_rgba(hdpack.contained_path(pack, file)).astype(np.int32)
        em = read_rgba(hdpack.contained_path(maps, rel)).astype(np.int32)
        if em.shape[:2] != hd.shape[:2]:
            em = np.asarray(
                Image.fromarray(em.astype(np.uint8), "RGBA").resize(
                    (hd.shape[1], hd.shape[0]), Image.Resampling.NEAREST
                ),
                dtype=np.int32,
            )
        a = hd[..., 3:4] / 255.0
        base = (hd[..., :3] * a * 0.35).astype(np.int32)  # dimmed, as at night
        glow = (em[..., :3] * (em[..., 3:4] / 255.0) * a).astype(np.int32)
        panels = [
            np.asarray(hd[..., :3] * a, dtype=np.uint8),
            np.asarray(glow, dtype=np.uint8),
            np.asarray(np.clip(base + glow, 0, 255), dtype=np.uint8),
        ]
        y = row * (cell + label)
        draw.text((2, y), file, fill=(230, 230, 230))
        for col, panel in enumerate(panels):
            img = Image.fromarray(panel, "RGB")
            img.thumbnail((cell, cell), Image.Resampling.LANCZOS)
            sheet.paste(img, (col * cell + (cell - img.width) // 2, y + label + (cell - img.height) // 2))
    return sheet


# ---------------------------------------------------------------------- cli


def cmd_plan(args: argparse.Namespace) -> int:
    proposal = propose(Path(args.export), args.max_share, args.only)
    text = json.dumps(proposal, indent=1) + "\n"
    if args.out:
        provenance.write_bytes_atomic(Path(args.out), text.encode("utf-8"))
    else:
        sys.stdout.write(text)
    if args.swatches:
        buf = BytesIO()
        swatch_sheet(proposal).save(buf, "PNG", optimize=False, compress_level=PNG_COMPRESS_LEVEL)
        provenance.write_bytes_atomic(Path(args.swatches), buf.getvalue())
    n = sum(len(v) for p in proposal["palettes"].values() for v in p["proposed"].values())
    x = sum(len(p["excluded"]) for p in proposal["palettes"].values())
    print(f"{len(proposal['palettes'])} palettes, {n} colours proposed, {x} excluded as too common", file=sys.stderr)
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    keys = load_keys(Path(args.keys))
    params = Params(args.luma_ratio, args.radius, args.k, dict(STRENGTHS), args.max_share)
    report = run(Path(args.export), Path(args.pack), Path(args.maps), keys, params, args.only, args.force)
    if args.json:
        print(json.dumps(report.to_json(), indent=1))
    print(
        f"{len(report.written)} written, {len(report.unchanged)} unchanged, {len(report.no_keys)} without keys, "
        f"{len(report.skipped)} skipped, {len(report.removed)} removed, {len(report.failed)} failed",
        file=sys.stderr,
    )
    for f in report.failed:
        print("error:", f, file=sys.stderr)
    return 1 if report.failed else 0


def cmd_sheet(args: argparse.Namespace) -> int:
    buf = BytesIO()
    contact_sheet(Path(args.pack), Path(args.maps), args.only, args.cell).save(buf, "PNG")
    provenance.write_bytes_atomic(Path(args.out), buf.getvalue())
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="emissive", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = p.add_subparsers(dest="command", required=True)

    pl = sub.add_parser("plan", help="propose key colours from the export (writes no masks)")
    pl.add_argument("export", help="folder written by plegacy --export-assets")
    pl.add_argument("--out", default=None, help="proposal JSON (default: stdout)")
    pl.add_argument("--swatches", default=None, help="also write a swatch sheet PNG")
    pl.add_argument("--max-share", type=float, default=MAX_SHARE)
    pl.add_argument("--only", action="append", default=[], help="glob on export paths")
    pl.set_defaults(func=cmd_plan)

    ru = sub.add_parser("run", help="write the emissive masks and their sidecars")
    ru.add_argument("export")
    ru.add_argument("pack", help="HD pack folder (read only)")
    ru.add_argument("maps", help="maps folder (the engine reads <pack>/_maps)")
    ru.add_argument("--keys", default=str(DEFAULT_KEYS))
    ru.add_argument("--k", type=int, default=None, help="scale; default from the size ratio")
    ru.add_argument("--luma-ratio", type=float, default=LUMA_RATIO)
    ru.add_argument("--radius", type=int, default=None, help="dilation radius; default max(1, round(k/2))")
    ru.add_argument("--max-share", type=float, default=MAX_SHARE, help="recorded with the masks")
    ru.add_argument("--only", action="append", default=[], help="glob on export paths, e.g. 'gfx/k*'")
    ru.add_argument("--force", action="store_true")
    ru.add_argument("--json", action="store_true")
    ru.set_defaults(func=cmd_run)

    sh = sub.add_parser("sheet", help="contact sheet of the masks for review")
    sh.add_argument("pack")
    sh.add_argument("maps")
    sh.add_argument("--out", required=True)
    sh.add_argument("--only", action="append", default=[])
    sh.add_argument("--cell", type=int, default=256)
    sh.set_defaults(func=cmd_sheet)
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
