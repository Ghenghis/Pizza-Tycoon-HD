# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Location tile sets (tileset.py) on synthetic atlases and locations."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

import provenance
import redraw
import tileset

SCALE = 2  # HD texels per original pixel in these tests (the real pack is 12)
T = tileset.TILE * SCALE


def _colour(tile_id: int) -> tuple[int, int, int]:
    return (tile_id * 7) % 256, (tile_id * 53) % 256, (tile_id * 101) % 256


def _orig_atlas() -> np.ndarray:
    """256 tiles, each a flat colour of its own with a 2x2 marker at its top
    left, so geometry mistakes show."""
    a = np.zeros((256, 256, 4), dtype=np.uint8)
    for t in range(256):
        x, y = tileset.tile_origin(t)
        a[y : y + 16, x : x + 16, :3] = _colour(t)
        a[y : y + 2, x : x + 2, :3] = (255, 255, 255)
        a[..., 3] = 255
    return a


def _classic_hd(orig: np.ndarray) -> np.ndarray:
    return np.asarray(Image.fromarray(orig, "RGBA").resize((256 * SCALE, 256 * SCALE), Image.Resampling.NEAREST))


def _location(path: Path, grid: list[list[int]], cells: list[list[int]], districts: list[str]) -> Path:
    h, w = len(grid), len(grid[0])
    doc = {
        "format": "pthd.location",
        "location_key": "test:village",
        "grid": {
            "width": w,
            "height": h,
            "district_cell_columns": len(cells[0]),
            "district_cell_rows": len(cells),
            "tile_encoding": "hex-rows-v1",
        },
        "tiles": ["".join(f"{v:02x}" for v in row) for row in grid],
        "district_cells": ["".join(f"{v:02x}" for v in row) for row in cells],
        "districts": [{"id": d} for d in districts],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc))
    return path


GRASS, ROAD, HOUSE, CHURCH = 0x60, 0x06, 0xAB, 0x8F


def _grid() -> list[list[int]]:
    """8x4: grass everywhere, a road row, two houses, a church on the edge."""
    g = [[GRASS] * 8 for _ in range(4)]
    g[2] = [ROAD] * 8
    g[1][2] = HOUSE
    g[1][5] = HOUSE
    g[0][7] = CHURCH  # corner: never has eight neighbours
    return g


@pytest.fixture()
def job(tmp_path: Path) -> dict[str, Path]:
    export = tmp_path / "export"
    (export / "gfx").mkdir(parents=True)
    orig = _orig_atlas()
    Image.fromarray(orig, "RGBA").save(export / "gfx/landsym.png")
    classic = tmp_path / "classic_landsym.png"
    Image.fromarray(_classic_hd(orig), "RGBA").save(classic)
    loc = _location(tmp_path / "locations/test-village/village.json", _grid(), [[0, 1]], ["d-west", "d-east"])
    return {"export": export, "classic": classic, "loc": loc, "job": tmp_path / "job", "root": tmp_path}


def test_location_and_tile_geometry(job: dict[str, Path]) -> None:
    loc = tileset.load_location(job["loc"])
    assert loc.folder == "test-village" and loc.width == 8 and loc.cell_w == 4
    assert loc.district_at(1, 0) == "d-west" and loc.district_at(6, 3) == "d-east"
    # Engine order: high nibble is the column, low nibble the row.
    assert tileset.tile_origin(0xAB) == (0xA * 16, 0xB * 16)
    assert tileset.tile_origin(0x01, 192) == (0, 192)


def test_crop_geometry_uses_the_real_neighbours(job: dict[str, Path]) -> None:
    loc = tileset.load_location(job["loc"])
    n = tileset.choose_neighbourhood(loc, HOUSE)
    assert n.source == "grid" and n.at == (2, 1) and n.occurrences == 2
    assert n.ids == ((GRASS, GRASS, GRASS), (GRASS, HOUSE, GRASS), (ROAD, ROAD, ROAD))
    orig = _orig_atlas()
    crop = tileset.neighbourhood_original(orig, n.ids)
    for r in range(3):
        for c in range(3):
            block = crop[r * 16 : (r + 1) * 16, c * 16 : (c + 1) * 16]
            assert np.array_equal(block, tileset.tile_of(orig, n.ids[r][c], 16))
    hd = tileset.neighbourhood_hd(orig, n.ids, SCALE)
    assert hd.shape == (3 * T, 3 * T, 4)
    # The centre keeps the house's colour after MMPX.
    assert tuple(hd[T + T // 2, T + T // 2, :3]) == _colour(HOUSE)


def test_uniform_surround_when_never_inside(job: dict[str, Path]) -> None:
    loc = tileset.load_location(job["loc"])
    n = tileset.choose_neighbourhood(loc, CHURCH)
    assert n.source == "uniform" and n.at is None
    assert n.ids == ((CHURCH,) * 3,) * 3


def test_centre_cut() -> None:
    crop = np.arange(9 * 9 * 4, dtype=np.uint32).reshape(9, 9, 4).astype(np.uint8)
    assert np.array_equal(tileset.centre(crop), crop[3:6, 3:6])
    with pytest.raises(tileset.TilesetError):
        tileset.centre(crop[:8, :8])


def test_plan_writes_crops_items_and_group_seeds(job: dict[str, Path]) -> None:
    entries = tileset.write_plan(job["export"], job["loc"], job["job"], scale=SCALE)
    ids = [e.tile_id for e in entries]
    assert ids == sorted({ROAD, GRASS, HOUSE, CHURCH})
    by_id = {e.tile_id: e for e in entries}
    assert by_id[HOUSE].group == "test-village:d-west"  # used once in each: the tie goes to the lower index
    assert by_id[CHURCH].group == "test-village:d-east"
    assert by_id[HOUSE].seed == tileset.group_seed(tileset.DEFAULT_GLOBAL_SEED, "test-village:d-west")
    assert by_id[HOUSE].seed != by_id[CHURCH].seed
    assert by_id[GRASS].seed == by_id[HOUSE].seed  # same group, same seed
    # Same inputs, same seeds and bytes.
    again = tileset.write_plan(job["export"], job["loc"], job["root"] / "job2", scale=SCALE)
    assert [e.seed for e in again] == [e.seed for e in entries]
    for e in entries:
        assert (job["job"] / "in" / e.file).read_bytes() == (job["root"] / "job2/in" / e.file).read_bytes()
    assert provenance.check(job["job"] / "in").errors == []
    side = json.loads((job["job"] / "in/_provenance/tiles/test-village/tab/provenance.json").read_text())
    assert side["stage"] == "tileset-crop" and side["seed_inputs"]["group"] == "test-village:d-west"
    # redraw.py reads the items as they are.
    items = redraw.load_items([], str(job["job"] / "items.json"), 0)
    assert [i[0] for i in items] == [e.file for e in entries]
    plan = json.loads((job["job"] / "plan.json").read_text())
    assert plan["atlas_folder"] == "locations/test-village/gfx/landsym.png"


def test_plan_for_one_district(job: dict[str, Path]) -> None:
    entries = tileset.write_plan(job["export"], job["loc"], job["job"], ["d-east"], scale=SCALE)
    assert CHURCH in [e.tile_id for e in entries]
    with pytest.raises(tileset.TilesetError, match="unknown district"):
        tileset.write_plan(job["export"], job["loc"], job["job"], ["d-north"], scale=SCALE)


def _fake_redraw(job: dict[str, Path], ids: dict[int, tuple[int, int, int] | None]) -> Path:
    """A redraw output: the input crop, with the centre tile shifted to a new
    colour (None: unchanged)."""
    out = job["job"] / "out/redraw/0.60"
    plan = json.loads((job["job"] / "plan.json").read_text())
    for t in plan["tiles"]:
        tid = int(t["tile_id"], 16)
        if tid not in ids:
            continue
        crop = tileset.read_rgba(job["job"] / "in" / t["file"]).copy()
        colour = ids[tid]
        if colour is not None:
            crop[T : 2 * T, T : 2 * T, :3] = colour
        path = out / t["file"]
        path.parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(crop, "RGBA").save(path)
    return out


def test_reassembly_is_byte_exact_for_untouched_ids(job: dict[str, Path]) -> None:
    tileset.write_plan(job["export"], job["loc"], job["job"], scale=SCALE)
    redrawn = _fake_redraw(job, {HOUSE: None})
    dest = job["root"] / "dest"
    report = tileset.assemble(
        job["export"], job["classic"], job["loc"], job["job"] / "plan.json", redrawn, dest, log=lambda *_: None
    )
    assert report.replaced == [HOUSE] and report.missing == sorted([GRASS, ROAD, CHURCH])
    atlas = tileset.read_rgba(dest / "atlas/locations/test-village/gfx/landsym.png")
    classic = tileset.read_rgba(job["classic"])
    assert atlas.shape == classic.shape
    for t in range(256):
        if t != HOUSE:
            assert tileset.tile_of(atlas, t, T).tobytes() == tileset.tile_of(classic, t, T).tobytes()
    crop = tileset.read_rgba(redrawn / "tiles/test-village/tab.png")
    assert np.array_equal(tileset.tile_of(atlas, HOUSE, T), tileset.centre(crop))
    assert provenance.check(dest / "atlas").errors == []
    assert provenance.check(dest / "tiles").errors == []
    seams = json.loads((dest / "seams.json").read_text())
    assert seams["replaced"] == ["ab"] and seams["atlas_sha256"] == provenance.sha256_file(
        dest / "atlas/locations/test-village/gfx/landsym.png"
    )
    assert (dest / "compare.png").is_file()


def test_edge_match() -> None:
    a = np.zeros((8, 8, 4), dtype=np.uint8)
    a[..., :3] = (120, 80, 40)
    assert tileset.edge_match(a, a, "right") == 0.0
    b = a.copy()
    b[..., :3] = (40, 80, 120)
    assert tileset.edge_match(a, b, "right") > 30
    c = a.copy()
    c[:, -2:, :3] = (40, 80, 120)  # a's own last strip already matches b
    assert tileset.edge_match(c, b, "right") == pytest.approx(0.0)
    assert tileset.edge_match(a, b, "below") == tileset.edge_match(a, b, "right")
    with pytest.raises(tileset.TilesetError):
        tileset.edge_match(a, b, "left")


def test_seam_check_rejects_a_tile_that_breaks_its_seams(job: dict[str, Path]) -> None:
    tileset.write_plan(job["export"], job["loc"], job["job"], scale=SCALE)
    # The house stays as it was; the road turns black (a much harder seam
    # against the light grass than the classic one, and it also spoils the
    # house's seam with the road until the road is put back).
    redrawn = _fake_redraw(job, {HOUSE: None, ROAD: (0, 0, 0)})
    dest = job["root"] / "dest"
    report = tileset.assemble(
        job["export"], job["classic"], job["loc"], job["job"] / "plan.json", redrawn, dest, log=lambda *_: None
    )
    assert report.replaced == [HOUSE] and report.rejected == [ROAD]
    atlas = tileset.read_rgba(dest / "atlas/locations/test-village/gfx/landsym.png")
    classic = tileset.read_rgba(job["classic"])
    assert tileset.tile_of(atlas, ROAD, T).tobytes() == tileset.tile_of(classic, ROAD, T).tobytes()
    road = report.tiles[ROAD]
    assert not road.accepted and any(not s["pass"] for s in road.seams)
    # The rejected tile is still written, with its sidecar, for review.
    assert (dest / "tiles/test-village/t06.png").is_file()
    side = json.loads((dest / "tiles/_provenance/test-village/t06/provenance.json").read_text())
    assert side["asset"]["accepted"] is False
    # --keep-failed keeps it (review builds only).
    kept = tileset.assemble(
        job["export"],
        job["classic"],
        job["loc"],
        job["job"] / "plan.json",
        redrawn,
        job["root"] / "dest2",
        keep_failed=True,
        log=lambda *_: None,
    )
    assert sorted(kept.replaced) == sorted([ROAD, HOUSE])


def test_absolute_mode_is_stricter(job: dict[str, Path]) -> None:
    tileset.write_plan(job["export"], job["loc"], job["job"], scale=SCALE)
    redrawn = _fake_redraw(job, {HOUSE: None})
    rel = tileset.assemble(
        job["export"],
        job["classic"],
        job["loc"],
        job["job"] / "plan.json",
        redrawn,
        job["root"] / "a",
        log=lambda *_: None,
    )
    ab = tileset.assemble(
        job["export"],
        job["classic"],
        job["loc"],
        job["job"] / "plan.json",
        redrawn,
        job["root"] / "b",
        mode="absolute",
        log=lambda *_: None,
    )
    strict = tileset.assemble(
        job["export"],
        job["classic"],
        job["loc"],
        job["job"] / "plan.json",
        redrawn,
        job["root"] / "c",
        mode="absolute",
        strict=True,
        log=lambda *_: None,
    )
    assert rel.replaced == [HOUSE]  # unchanged art: no seam got worse
    # Absolute: house against grass is a hard edge in 1994 too. Its
    # neighbours are all classic, so only the strict check rejects it.
    assert ab.replaced == [HOUSE]
    assert any(not s["pass"] and not s["decides"] for s in ab.tiles[HOUSE].seams)
    assert strict.rejected == [HOUSE]


def test_cli(job: dict[str, Path]) -> None:
    assert tileset.main(["plan", str(job["export"]), str(job["loc"]), str(job["job"]), "--scale", str(SCALE)]) == 0
    redrawn = _fake_redraw(job, {HOUSE: None, GRASS: None, ROAD: None, CHURCH: None})
    dest = job["root"] / "dest"
    args = ["assemble", str(job["export"]), str(job["classic"]), str(job["loc"]), str(job["job"] / "plan.json")]
    assert tileset.main([*args, str(redrawn), str(dest)]) == 0
    assert tileset.main(["plan", str(job["export"]), str(job["root"] / "nope.json"), str(job["job"])]) == 2


def test_assemble_refuses_another_export(job: dict[str, Path]) -> None:
    tileset.write_plan(job["export"], job["loc"], job["job"], scale=SCALE)
    redrawn = _fake_redraw(job, {HOUSE: None})
    other = _orig_atlas()
    other[0, 0, :3] = (1, 2, 3)
    Image.fromarray(other, "RGBA").save(job["export"] / "gfx/landsym.png")
    with pytest.raises(tileset.TilesetError, match="not the atlas the plan was made from"):
        tileset.assemble(
            job["export"],
            job["classic"],
            job["loc"],
            job["job"] / "plan.json",
            redrawn,
            job["root"] / "d",
            log=lambda *_: None,
        )
