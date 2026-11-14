# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""The map maths (the model itself needs a GPU and is run by hand)."""

from __future__ import annotations

import numpy as np

import depthmaps


def test_flat_depth_points_straight_out() -> None:
    n = depthmaps.normals_from_depth(np.full((8, 8), 0.5, dtype=np.float32))
    assert (n[..., 0] == 128).all() and (n[..., 1] == 128).all() and (n[..., 2] == 255).all()


def test_slope_tilts_the_normal_away_from_the_near_side() -> None:
    ramp = np.tile(np.linspace(0, 1, 32, dtype=np.float32), (8, 1))  # nearer to the right
    n = depthmaps.normals_from_depth(ramp)
    assert n[4, 16, 0] < 128  # x component points left, away from the rise
    assert abs(int(n[4, 16, 1]) - 128) <= 1


def test_normalise_ignores_transparent_pixels() -> None:
    raw = np.array([[0.0, 10.0, 20.0], [1000.0, 15.0, 12.0]], dtype=np.float32)
    mask = np.array([[False, True, True], [False, True, True]])
    d = depthmaps.normalise(raw, mask)
    assert d[0, 0] == 0.0 and d[1, 0] == 0.0
    assert d[mask].min() >= 0.0 and d[mask].max() <= 1.0
    assert d[0, 2] > d[0, 1]
