/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/fx/RendererSelect.h"

#include <string>
#include "engine/Logger.h"
#include "engine/fx/PostFx.h"

namespace fx {

bool display_has_hdr(SDL_Window* window) {
  const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
  if (display == 0) {
    return false;
  }
  return SDL_GetBooleanProperty(SDL_GetDisplayProperties(display),
                                SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
}

bool renderer_hdr(SDL_Renderer* renderer) {
  return SDL_GetNumberProperty(SDL_GetRendererProperties(renderer),
                               SDL_PROP_RENDERER_OUTPUT_COLORSPACE_NUMBER,
                               SDL_COLORSPACE_SRGB) ==
         SDL_COLORSPACE_SRGB_LINEAR;
}

namespace {

SDL_Renderer* make(SDL_Window* window, bool gpu, bool hdr) {
  const SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER,
                         window);
  if (gpu) {
    SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING,
                          SDL_GPU_RENDERER);
    // The post-FX shader ships in all three formats.
    SDL_SetBooleanProperty(
        props, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(
        props, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_DXIL_BOOLEAN, true);
    SDL_SetBooleanProperty(
        props, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
  }
  if (hdr) {
    SDL_SetNumberProperty(props,
                          SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER,
                          SDL_COLORSPACE_SRGB_LINEAR);
  }
  SDL_Renderer* r = SDL_CreateRendererWithProperties(props);
  SDL_DestroyProperties(props);
  if (r && hdr && gpu) {
    // SDL's GPU driver asks for an HDR swapchain without checking it was
    // given one.
    SDL_GPUDevice* device = SDL_GetGPURendererDevice(r);
    if (!device ||
        !SDL_WindowSupportsGPUSwapchainComposition(
            device, window, SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR)) {
      SDL_DestroyRenderer(r);
      SDL_SetError("the GPU swapchain takes no HDR (extended linear)");
      return nullptr;
    }
  }
  return r;
}

} // namespace

SDL_Renderer* create_renderer(SDL_Window* window,
                              const RendererRequest& request) {
  const bool gpu = request.choice == RendererChoice::Gpu;
  bool hdr = request.hdr;
  if (hdr && !display_has_hdr(window)) {
    Logger::info("HDR output off: the display has no HDR turned on");
    hdr = false;
  }
  SDL_Renderer* r = nullptr;
  if (gpu) {
    if (hdr) {
      r = make(window, true, true);
      if (!r) {
        Logger::warn("HDR output with the GPU renderer failed ({}); "
                     "trying without HDR",
                     SDL_GetError());
      }
    }
    if (!r) {
      r = make(window, true, false);
    }
    if (!r) {
      Logger::warn("The GPU renderer failed ({}); using the default renderer",
                   SDL_GetError());
    }
  }
  if (!r && hdr) {
    r = make(window, false, true);
    if (!r) {
      Logger::warn("HDR output failed ({}); trying without HDR",
                   SDL_GetError());
    }
  }
  if (!r) {
    r = make(window, false, false);
  }
  if (r) {
    const char* name = SDL_GetRendererName(r);
    Logger::info("Renderer: {}{}{}", name ? name : "?",
                 PostFx::is_gpu_driver(r) ? " (post-FX shader available)" : "",
                 renderer_hdr(r) ? ", HDR (scRGB)" : "");
  }
  return r;
}

} // namespace fx
