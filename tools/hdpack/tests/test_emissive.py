# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""The emissive stage (docs/studio/notes/hdpack-maps-layout.md)."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

import emissive
import provenance

PANE = (250, 210, 60)  # yellow window pane: hue 46, s 0.76, v 0.98
LAMP = (240, 240, 236)
WALL = (120, 80, 50)
BRIGHT_NON_KEY = (60, 250, 250)  # bright, but not a key colour
K = 4


def _sprite() -> Image.Image:
    """8x8: wall, a 2x2 pane at (1,1), a lamp texel at (6,6), a bright
    non-key texel at (6,1) and a pane texel at (1,6) the redraw darkened."""
    img = Image.new("RGBA", (8, 8), WALL + (255,))
    for x, y in ((1, 1), (2, 1), (1, 2), (2, 2), (1, 6)):
        img.putpixel((x, y), PANE + (255,))
    img.putpixel((6, 6), LAMP + (255,))
    img.putpixel((6, 1), BRIGHT_NON_KEY + (255,))
    img.putpixel((0, 7), (0, 0, 0, 0))
    return img


def _hd(img: Image.Image) -> Image.Image:
    hd = img.resize((img.width * K, img.height * K), Image.Resampling.NEAREST)
    for y in range(6 * K, 7 * K):  # the redraw moved the pane at (1,6) away
        for x in range(1 * K, 2 * K):
            hd.putpixel((x, y), (40, 30, 20, 255))
    return hd


def _export(root: Path, pictures: dict[str, Image.Image], palette: str = "4") -> Path:
    images = []
    for rel, img in pictures.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        img.save(path)
        images.append({"file": rel, "key": rel[:-4], "palette": int(palette), "width": img.width})
    (root / "manifest.json").write_text(json.dumps({"format": 1, "images": images}))
    return root


def _pack(root: Path, pictures: dict[str, Image.Image]) -> Path:
    for rel, img in pictures.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        img.save(path)
    return root


def _keys(path: Path, palettes: dict[str, dict[str, list[str]]], assets: dict[str, object] | None = None) -> Path:
    path.write_text(json.dumps({"schema": emissive.KEYS_SCHEMA, "palettes": palettes, "assets": assets or {}}))
    return path


@pytest.fixture()
def setup(tmp_path: Path) -> tuple[Path, Path, Path, emissive.Keys]:
    spr = _sprite()
    export = _export(tmp_path / "export", {"gfx/k0.png": spr})
    pack = _pack(tmp_path / "pack", {"gfx/k0.png": _hd(spr)})
    keys = emissive.load_keys(
        _keys(tmp_path / "keys.json", {"4": {"window": [emissive.hex_colour(PANE)], "lamp": ["#f0f0ec"]}})
    )
    return export, pack, tmp_path / "maps", keys


def _expected_area(k: int, radius: int) -> np.ndarray:
    """Kept texels (pane 2x2 and lamp), dilated by the square, then the 3x3
    feather's reach: anything within radius + 1."""
    kept = np.zeros((8 * k, 8 * k), dtype=bool)
    kept[1 * k : 3 * k, 1 * k : 3 * k] = True
    kept[6 * k : 7 * k, 6 * k : 7 * k] = True
    out = np.zeros_like(kept)
    reach = radius + 1
    for y, x in zip(*np.nonzero(kept), strict=True):
        out[max(0, y - reach) : y + reach + 1, max(0, x - reach) : x + reach + 1] = True
    return out


def test_classify_matches_the_spec_table() -> None:
    assert emissive.classify(PANE) == "window"
    assert emissive.classify(LAMP) == "lamp"
    assert emissive.classify((240, 110, 30)) == "flame"
    assert emissive.classify((40, 220, 250)) == "sign"
    assert emissive.classify(WALL) is None


def test_mask_covers_exactly_the_dilated_key_area(setup: tuple[Path, Path, Path, emissive.Keys]) -> None:
    export, pack, maps, keys = setup
    report = emissive.run(export, pack, maps, keys, log=lambda *_: None)
    assert [f for f, _ in report.written] == ["gfx/k0/emissive.png"]
    em = np.asarray(Image.open(maps / "gfx/k0/emissive.png"))
    assert em.shape == (32, 32, 4)
    radius = max(1, round(0.5 * K))
    lit = em[..., 3] > 0
    assert np.array_equal(lit, _expected_area(K, radius))
    # The cores carry the class strengths; the colour is the art's, at full brightness.
    assert em[6, 6, 3] == round(255 * 0.85)
    assert em[26, 26, 3] == 255
    assert em[6, 6, :3].max() == 255
    r, g, b = (int(c) for c in em[6, 6, :3])
    assert r > g > b  # still yellow


def test_bright_non_key_and_moved_key_texels_stay_dark(setup: tuple[Path, Path, Path, emissive.Keys]) -> None:
    export, pack, maps, keys = setup
    emissive.run(export, pack, maps, keys, log=lambda *_: None)
    em = np.asarray(Image.open(maps / "gfx/k0/emissive.png"))
    assert em[1 * K + 1, 6 * K + 1, 3] == 0  # bright cyan, not a key
    assert em[6 * K + 2, 1 * K + 2, 3] == 0  # pane colour in the original, dark in the redraw


def test_two_runs_give_identical_bytes(setup: tuple[Path, Path, Path, emissive.Keys], tmp_path: Path) -> None:
    export, pack, maps, keys = setup
    a = emissive.run(export, pack, maps, keys, log=lambda *_: None)
    b = emissive.run(export, pack, tmp_path / "maps2", keys, log=lambda *_: None)
    assert a.written == b.written
    assert (maps / "gfx/k0/emissive.png").read_bytes() == (tmp_path / "maps2/gfx/k0/emissive.png").read_bytes()


def test_sidecar_and_resume(setup: tuple[Path, Path, Path, emissive.Keys]) -> None:
    export, pack, maps, keys = setup
    emissive.run(export, pack, maps, keys, log=lambda *_: None)
    side = json.loads((maps / "_provenance/gfx/k0/emissive/provenance.json").read_text())
    assert side["stage"] == "emissive"
    assert side["workflow"] is None and side["models"] == [] and side["seed"] is None
    assert side["seed_scheme"] == "none"
    assert [i["role"] for i in side["inputs"]] == ["export", "pack"]
    assert side["inputs"][0]["sha256"] == provenance.sha256_file(export / "gfx/k0.png")
    assert side["inputs"][1]["sha256"] == provenance.sha256_file(pack / "gfx/k0.png")
    assert side["parameters"]["keys_sha256"] == keys.sha256
    assert side["parameters"]["luma_ratio"] == 0.55 and side["parameters"]["radius"] == 2
    report = provenance.check(maps)
    assert report.errors == [] and report.recorded == 1
    again = emissive.run(export, pack, maps, keys, log=lambda *_: None)
    assert again.written == [] and again.unchanged == ["gfx/k0/emissive.png"]
    # A changed HD picture makes the sidecar stale.
    Image.new("RGBA", (32, 32), PANE + (255,)).save(pack / "gfx/k0.png")
    assert [f for f, _ in emissive.run(export, pack, maps, keys, log=lambda *_: None).written] == [
        "gfx/k0/emissive.png"
    ]


def test_no_keys_or_skip_writes_no_file(tmp_path: Path) -> None:
    plain = Image.new("RGBA", (8, 8), WALL + (255,))
    spr = _sprite()
    export = _export(tmp_path / "export", {"gfx/k0.png": plain, "gfx/k1.png": spr})
    pack = _pack(tmp_path / "pack", {"gfx/k0.png": _hd(plain), "gfx/k1.png": _hd(spr)})
    keys = emissive.load_keys(
        _keys(tmp_path / "k.json", {"4": {"window": [emissive.hex_colour(PANE)]}}, {"gfx/k1": {"skip": True}})
    )
    report = emissive.run(export, pack, tmp_path / "maps", keys, log=lambda *_: None)
    assert report.written == []
    assert report.no_keys == ["gfx/k0"] and report.skipped == ["gfx/k1"]
    assert not list((tmp_path / "maps").rglob("*.png"))


def test_plan_proposes_the_pane_and_not_a_common_yellow_wall(tmp_path: Path) -> None:
    wall_yellow = (230, 200, 50)  # classifies as a window colour, but covers 60%
    img = Image.new("RGBA", (10, 10), WALL + (255,))
    for y in range(10):
        for x in range(6):
            img.putpixel((x, y), wall_yellow + (255,))
    img.putpixel((8, 2), PANE + (255,))
    img.putpixel((8, 3), PANE + (255,))
    export = _export(tmp_path / "export", {"gfx/back1.png": img})
    proposal = emissive.propose(export)
    pal = proposal["palettes"]["4"]
    windows = [e["colour"] for e in pal["proposed"]["window"]]
    assert windows == [emissive.hex_colour(PANE)]
    assert pal["proposed"]["window"][0]["examples"] == ["gfx/back1"]
    assert [e["colour"] for e in pal["excluded"]] == [emissive.hex_colour(wall_yellow)]
    sheet = emissive.swatch_sheet(proposal)
    assert sheet.width > 0 and sheet.height > 0


def test_keys_are_validated(tmp_path: Path) -> None:
    with pytest.raises(emissive.EmissiveError, match="cannot read"):
        emissive.load_keys(tmp_path / "missing.json")
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"schema": "other/1"}))
    with pytest.raises(emissive.EmissiveError, match="schema"):
        emissive.load_keys(bad)
    bad.write_text(json.dumps({"schema": emissive.KEYS_SCHEMA, "palettes": {"4": {"glow": ["#ffffff"]}}}))
    with pytest.raises(emissive.EmissiveError, match="unknown class"):
        emissive.load_keys(bad)
    bad.write_text(json.dumps({"schema": emissive.KEYS_SCHEMA, "palettes": {"4": {"lamp": ["white"]}}}))
    with pytest.raises(emissive.EmissiveError, match="#rrggbb"):
        emissive.load_keys(bad)
    bad.write_text(
        json.dumps({"schema": emissive.KEYS_SCHEMA, "palettes": {"4": {"lamp": ["#ffffff"], "sign": ["#FFFFFF"]}}})
    )
    with pytest.raises(emissive.EmissiveError, match="two classes"):
        emissive.load_keys(bad)


def test_wrong_k_is_refused(setup: tuple[Path, Path, Path, emissive.Keys]) -> None:
    export, pack, maps, keys = setup
    report = emissive.run(export, pack, maps, keys, emissive.Params(k=3), log=lambda *_: None)
    assert report.written == [] and len(report.failed) == 1


def test_contact_sheet(setup: tuple[Path, Path, Path, emissive.Keys]) -> None:
    export, pack, maps, keys = setup
    emissive.run(export, pack, maps, keys, log=lambda *_: None)
    sheet = emissive.contact_sheet(pack, maps, cell=64)
    assert sheet.size == (192, 78)


def test_cli_plan_and_run(setup: tuple[Path, Path, Path, emissive.Keys], tmp_path: Path) -> None:
    export, pack, maps, _ = setup
    out = tmp_path / "proposal.json"
    assert emissive.main(["plan", str(export), "--out", str(out), "--swatches", str(tmp_path / "sw.png")]) == 0
    assert json.loads(out.read_text())["schema"] == emissive.PROPOSAL_SCHEMA
    assert emissive.main(["run", str(export), str(pack), str(maps), "--keys", str(tmp_path / "keys.json")]) == 0
    assert (maps / "gfx/k0/emissive.png").is_file()
    assert emissive.main(["run", str(export), str(pack), str(maps), "--keys", str(tmp_path / "nope.json")]) == 2


def test_scopes_key_only_the_matching_pictures(tmp_path: Path) -> None:
    spr = _sprite()
    export = _export(tmp_path / "export", {"bob/moebel/0101.png": spr, "bob/moebel/0200.png": spr})
    pack = _pack(tmp_path / "pack", {"bob/moebel/0101.png": _hd(spr), "bob/moebel/0200.png": _hd(spr)})
    path = tmp_path / "k.json"
    path.write_text(
        json.dumps(
            {
                "schema": emissive.KEYS_SCHEMA,
                "palettes": {},
                "scopes": [
                    {"palette": "4", "only": ["bob/moebel/010[1-4]"], "lamp": [emissive.hex_colour(PANE)]},
                    {"palette": "5", "only": ["bob/moebel/*"], "sign": [emissive.hex_colour(PANE)]},
                ],
                "assets": {"gfx/*": {"skip": True}},
            }
        )
    )
    keys = emissive.load_keys(path)
    assert keys.skipped("gfx/k0") and not keys.skipped("bob/moebel/0101")
    report = emissive.run(export, pack, tmp_path / "maps", keys, log=lambda *_: None)
    assert [f for f, _ in report.written] == ["bob/moebel/0101/emissive.png"]
    assert report.no_keys == ["bob/moebel/0200"]
    em = np.asarray(Image.open(tmp_path / "maps/bob/moebel/0101/emissive.png"))
    assert em[6, 6, 3] == 255  # keyed as a lamp by the scope


def test_a_colour_keyed_twice_for_one_picture_fails_that_picture(tmp_path: Path) -> None:
    spr = _sprite()
    export = _export(tmp_path / "export", {"gfx/k0.png": spr})
    pack = _pack(tmp_path / "pack", {"gfx/k0.png": _hd(spr)})
    path = tmp_path / "k.json"
    pane = emissive.hex_colour(PANE)
    path.write_text(
        json.dumps(
            {
                "schema": emissive.KEYS_SCHEMA,
                "palettes": {"4": {"window": [pane]}},
                "scopes": [{"palette": "4", "only": ["gfx/k0"], "lamp": [pane]}],
            }
        )
    )
    report = emissive.run(export, pack, tmp_path / "maps", emissive.load_keys(path), log=lambda *_: None)
    assert report.written == [] and "keyed as window and lamp" in report.failed[0]
    path.write_text(json.dumps({"schema": emissive.KEYS_SCHEMA, "scopes": [{"palette": "4", "lamp": [pane]}]}))
    with pytest.raises(emissive.EmissiveError, match="only"):
        emissive.load_keys(path)
