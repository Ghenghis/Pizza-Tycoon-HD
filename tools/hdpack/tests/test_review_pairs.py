# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""review_pairs.py: original and redraw side by side, in pages of 64."""

from __future__ import annotations

import json
from pathlib import Path

from PIL import Image

import review_pairs


def _write(path: Path, colour: tuple[int, int, int, int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.new("RGBA", (30, 20), colour).save(path)


def test_pairs_original_left_redraw_right_and_pages(tmp_path: Path) -> None:
    orig, new = tmp_path / "pack", tmp_path / "redraw"
    files = [f"bob/moebel/{i:04d}.png" for i in range(70)]
    for f in files:
        _write(orig / f, (255, 0, 0, 255))
    for f in files[:66]:  # four not redrawn yet: skipped
        _write(new / f, (0, 0, 255, 255))
    items = tmp_path / "items.json"
    items.write_text(json.dumps([{"file": f, "description": "x", "seed": 1} for f in files]))
    assert review_pairs.main([str(orig), str(new), str(items), str(tmp_path / "pairs")]) == 0
    first, second = Image.open(tmp_path / "pairs0.png"), Image.open(tmp_path / "pairs1.png")
    assert not (tmp_path / "pairs2.png").exists()  # 66 pairs: 64 + 2
    left = first.getpixel((3 + 70, 3 + 70))
    right = first.getpixel((review_pairs.CELL + 3 + 70, 3 + 70))
    assert left[0] > 200 and left[2] < 60
    assert right[2] > 200 and right[0] < 60
    assert second.getpixel((2 * (2 * review_pairs.CELL + 8) + 70, 70))[:3] == (40, 40, 46)  # third pair: none


def test_usage_on_wrong_arguments() -> None:
    assert review_pairs.main(["only", "two"]) == 2
