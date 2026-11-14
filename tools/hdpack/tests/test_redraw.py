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
