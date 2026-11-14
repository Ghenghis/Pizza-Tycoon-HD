# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""The parts of redraw.py around the GPU step."""

from __future__ import annotations

import numpy as np
from PIL import Image

import redraw


def test_model_size_keeps_aspect_in_multiples_of_8() -> None:
    w, h = redraw.model_size(312, 300)
    assert max(w, h) == 1024 and w % 8 == 0 and h % 8 == 0
    assert abs(w / h - 312 / 300) < 0.02


def test_result_keeps_the_exact_outline() -> None:
    sprite = Image.new("RGBA", (12, 10), (0, 0, 0, 0))
    for x in range(3, 9):
        for y in range(2, 8):
            sprite.putpixel((x, y), (200, 10, 10, 255))
    generated = Image.new("RGB", (1024, 856), (20, 200, 30))
    out = redraw.finish(generated, sprite)
    assert out.size == sprite.size
    assert np.array_equal(np.asarray(out)[..., 3], np.asarray(sprite)[..., 3])
    assert out.getpixel((5, 5))[:3] == (20, 200, 30)


def test_workflow_is_wired_through_the_depth_controlnet() -> None:
    wf = redraw.workflow(
        {
            "checkpoint": "c",
            "controlnet": "d",
            "positive": "p",
            "negative": "n",
            "input_image": "i",
            "depth_image": "dep",
            "seed": 1,
            "denoise": 0.5,
            "control_strength": 0.8,
        }
    )
    assert wf["9"]["inputs"]["positive"] == ["8", 0]
    assert wf["8"]["inputs"]["image"] == ["5", 0]
    assert wf["5"]["inputs"]["image"] == "dep"


def test_baked_in_shadow_is_kept() -> None:
    sprite = Image.new("RGBA", (40, 40), (0, 0, 0, 0))
    for x in range(0, 40):
        for y in range(0, 20):
            sprite.putpixel((x, y), (200, 120, 40, 255))  # the object
        for y in range(20, 40):
            sprite.putpixel((x, y), (48, 48, 48, 255))  # its drop shadow
    out = redraw.finish(Image.new("RGB", (512, 512), (0, 255, 0)), sprite)
    assert out.getpixel((20, 35))[:3] == (48, 48, 48)
    assert out.getpixel((20, 5))[:3] == (0, 255, 0)


def _grid(size: int, block: int) -> Image.Image:
    """Pixel art at its worst: a checkerboard of `block`-sized squares."""
    a = np.zeros((size, size, 4), np.uint8)
    a[..., 3] = 255
    yy, xx = np.mgrid[0:size, 0:size]
    on = ((yy // block + xx // block) % 2).astype(bool)
    a[on, :3] = 220
    a[~on, :3] = 40
    return Image.fromarray(a, "RGBA")


def test_block_size_is_one_original_pixel_in_model_pixels() -> None:
    # A 21x23 sprite at 12x is 252x276; at 1024 long side one pixel is ~44.5.
    assert abs(redraw.block_size((252, 276), (936, 1024), 12) - 1024 / 23) < 1e-6


def test_prepare_softens_the_pixel_grid_but_keeps_the_colours() -> None:
    sprite = _grid(240, 12)  # 20x20 original pixels at 12x
    depth = Image.fromarray(np.asarray(sprite)[..., 0].astype(np.uint16) * 200)
    size = (1024, 1024)
    block = redraw.block_size(sprite.size, size, 12)
    sharp, sharp_depth = redraw.prepare(sprite, depth, size)
    soft, soft_depth = redraw.prepare(sprite, depth, size, block)

    def edges(im: Image.Image) -> float:
        g = np.asarray(im.convert("L"), np.float32)
        return float(np.abs(np.diff(g, axis=1)).max())

    assert edges(soft) < 0.35 * edges(sharp)
    assert edges(soft_depth) < 0.35 * edges(sharp_depth)
    mean_sharp = np.asarray(sharp, np.float32).mean()
    assert abs(np.asarray(soft, np.float32).mean() - mean_sharp) < 3
