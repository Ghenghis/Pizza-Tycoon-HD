# SPDX-FileCopyrightText: 2020 Morgan McGuire & Mara Gagiu
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: MIT
"""MMPX pixel-art upscaling, vectorised with numpy.

The same rules as the engine's src/engine/gfx/Mmpx.cpp (McGuire & Gagiu,
"MMPX Style-Preserving Pixel Art Magnification", JCGT 2021), applied to
RGBA pictures instead of palette indices. Each pass doubles the size; edges
and diagonals stay crisp, no colour is invented, so sprites keep their
look while losing the blocks.
"""

# The neighbour names A..S, J..M and Bl..Hl are the MMPX paper's own, kept so
# the code can be checked against it line by line.
# ruff: noqa: N806, E741, SIM300

from __future__ import annotations

import numpy as np
from numpy.typing import NDArray
from PIL import Image

U32 = NDArray[np.uint32]


def _pack(rgba: NDArray[np.uint8]) -> U32:
    a = rgba.astype(np.uint32)
    packed: U32 = ((a[..., 0] << 24) | (a[..., 1] << 16) | (a[..., 2] << 8) | a[..., 3]).astype(np.uint32)
    # Every fully transparent pixel is the same "colour", like palette index 0.
    packed[a[..., 3] == 0] = 0
    return packed


def _unpack(p: U32) -> NDArray[np.uint8]:
    out = np.empty(p.shape + (4,), dtype=np.uint8)
    out[..., 0] = (p >> 24) & 0xFF
    out[..., 1] = (p >> 16) & 0xFF
    out[..., 2] = (p >> 8) & 0xFF
    out[..., 3] = p & 0xFF
    return out


def _luma(p: U32) -> U32:
    r = (p >> 24) & 0xFF
    g = (p >> 16) & 0xFF
    b = (p >> 8) & 0xFF
    luma: U32 = (r + g + b + 1).astype(np.uint32)
    # Transparent weighs 256 times its colour, as MMPX weighs alpha.
    return np.where((p & 0xFF) == 0, luma * 256, luma).astype(np.uint32)


def mmpx2(p: U32, blank_edges: bool) -> U32:
    """One MMPX pass on packed RGBA (0xRRGGBBAA): returns twice the size."""
    h, w = p.shape
    pad = 3
    padded = np.pad(p, pad, mode="constant" if blank_edges else "edge")

    def at(dx: int, dy: int) -> U32:
        view: U32 = padded[pad + dy : pad + dy + h, pad + dx : pad + dx + w]
        return view

    A, B, C = at(-1, -1), at(0, -1), at(1, -1)
    D, E, F = at(-1, 0), at(0, 0), at(1, 0)
    G, H, I = at(-1, 1), at(0, 1), at(1, 1)  # noqa: E741
    P, Q, R, S = at(0, -2), at(-2, 0), at(2, 0), at(0, 2)
    Bl, Dl, El, Fl, Hl = _luma(B), _luma(D), _luma(E), _luma(F), _luma(H)

    def eq(x: U32, *ys: U32) -> NDArray[np.bool_]:
        r: NDArray[np.bool_] = np.equal(x, ys[0])
        for y in ys[1:]:
            r &= np.equal(x, y)
        return r

    def any_eq(x: U32, *ys: U32) -> NDArray[np.bool_]:
        r: NDArray[np.bool_] = np.equal(x, ys[0])
        for y in ys[1:]:
            r |= np.equal(x, y)
        return r

    def none_eq(x: U32, *ys: U32) -> NDArray[np.bool_]:
        return ~any_eq(x, *ys)

    J, K, L, M = E.copy(), E.copy(), E.copy(), E.copy()
    # Pixels inside a flat 3x3 area keep E everywhere; the rules can't fire
    # there anyway except through the checks below, which match the C++.
    busy = ~(eq(E, A, B, C, D) & eq(E, F, G, H, I))

    def put(dst: U32, cond: NDArray[np.bool_], src: U32) -> None:
        m = busy & cond
        dst[m] = src[m]

    # 1:1 slope rules
    put(
        J,
        (D == B)
        & (D != H)
        & (D != F)
        & ((El >= Dl) | (E == A))
        & any_eq(E, A, C, G)
        & ((El < Dl) | (A != D) | (E != P) | (E != Q)),
        D,
    )
    put(
        K,
        (B == F)
        & (B != D)
        & (B != H)
        & ((El >= Bl) | (E == C))
        & any_eq(E, A, C, I)
        & ((El < Bl) | (C != B) | (E != P) | (E != R)),
        B,
    )
    put(
        L,
        (H == D)
        & (H != F)
        & (H != B)
        & ((El >= Hl) | (E == G))
        & any_eq(E, A, G, I)
        & ((El < Hl) | (G != H) | (E != S) | (E != Q)),
        H,
    )
    put(
        M,
        (F == H)
        & (F != B)
        & (F != D)
        & ((El >= Fl) | (E == I))
        & any_eq(E, C, G, I)
        & ((El < Fl) | (I != H) | (E != R) | (E != S)),
        F,
    )

    # Intersection rules
    c = (E != F) & eq(E, C, I, D, Q) & eq(F, B, H) & (F != at(3, 0))
    put(K, c, F)
    put(M, c, F)
    c = (E != D) & eq(E, A, G, F, R) & eq(D, B, H) & (D != at(-3, 0))
    put(J, c, D)
    put(L, c, D)
    c = (E != H) & eq(E, G, I, B, P) & eq(H, D, F) & (H != at(0, 3))
    put(L, c, H)
    put(M, c, H)
    c = (E != B) & eq(E, A, C, H, S) & eq(B, D, F) & (B != at(0, -3))
    put(J, c, B)
    put(K, c, B)
    c = (Bl < El) & eq(E, G, H, I, S) & none_eq(E, A, D, C, F)
    put(J, c, B)
    put(K, c, B)
    c = (Hl < El) & eq(E, A, B, C, P) & none_eq(E, D, G, I, F)
    put(L, c, H)
    put(M, c, H)
    c = (Fl < El) & eq(E, A, D, G, Q) & none_eq(E, B, C, I, H)
    put(K, c, F)
    put(M, c, F)
    c = (Dl < El) & eq(E, C, F, I, R) & none_eq(E, B, A, G, H)
    put(J, c, D)
    put(L, c, D)

    # 2:1 slope rules (each reads the current J/K/L/M, as in the C++)
    hb = H != B
    c1 = hb & (H != A) & (H != E) & (H != C)
    put(L, c1 & eq(H, G, F, R) & none_eq(H, D, at(2, -1)), M.copy())
    put(M, c1 & eq(H, I, D, Q) & none_eq(H, F, at(-2, -1)), L.copy())
    c2 = hb & (B != I) & (B != G) & (B != E)
    put(J, c2 & eq(B, A, F, R) & none_eq(B, D, at(2, 1)), K.copy())
    put(K, c2 & eq(B, C, D, Q) & none_eq(B, F, at(-2, 1)), J.copy())
    fd = F != D
    c3 = fd & (D != I) & (D != E) & (D != C)
    put(J, c3 & eq(D, A, H, S) & none_eq(D, B, at(1, 2)), L.copy())
    put(L, c3 & eq(D, G, B, P) & none_eq(D, H, at(1, -2)), J.copy())
    c4 = fd & (F != E) & (F != A) & (F != G)
    put(K, c4 & eq(F, C, H, S) & none_eq(F, B, at(-1, 2)), M.copy())
    put(M, c4 & eq(F, I, B, P) & none_eq(F, H, at(-1, -2)), K.copy())

    out = np.empty((h * 2, w * 2), dtype=np.uint32)
    out[0::2, 0::2] = J
    out[0::2, 1::2] = K
    out[1::2, 0::2] = L
    out[1::2, 1::2] = M
    return out


def upscale(img: Image.Image, size: tuple[int, int], passes: int = 4) -> Image.Image:
    """MMPX `passes` times (16x for 4), then an alpha-correct area average down
    to `size`, which turns the stair steps into smooth anti-aliased edges."""
    rgba = np.asarray(img.convert("RGBA"))
    p = _pack(rgba)
    blank = bool((rgba[..., 3] == 0).any())
    for _ in range(passes):
        p = mmpx2(p, blank)
    big = Image.fromarray(_unpack(p), "RGBA")
    # Premultiplied area average: transparent pixels never bleed colour in.
    premul = big.convert("RGBa")
    small = premul.resize(size, Image.Resampling.BOX)
    return small.convert("RGBA")
