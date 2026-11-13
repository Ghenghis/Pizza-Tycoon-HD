/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/gfx/HdImage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "engine/Fatal.h"

namespace gfx {

namespace {
int current_scale = 1;

// One premultiplied RGBA sample in floating point.
using Px = std::array<float, 4>;

struct Plane {
  int w = 0;
  int h = 0;
  std::vector<Px> px;
  Px& at(int x, int y) { return px[static_cast<std::size_t>(y) * w + x]; }
  [[nodiscard]] const Px& at(int x, int y) const {
    return px[static_cast<std::size_t>(y) * w + x];
  }
};

Plane premultiply(const HdImage& img) {
  Plane p{img.width, img.height, {}};
  p.px.resize(img.pixels.size());
  for (std::size_t i = 0; i < img.pixels.size(); ++i) {
    const uint32_t v = img.pixels[i];
    const float a = static_cast<float>(alpha(v)) / 255.0f;
    p.px[i] = {static_cast<float>(red(v)) * a, static_cast<float>(green(v)) * a,
               static_cast<float>(blue(v)) * a, a};
  }
  return p;
}

uint32_t unpremultiply(const Px& p) {
  const float a = std::clamp(p[3], 0.0f, 1.0f);
  if (a <= 0.0f) {
    return 0;
  }
  auto ch = [a](float v) {
    return static_cast<uint32_t>(std::lround(std::clamp(v / a, 0.0f, 255.0f)));
  };
  return pack_rgba(ch(p[0]), ch(p[1]), ch(p[2]),
                   static_cast<uint32_t>(std::lround(a * 255.0f)));
}

// Weights for one output sample: which source indices and how much of each.
struct Tap {
  int index;
  float weight;
};

std::vector<std::vector<Tap>> make_taps(int src_len, int dst_len) {
  std::vector<std::vector<Tap>> taps(static_cast<std::size_t>(dst_len));
  const double ratio = static_cast<double>(src_len) / dst_len;
  for (int d = 0; d < dst_len; ++d) {
    auto& t = taps[static_cast<std::size_t>(d)];
    if (dst_len < src_len) {
      // Exact box filter: each output covers [d*ratio, (d+1)*ratio).
      const double start = d * ratio;
      const double end = (d + 1) * ratio;
      double total = 0.0;
      for (int s = static_cast<int>(std::floor(start));
           s < static_cast<int>(std::ceil(end)) && s < src_len; ++s) {
        const double cover =
            std::min<double>(end, s + 1) - std::max<double>(start, s);
        if (cover > 0.0) {
          t.push_back({s, static_cast<float>(cover)});
          total += cover;
        }
      }
      for (auto& tap : t) {
        tap.weight = static_cast<float>(tap.weight / total);
      }
    }
    else {
      // Bilinear with pixel centres aligned.
      const double centre = (d + 0.5) * ratio - 0.5;
      const int s0 =
          std::clamp(static_cast<int>(std::floor(centre)), 0, src_len - 1);
      const int s1 = std::min(s0 + 1, src_len - 1);
      const float f =
          static_cast<float>(std::clamp(centre - std::floor(centre), 0.0, 1.0));
      if (s0 == s1 || centre < 0.0) {
        t.push_back({s0, 1.0f});
      }
      else {
        t.push_back({s0, 1.0f - f});
        t.push_back({s1, f});
      }
    }
  }
  return taps;
}

Plane resize_x(const Plane& src, int w) {
  Plane out{w, src.h, {}};
  out.px.resize(static_cast<std::size_t>(w) * src.h);
  const auto taps = make_taps(src.w, w);
  for (int y = 0; y < src.h; ++y) {
    for (int x = 0; x < w; ++x) {
      Px acc{0, 0, 0, 0};
      for (const Tap& t : taps[static_cast<std::size_t>(x)]) {
        const Px& s = src.at(t.index, y);
        for (int c = 0; c < 4; ++c) {
          acc[c] += s[c] * t.weight;
        }
      }
      out.at(x, y) = acc;
    }
  }
  return out;
}

Plane resize_y(const Plane& src, int h) {
  Plane out{src.w, h, {}};
  out.px.resize(static_cast<std::size_t>(src.w) * h);
  const auto taps = make_taps(src.h, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < src.w; ++x) {
      Px acc{0, 0, 0, 0};
      for (const Tap& t : taps[static_cast<std::size_t>(y)]) {
        const Px& s = src.at(x, t.index);
        for (int c = 0; c < 4; ++c) {
          acc[c] += s[c] * t.weight;
        }
      }
      out.at(x, y) = acc;
    }
  }
  return out;
}

} // namespace

HdImage::HdImage(int w, int h, uint32_t fill)
    : width(w), height(h),
      pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), fill) {
  if (w <= 0 || h <= 0) {
    fatal("HdImage: dimensions must be positive");
  }
}

void hd_scale(int scale) {
  if (scale < 1 || scale > max_hd_scale) {
    fatal("hd_scale must be between 1 and 8");
  }
  current_scale = scale;
}

int hd_scale() noexcept { return current_scale; }

uint32_t blend_over(uint32_t src, uint32_t dst) noexcept {
  const uint32_t sa = alpha(src);
  if (sa == 255) {
    return src;
  }
  if (sa == 0) {
    return dst;
  }
  const uint32_t da = alpha(dst);
  if (da == 0) {
    return src;
  }
  // out_a = sa + da * (1 - sa), all in 0..255 fixed point.
  const uint32_t inv = 255 - sa;
  const uint32_t out_a = sa + (da * inv + 127) / 255;
  auto ch = [&](uint32_t s, uint32_t d) {
    // (s*sa + d*da*(1-sa)) / out_a
    const uint32_t num = s * sa * 255 + d * da * inv;
    return (num + out_a * 255 / 2) / (out_a * 255);
  };
  return pack_rgba(ch(red(src), red(dst)), ch(green(src), green(dst)),
                   ch(blue(src), blue(dst)), out_a);
}

uint32_t scale_rgb(uint32_t p, float factor) noexcept {
  if (factor >= 1.0f) {
    return p;
  }
  const float f = std::max(factor, 0.0f);
  auto ch = [f](uint32_t v) {
    return static_cast<uint32_t>(static_cast<float>(v) * f);
  };
  return pack_rgba(ch(red(p)), ch(green(p)), ch(blue(p)), alpha(p));
}

HdImage resample(const HdImage& src, int width, int height) {
  if (width <= 0 || height <= 0) {
    fatal("resample: target dimensions must be positive");
  }
  if (src.width == width && src.height == height) {
    return src;
  }
  Plane p = premultiply(src);
  // Shrinking first along the axis that shrinks most keeps the work small.
  if (width != src.width) {
    p = resize_x(p, width);
  }
  if (height != src.height) {
    p = resize_y(p, height);
  }
  HdImage out(width, height);
  for (std::size_t i = 0; i < out.pixels.size(); ++i) {
    out.pixels[i] = unpremultiply(p.px[i]);
  }
  return out;
}

} // namespace gfx
