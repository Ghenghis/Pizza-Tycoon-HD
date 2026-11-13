# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixtures: a small export written in the engine's real format, and the
real export of the original game when PLEGACY_TEST_EXPORT_DIR points at one."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


def _write(root: Path, rel: str, img: Image.Image) -> dict:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path)
    opaque = int(np.count_nonzero(np.asarray(img.getchannel("A")) > 0))
    return {
        "width": img.width,
        "height": img.height,
        "opaque_pixels": opaque,
        "transparent_pixels": img.width * img.height - opaque,
    }


@pytest.fixture()
def small_export(tmp_path: Path) -> Path:
    """Two pictures and two sprite frames, laid out like --export-assets."""
    root = tmp_path / "export"
    images = []
    bg = Image.new("RGBA", (20, 10), (200, 120, 40, 255))
    for x in range(20):
        bg.putpixel((x, 5), (10, 10, 200, 255))
    images.append({"kind": "vga", "key": "gfx/back1", "file": "gfx/back1.png", **_write(root, "gfx/back1.png", bg)})
    icons = Image.new("RGBA", (12, 4), (0, 0, 0, 255))
    images.append({"kind": "vga", "key": "gfx/k0", "file": "gfx/k0.png", **_write(root, "gfx/k0.png", icons)})
    for f in range(2):
        spr = Image.new("RGBA", (6, 8), (0, 0, 0, 0))
        for y in range(1, 7):
            for x in range(1, 5):
                spr.putpixel((x, y), (255, 0, 0, 255) if f == 0 else (0, 255, 0, 255))
        file = f"bob/moebel/{f:04d}.png"
        images.append({"kind": "bob", "key": "bob/moebel", "file": file, "frame": f, **_write(root, file, spr)})
    (root / "manifest.json").write_text(json.dumps({"format": 1, "images": images}))
    return root


@pytest.fixture()
def real_export() -> Path:
    env = os.environ.get("PLEGACY_TEST_EXPORT_DIR")
    if not env or not (Path(env) / "manifest.json").exists():
        pytest.skip("set PLEGACY_TEST_EXPORT_DIR to a folder written by plegacy --export-assets")
    return Path(env)
