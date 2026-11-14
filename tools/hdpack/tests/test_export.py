# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import annotations

import json
import os
import struct
from pathlib import Path

import pytest
from PIL import Image, ImageChops

import export


def test_decode_vga_literal_and_runs() -> None:
    # 4x2 picture, threshold 0x80 (stored as 0x81): literal run of 3, then 5 x 9
    # (a run byte means byte - threshold repeats, as in VgaLoader)
    body = bytes([2, 7, 8, 9, 0x80 + 5, 9])
    data = struct.pack("<HHI", 4, 2, len(body)) + body + bytes([0x81])
    w, h, px = export.decode_vga(data)
    assert (w, h) == (4, 2)
    assert list(px) == [7, 8, 9, 9, 9, 9, 9, 9]


def test_decode_vga_rejects_non_pictures() -> None:
    assert export.decode_vga(b"\x00" * 4) is None
    assert export.decode_vga(struct.pack("<HHI", 0, 5, 0) + b"\x01") is None
    assert export.decode_vga(struct.pack("<HHI", 400, 5, 0) + b"\x01") is None


def test_bob_frames_and_bounds() -> None:
    header = struct.pack("<HI", 2, 0) + bytes([2, 1, 0, 0, 0, 0, 0]) + bytes([1, 2, 0, 0, 0, 0, 0])
    frames = export.bob_frames(header + bytes([1, 2, 3, 4]))
    assert frames == [(2, 1, b"\x01\x02"), (1, 2, b"\x03\x04")]
    assert export.bob_frames(header + bytes([1, 2, 3])) is None  # truncated


def test_to_rgba_keys_index_zero() -> None:
    pal = [(i, i, i) for i in range(256)]
    img, opaque = export.to_rgba(2, 1, bytes([0, 5]), pal)
    assert img.getpixel((0, 0))[3] == 0 and img.getpixel((1, 0)) == (5, 5, 5, 255) and opaque == 1


def test_normalize_key_matches_engine() -> None:
    assert export.normalize_key("GFX\\Back1.VGA") == "gfx/back1"
    assert export.normalize_key("./bob/MOEBEL.BOB") == "bob/moebel"


def test_matches_engine_export_pixel_for_pixel(tmp_path: Path) -> None:
    game = os.environ.get("PLEGACY_TEST_GAME_PATH")
    engine = os.environ.get("PLEGACY_TEST_EXPORT_DIR")
    if not game or not engine:
        pytest.skip("set PLEGACY_TEST_GAME_PATH and PLEGACY_TEST_EXPORT_DIR")
    report = export.export(Path(game), tmp_path)
    assert report.skipped == 0
    a = {i["file"]: i for i in json.loads((Path(engine) / "manifest.json").read_text())["images"]}
    b = {i["file"]: i for i in json.loads((tmp_path / "manifest.json").read_text())["images"]}
    assert a == b
    for f in a:
        with Image.open(Path(engine) / f) as x, Image.open(tmp_path / f) as y:
            assert x.size == y.size
            assert ImageChops.difference(x.convert("RGBA"), y.convert("RGBA")).getbbox() is None, f


def test_wait_for_gpu_returns_when_unknown(monkeypatch: pytest.MonkeyPatch) -> None:
    import hdpack

    monkeypatch.setattr(hdpack, "gpu_utilisation", lambda index: None)
    hdpack.wait_for_gpu(0, 70, log=lambda *_: None, poll=0)


def test_wait_for_gpu_waits_while_busy(monkeypatch: pytest.MonkeyPatch) -> None:
    import hdpack

    readings = iter([95, 95, 95, 10, 10, 10])
    monkeypatch.setattr(hdpack, "gpu_utilisation", lambda index: next(readings))
    seen: list[str] = []
    hdpack.wait_for_gpu(0, 70, log=seen.append, poll=0)
    assert seen and "busy" in seen[0]
