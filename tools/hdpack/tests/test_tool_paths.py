# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""redraw.py, review_pairs.py and depthmaps.py: file names from JSON, the
command line or the pack are data, and no read or write may leave the
folders they are joined to, by spelling or through a symlink."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest
from PIL import Image

import depthmaps
import redraw
import review_pairs

HOSTILE = [
    "../outside.png",
    "bob/../../outside.png",
    "/tmp/outside.png",
    "C:/outside.png",
    "C:outside.png",
    "gfx\\..\\..\\outside.png",
    "//server/share/outside.png",
    "gfx/./a.png",
    "gfx//a.png",
    "",
    "gfx/a.png:stream",
    "gfx/con.png",
]


def _png(path: Path, colour: tuple[int, int, int, int] = (200, 150, 100, 255), size: int = 48) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.new("RGBA", (size, size), colour).save(path)


def _symlink(link: Path, target: Path) -> None:
    link.parent.mkdir(parents=True, exist_ok=True)
    try:
        link.symlink_to(target, target_is_directory=target.is_dir())
    except OSError as exc:  # e.g. Windows without the symlink privilege
        pytest.skip(f"cannot make symlinks here: {exc}")


def _items(path: Path, entries: object) -> Path:
    path.write_text(json.dumps(entries))
    return path


# ------------------------------------------------------------------ redraw


@pytest.mark.parametrize("name", HOSTILE)
def test_redraw_refuses_hostile_names_before_any_work(tmp_path: Path, name: str) -> None:
    pack, maps, out = tmp_path / "pack", tmp_path / "maps", tmp_path / "out"
    items = _items(tmp_path / "items.json", [{"file": name, "description": "x"}])
    base = [str(pack), str(maps), str(out), "--from-raw", "--denoise", "0.8"]
    assert redraw.main(base + ["--items-json", str(items)]) == 2
    assert redraw.main(base + ["--item", name, "seat"]) == 2
    assert not out.exists()
    assert not (tmp_path / "outside.png").exists()


@pytest.mark.parametrize(
    "entries",
    [
        {"file": "bob/a.png", "description": "x"},  # not a list
        [["bob/a.png", "x"]],  # an item that is not an object
        [{"file": "bob/a.png"}],  # no description
        [{"file": "bob/a.png", "description": "x", "seed": "many"}],
        [{"file": "bob/a.txt", "description": "x"}],  # not a picture
        [{"description": "x"}],  # no file
    ],
)
def test_redraw_refuses_malformed_item_lists(tmp_path: Path, entries: object) -> None:
    items = _items(tmp_path / "items.json", entries)
    args = [str(tmp_path / "p"), str(tmp_path / "m"), str(tmp_path / "o"), "--from-raw", "--items-json", str(items)]
    assert redraw.main(args) == 2


def test_redraw_items_json_that_is_not_json(tmp_path: Path) -> None:
    bad = tmp_path / "items.json"
    bad.write_text("{not json")
    args = [str(tmp_path / "p"), str(tmp_path / "m"), str(tmp_path / "o"), "--from-raw"]
    assert redraw.main(args + ["--items-json", str(bad)]) == 2
    assert redraw.main(args + ["--items-json", str(tmp_path / "missing.json")]) == 2


def test_redraw_load_items_keeps_safe_names_and_seeds(tmp_path: Path) -> None:
    items = _items(tmp_path / "items.json", [{"file": "bob/b.png", "description": "chair", "seed": 7}])
    got = redraw.load_items([["bob/a.png", "table"]], str(items), 100)
    assert got == [("bob/a.png", "table", 100), ("bob/b.png", "chair", 7)]


def test_redraw_symlinks_cannot_lead_writes_or_reads_outside(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    pack, maps, out = tmp_path / "pack", tmp_path / "maps", tmp_path / "out"
    outside = tmp_path / "outside"
    outside.mkdir()
    secret = tmp_path / "secret.png"
    _png(secret, (1, 2, 3, 255))
    safe, redirected, leaked = "bob/moebel/0001.png", "gfx/back1.png", "z/zb3.png"
    for rel in (safe, redirected):
        _png(pack / rel)
        _png(out / "raw/0.80" / rel, (120, 70, 30, 255))
    # Results for gfx/ would land outside; the z/ sprite is read from outside.
    _symlink(out / "0.80" / "gfx", outside)
    _symlink(pack / leaked, secret)
    _png(out / "raw/0.80" / leaked)
    args = [str(pack), str(maps), str(out), "--from-raw", "--denoise", "0.8"]
    for rel in (safe, redirected, leaked):
        args += ["--item", rel, "x"]
    assert redraw.main(args) == 1
    assert (out / "0.80" / safe).exists()  # the rest is still done
    assert list(outside.iterdir()) == []
    err = capsys.readouterr().err
    assert f"{redirected}:" in err and f"{leaked}:" in err and "leads outside" in err


# ------------------------------------------------------------ review_pairs


@pytest.mark.parametrize("name", HOSTILE)
def test_review_pairs_refuses_hostile_names(tmp_path: Path, name: str) -> None:
    items = _items(tmp_path / "items.json", [{"file": "bob/a.png"}, {"file": name}])
    prefix = tmp_path / "pairs"
    assert review_pairs.main([str(tmp_path / "o"), str(tmp_path / "r"), str(items), str(prefix)]) == 2
    assert not list(tmp_path.glob("pairs*.png"))


@pytest.mark.parametrize("entries", [{"file": "a.png"}, ["a.png"], [{"name": "a.png"}], [{"file": 5}]])
def test_review_pairs_refuses_malformed_item_lists(tmp_path: Path, entries: object) -> None:
    items = _items(tmp_path / "items.json", entries)
    assert review_pairs.main([str(tmp_path / "o"), str(tmp_path / "r"), str(items), str(tmp_path / "p")]) == 2


def test_review_pairs_leaves_out_pairs_that_escape(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    orig, new = tmp_path / "pack", tmp_path / "redraw"
    outside = tmp_path / "outside"
    _png(outside / "0000.png", (9, 9, 9, 255))
    safe, escaping = "bob/moebel/0000.png", "gfx/0000.png"
    for rel in (safe, escaping):
        _png(orig / rel, (255, 0, 0, 255))
    _png(new / safe, (0, 0, 255, 255))
    _symlink(new / "gfx", outside)  # would show a picture from outside
    items = _items(tmp_path / "items.json", [{"file": safe}, {"file": escaping}])
    assert review_pairs.main([str(orig), str(new), str(items), str(tmp_path / "pairs")]) == 1
    assert (tmp_path / "pairs0.png").exists()
    with Image.open(tmp_path / "pairs0.png") as sheet:
        # The second cell is empty: the escaping pair was left out.
        pixel = sheet.getpixel((2 * review_pairs.CELL + 8 + 70, 70))
        assert isinstance(pixel, tuple) and pixel[:3] == (40, 40, 46)
    assert "leads outside" in capsys.readouterr().err


# --------------------------------------------------------------- depthmaps


def test_depthmaps_plans_paths_inside_both_folders(tmp_path: Path) -> None:
    pack, out = tmp_path / "pack", tmp_path / "maps"
    for rel in ("gfx/back1.png", "bob/moebel/0000.png", "bob/moebel/0001.png"):
        _png(pack / rel)
    _png(out / "depth" / "bob/moebel/0000.png")  # done already
    work = depthmaps.plan(pack, out)
    assert work.total == 3 and work.refused == []
    assert [j.rel for j in work.jobs] == ["bob/moebel/0001.png", "gfx/back1.png"]
    job = work.jobs[1]
    assert job.source == (pack / "gfx/back1.png").resolve()
    assert job.depth == (out / "depth/gfx/back1.png").resolve()
    assert job.normal == (out / "normal/gfx/back1.png").resolve()
    assert len(depthmaps.plan(pack, out, force=True).jobs) == 3
    assert [j.rel for j in depthmaps.plan(pack, out, ["gfx/*"]).jobs] == ["gfx/back1.png"]


def test_depthmaps_refuses_symlinks_that_lead_outside(tmp_path: Path) -> None:
    pack, out = tmp_path / "pack", tmp_path / "maps"
    outside = tmp_path / "outside"
    outside.mkdir()
    secret = tmp_path / "secret.png"
    _png(secret)
    _png(pack / "gfx/back1.png")
    _png(pack / "bob/a.png")
    _symlink(pack / "gfx/leak.png", secret)  # a read from outside
    _symlink(out / "normal" / "bob", outside)  # a write to outside
    work = depthmaps.plan(pack, out)
    assert [j.rel for j in work.jobs] == ["gfx/back1.png"]
    assert len(work.refused) == 2
    assert all("leads outside" in r for r in work.refused)


def test_depthmaps_refuses_names_that_are_not_plain(tmp_path: Path) -> None:
    if sys.platform == "win32":
        pytest.skip("Windows cannot hold these names (con.png is the console)")
    pack = tmp_path / "pack"
    try:
        _png(pack / "gfx/a:b.png")
        _png(pack / "gfx/con.png")
    except OSError:
        pytest.skip("this file system cannot hold such names")
    _png(pack / "gfx/fine.png")
    work = depthmaps.plan(pack, tmp_path / "maps")
    assert [j.rel for j in work.jobs] == ["gfx/fine.png"]
    assert len(work.refused) == 2


def test_depthmaps_stops_before_the_model_when_everything_is_refused(tmp_path: Path) -> None:
    pack = tmp_path / "pack"
    secret = tmp_path / "secret.png"
    _png(secret)
    _symlink(pack / "gfx/leak.png", secret)
    # No model is loaded (torch is not needed) and the refusal is an error.
    assert depthmaps.main([str(pack), str(tmp_path / "maps")]) == 1
    (tmp_path / "empty").mkdir()
    assert depthmaps.main([str(tmp_path / "empty"), str(tmp_path / "maps")]) == 0
