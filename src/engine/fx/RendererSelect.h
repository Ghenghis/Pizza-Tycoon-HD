/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "engine/SDL_include.h"

#include "engine/fx/Look.h"

namespace fx {

// True when the display the window is on has HDR turned on (Windows HDR,
// macOS EDR): only then is scRGB output worth asking for.
[[nodiscard]] bool display_has_hdr(SDL_Window* window);

struct RendererRequest {
  RendererChoice choice = RendererChoice::Auto;
  // scRGB (SDL_COLORSPACE_SRGB_LINEAR) output, used only when the display
  // has HDR on and the driver supports it (Direct3D 11/12, Metal, and SDL's
  // GPU driver where the swapchain takes HDR_EXTENDED_LINEAR).
  bool hdr = false;
};

// Creates the window's renderer as asked, falling back step by step: no
// HDR, then SDL's own driver choice. Each fallback is logged. Null only when
// SDL can make no renderer at all (SDL_GetError() says why).
[[nodiscard]] SDL_Renderer* create_renderer(SDL_Window* window,
                                            const RendererRequest& request);

// True when the renderer outputs scRGB (HDR).
[[nodiscard]] bool renderer_hdr(SDL_Renderer* renderer);

} // namespace fx
