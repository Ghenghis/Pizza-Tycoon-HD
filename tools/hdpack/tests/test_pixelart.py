# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""The numpy MMPX must give exactly what the engine's C++ MMPX gives. The
reference below is a line-by-line copy of src/engine/gfx/Mmpx.cpp."""

# The neighbour names A..S, J..M and Bl..Hl are the MMPX paper's own, kept so
# the code can be checked against it line by line.
# ruff: noqa: N806, E741, SIM300

from __future__ import annotations

import numpy as np
import pytest
from PIL import Image

import pixelart


def mmpx_reference(src: list[list[int]], blank: bool) -> list[list[int]]:
    h, w = len(src), len(src[0])

    def luma(v: int) -> int:
        lum = ((v >> 24) & 255) + ((v >> 16) & 255) + ((v >> 8) & 255) + 1
        return lum * 256 if (v & 255) == 0 else lum

    def at(x: int, y: int) -> int:
        if x < 0 or x >= w or y < 0 or y >= h:
            if blank:
                return 0
            x = min(max(x, 0), w - 1)
            y = min(max(y, 0), h - 1)
        return src[y][x]

    out = [[0] * (w * 2) for _ in range(h * 2)]
    for y in range(h):
        for x in range(w):
            A, B, C = at(x - 1, y - 1), at(x, y - 1), at(x + 1, y - 1)
            D, E, F = at(x - 1, y), at(x, y), at(x + 1, y)
            G, H, I = at(x - 1, y + 1), at(x, y + 1), at(x + 1, y + 1)  # noqa: E741
            J = K = L = M = E
            if not (E == A == B == C == D and E == F == G == H == I):
                P, Q, R, S = at(x, y - 2), at(x - 2, y), at(x + 2, y), at(x, y + 2)
                Bl, Dl, El, Fl, Hl = luma(B), luma(D), luma(E), luma(F), luma(H)
                if (
                    (D == B and D != H and D != F)
                    and (El >= Dl or E == A)
                    and E in (A, C, G)
                    and (El < Dl or A != D or E != P or E != Q)
                ):
                    J = D
                if (
                    (B == F and B != D and B != H)
                    and (El >= Bl or E == C)
                    and E in (A, C, I)
                    and (El < Bl or C != B or E != P or E != R)
                ):
                    K = B
                if (
                    (H == D and H != F and H != B)
                    and (El >= Hl or E == G)
                    and E in (A, G, I)
                    and (El < Hl or G != H or E != S or E != Q)
                ):
                    L = H
                if (
                    (F == H and F != B and F != D)
                    and (El >= Fl or E == I)
                    and E in (C, G, I)
                    and (El < Fl or I != H or E != R or E != S)
                ):
                    M = F
                if E != F and E == C == I == D == Q and F == B == H and F != at(x + 3, y):
                    K = M = F
                if E != D and E == A == G == F == R and D == B == H and D != at(x - 3, y):
                    J = L = D
                if E != H and E == G == I == B == P and H == D == F and H != at(x, y + 3):
                    L = M = H
                if E != B and E == A == C == H == S and B == D == F and B != at(x, y - 3):
                    J = K = B
                if Bl < El and E == G == H == I == S and E not in (A, D, C, F):
                    J = K = B
                if Hl < El and E == A == B == C == P and E not in (D, G, I, F):
                    L = M = H
                if Fl < El and E == A == D == G == Q and E not in (B, C, I, H):
                    K = M = F
                if Dl < El and E == C == F == I == R and E not in (B, A, G, H):
                    J = L = D
                if H != B:
                    if H not in (A, E, C):
                        if H == G == F == R and H not in (D, at(x + 2, y - 1)):
                            L = M
                        if H == I == D == Q and H not in (F, at(x - 2, y - 1)):
                            M = L
                    if B not in (I, G, E):
                        if B == A == F == R and B not in (D, at(x + 2, y + 1)):
                            J = K
                        if B == C == D == Q and B not in (F, at(x - 2, y + 1)):
                            K = J
                if F != D:
                    if D not in (I, E, C):
                        if D == A == H == S and D not in (B, at(x + 1, y + 2)):
                            J = L
                        if D == G == B == P and D not in (H, at(x + 1, y - 2)):
                            L = J
                    if F not in (E, A, G):
                        if F == C == H == S and F not in (B, at(x - 1, y + 2)):
                            K = M
                        if F == I == B == P and F not in (H, at(x - 1, y - 2)):
                            M = K
            out[y * 2][x * 2], out[y * 2][x * 2 + 1] = J, K
            out[y * 2 + 1][x * 2], out[y * 2 + 1][x * 2 + 1] = L, M
    return out


@pytest.mark.parametrize("seed", range(12))
@pytest.mark.parametrize("blank", [True, False])
def test_vectorised_mmpx_matches_reference(seed: int, blank: bool) -> None:
    rng = np.random.default_rng(seed)
    colours = np.array([0, 0xFF0000FF, 0x00FF00FF, 0x2020A0FF, 0xF0F0F0FF], dtype=np.uint32)
    # Few colours and blocky shapes, so the slope rules actually fire.
    small = rng.integers(0, len(colours), size=(7, 9))
    img = colours[np.kron(small, np.ones((2, 2), dtype=np.int64))[:13, :15]]
    want = np.array(mmpx_reference(img.tolist(), blank), dtype=np.uint32)
    got = pixelart.mmpx2(img.astype(np.uint32), blank)
    assert got.shape == want.shape
    assert np.array_equal(got, want)


def test_upscale_keeps_outline_and_size() -> None:
    img = Image.new("RGBA", (6, 6), (0, 0, 0, 0))
    for x in range(1, 5):
        for y in range(1, 5):
            img.putpixel((x, y), (200, 40, 40, 255))
    out = pixelart.upscale(img, (72, 72))
    assert out.size == (72, 72)
    assert out.getpixel((0, 0))[3] == 0
    assert out.getpixel((36, 36)) == (200, 40, 40, 255)
    # No colour is invented: inside stays the sprite colour, edges only fade.
    r, g, b, a = out.getpixel((13, 36))
    assert (r, g, b) == (200, 40, 40) or a < 255
