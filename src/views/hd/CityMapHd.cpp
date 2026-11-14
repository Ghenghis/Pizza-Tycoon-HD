/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/hd/CityMapHd.h"
#include "views/hd/PictureCache.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace views::hd {

namespace {

struct Rgb {
  float r, g, b;
};

Rgb rgb(const std::vector<gfx::Color>& colors, uint8_t index) {
  if (index >= colors.size()) {
    return {0, 0, 0};
  }
  const gfx::Color& c = colors[index];
  return {static_cast<float>(c.r), static_cast<float>(c.g),
          static_cast<float>(c.b)};
}

Rgb rgb(gfx::Color c) {
  return {static_cast<float>(c.r), static_cast<float>(c.g),
          static_cast<float>(c.b)};
}

Rgb mix(Rgb a, Rgb b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

Rgb scaled(Rgb c, float f) { return {c.r * f, c.g * f, c.b * f}; }

uint32_t opaque(Rgb c) {
  const auto channel = [](float v) {
    return static_cast<uint32_t>(std::clamp(std::lround(v), 0L, 255L));
  };
  return gfx::pack_rgba(channel(c.r), channel(c.g), channel(c.b), 255);
}

// A well-mixed 32-bit hash (lowbias32).
uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

// Value noise on a lattice of one point per engine pixel, in -1..1.
class Lattice {
public:
  Lattice(int w, int h) : w_(w + 1), h_(h + 1), v_(std::size_t(w_) * h_) {
    for (int y = 0; y < h_; ++y) {
      for (int x = 0; x < w_; ++x) {
        const uint32_t n = hash32(static_cast<uint32_t>(x) * 0x9E3779B1U ^
                                  hash32(static_cast<uint32_t>(y) + 0x51ED27U));
        v_[std::size_t(y) * w_ + x] =
            static_cast<float>(n & 0xFFFF) / 32767.5f - 1.0f;
      }
    }
  }
  // x, y in engine pixels.
  [[nodiscard]] float at(float x, float y) const {
    const int x0 = std::clamp(static_cast<int>(x), 0, w_ - 2);
    const int y0 = std::clamp(static_cast<int>(y), 0, h_ - 2);
    float fx = std::clamp(x - static_cast<float>(x0), 0.0f, 1.0f);
    float fy = std::clamp(y - static_cast<float>(y0), 0.0f, 1.0f);
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    const float* r0 = &v_[std::size_t(y0) * w_ + x0];
    const float* r1 = r0 + w_;
    const float top = r0[0] + (r0[1] - r0[0]) * fx;
    const float bottom = r1[0] + (r1[1] - r1[0]) * fx;
    return top + (bottom - top) * fy;
  }

private:
  int w_, h_;
  std::vector<float> v_;
};

// Texture strength by class: open land and water get a faint grain, roads
// and buildings stay clean.
float texture_amount(MapTileClass c) {
  switch (c) {
  case MapTileClass::Ground:
    return 0.045f;
  case MapTileClass::Water:
    return 0.03f;
  case MapTileClass::Road:
  case MapTileClass::Building:
    return 0.0f;
  }
  return 0.0f;
}

class FarMapPainter {
public:
  FarMapPainter(std::span<const FarMapTile> tiles, int columns, int rows,
                int tile_px, int k, const std::vector<gfx::Color>& colors)
      : tiles_(tiles), columns_(columns), rows_(rows), k_(k), t_(tile_px * k),
        noise_(columns * tile_px, rows * tile_px) {
    for (int i = 0; i < 256; ++i) {
      palette_[static_cast<std::size_t>(i)] =
          rgb(colors, static_cast<uint8_t>(i));
    }
  }

  // Nothing when `stop` was asked for before the last row.
  std::optional<gfx::HdImage> paint(std::stop_token stop = {}) const {
    gfx::HdImage img(columns_ * t_, rows_ * t_);
    for (int j = 0; j < rows_; ++j) {
      if (stop.stop_requested()) {
        return std::nullopt;
      }
      for (int i = 0; i < columns_; ++i) {
        paint_tile(img, i, j);
      }
    }
    return img;
  }

private:
  [[nodiscard]] const FarMapTile& at(int i, int j) const {
    i = std::clamp(i, 0, columns_ - 1);
    j = std::clamp(j, 0, rows_ - 1);
    return tiles_[static_cast<std::size_t>(j) * columns_ + i];
  }
  [[nodiscard]] Rgb color(const FarMapTile& t) const {
    return palette_[t.color];
  }

  // Smooths the edge of class `cls` at one texel: the tile centres around
  // it are corners of a bilinear patch of "is cls" (1) or not (0); the edge
  // is where that patch crosses one half, anti-aliased over one texel. So a
  // staircase of tiles becomes a straight diagonal, and corners round off.
  [[nodiscard]] Rgb smooth(Rgb current, bool own_is_cls,
                           const std::array<const FarMapTile*, 4>& q,
                           const std::array<float, 4>& w, float fu, float fv,
                           MapTileClass cls) const {
    std::array<float, 4> in{};
    int count = 0;
    for (std::size_t n = 0; n < 4; ++n) {
      in[n] = q[n]->cls == cls ? 1.0f : 0.0f;
      count += q[n]->cls == cls ? 1 : 0;
    }
    if (count == 0 || count == 4) {
      return current;
    }
    float f = 0;
    for (std::size_t n = 0; n < 4; ++n) {
      f += w[n] * in[n];
    }
    const float du = (1 - fv) * (in[1] - in[0]) + fv * (in[3] - in[2]);
    const float dv = (1 - fu) * (in[2] - in[0]) + fu * (in[3] - in[1]);
    const float slope = std::sqrt(du * du + dv * dv) / static_cast<float>(t_);
    float alpha = 0.5f;
    if (slope > 1e-6f) {
      alpha = std::clamp((f - 0.5f) / slope + 0.5f, 0.0f, 1.0f);
    }
    else if (f != 0.5f) {
      alpha = f > 0.5f ? 1.0f : 0.0f;
    }
    // The nearest tile of each side gives that side's colour.
    std::size_t best_in = 0;
    std::size_t best_out = 0;
    float w_in = -1;
    float w_out = -1;
    for (std::size_t n = 0; n < 4; ++n) {
      if (in[n] > 0 && w[n] > w_in) {
        w_in = w[n];
        best_in = n;
      }
      if (in[n] == 0 && w[n] > w_out) {
        w_out = w[n];
        best_out = n;
      }
    }
    const Rgb inside = own_is_cls ? current : color(*q[best_in]);
    const Rgb outside = own_is_cls ? color(*q[best_out]) : current;
    return mix(outside, inside, alpha);
  }

  void paint_tile(gfx::HdImage& img, int i, int j) const {
    const FarMapTile& tile = at(i, j);
    bool mixed_water = false;
    bool mixed_road = false;
    for (int dj = -1; dj <= 1; ++dj) {
      for (int di = -1; di <= 1; ++di) {
        const FarMapTile& n = at(i + di, j + dj);
        mixed_water |=
            (n.cls == MapTileClass::Water) != (tile.cls == MapTileClass::Water);
        mixed_road |=
            (n.cls == MapTileClass::Road) != (tile.cls == MapTileClass::Road);
      }
    }
    const bool building = tile.cls == MapTileClass::Building;
    const bool open_l = building && at(i - 1, j).cls != MapTileClass::Building;
    const bool open_r = building && at(i + 1, j).cls != MapTileClass::Building;
    const bool open_t = building && at(i, j - 1).cls != MapTileClass::Building;
    const bool open_b = building && at(i, j + 1).cls != MapTileClass::Building;
    const int bevel = std::max(1, (k_ + 1) / 3);
    const float grain = texture_amount(tile.cls);
    const float inv_t = 1.0f / static_cast<float>(t_);
    const float inv_k = 1.0f / static_cast<float>(k_);
    const Rgb own = color(tile);

    for (int ty = 0; ty < t_; ++ty) {
      const int gy = j * t_ + ty;
      uint32_t* out = img.row(gy) + static_cast<std::size_t>(i) * t_;
      for (int tx = 0; tx < t_; ++tx) {
        const int gx = i * t_ + tx;
        Rgb c = own;
        if (mixed_water || mixed_road) {
          const float u = (static_cast<float>(gx) + 0.5f) * inv_t - 0.5f;
          const float v = (static_cast<float>(gy) + 0.5f) * inv_t - 0.5f;
          const int i0 = static_cast<int>(std::floor(u));
          const int j0 = static_cast<int>(std::floor(v));
          const float fu = u - static_cast<float>(i0);
          const float fv = v - static_cast<float>(j0);
          const std::array<const FarMapTile*, 4> q{&at(i0, j0), &at(i0 + 1, j0),
                                                   &at(i0, j0 + 1),
                                                   &at(i0 + 1, j0 + 1)};
          const std::array<float, 4> w{(1 - fu) * (1 - fv), fu * (1 - fv),
                                       (1 - fu) * fv, fu * fv};
          // A road tile stays road next to water (bridges, quays).
          if (mixed_water && tile.cls != MapTileClass::Road) {
            c = smooth(c, tile.cls == MapTileClass::Water, q, w, fu, fv,
                       MapTileClass::Water);
          }
          if (mixed_road) {
            // Roads are drawn over water (bridges) and land.
            c = smooth(c, tile.cls == MapTileClass::Road, q, w, fu, fv,
                       MapTileClass::Road);
          }
        }
        if (building) {
          // Lit from the top left, like the map's buildings.
          if ((open_b && ty >= t_ - bevel) || (open_r && tx >= t_ - bevel)) {
            c = scaled(c, 0.82f);
          }
          else if ((open_t && ty < bevel) || (open_l && tx < bevel)) {
            c = scaled(c, 1.1f);
          }
        }
        if (grain > 0) {
          const float n = noise_.at((static_cast<float>(gx) + 0.5f) * inv_k,
                                    (static_cast<float>(gy) + 0.5f) * inv_k);
          c = scaled(c, 1.0f + grain * n);
        }
        out[tx] = opaque(c);
      }
    }
  }

  std::span<const FarMapTile> tiles_;
  int columns_;
  int rows_;
  int k_;
  int t_; // texels per tile side
  Lattice noise_;
  std::array<Rgb, 256> palette_{};
};

int mod8(int v) { return ((v % 8) + 8) % 8; }

} // namespace

namespace {

bool far_map_input_fits(int k, std::span<const FarMapTile> tiles, int columns,
                        int rows, int tile_px) {
  return k > 1 && columns > 0 && rows > 0 && tile_px > 0 &&
         tiles.size() == static_cast<std::size_t>(columns) * rows;
}

// Everything the picture depends on: scale, layout, and per tile its class
// and the colour it is drawn in (so a season or ownership change is a new
// key).
uint64_t far_map_key(int k, std::span<const FarMapTile> tiles, int columns,
                     int rows, int tile_px,
                     const std::vector<gfx::Color>& colors) {
  KeyHash key;
  key.add(k);
  key.add(columns);
  key.add(rows);
  key.add(tile_px);
  for (const FarMapTile& t : tiles) {
    key.add(t.color);
    key.add(t.cls);
    const Rgb c = rgb(colors, t.color);
    key.add(c);
  }
  return key.value();
}

std::shared_ptr<const gfx::HdImage>
paint_far_map(uint64_t id, std::span<const FarMapTile> tiles, int columns,
              int rows, int tile_px, int k,
              const std::vector<gfx::Color>& colors, std::stop_token stop) {
  auto painted =
      FarMapPainter(tiles, columns, rows, tile_px, k, colors).paint(stop);
  if (!painted) {
    return nullptr;
  }
  auto img = std::make_shared<const gfx::HdImage>(std::move(*painted));
  prefill_opacity(*img, k);
  return map_picture_cache().add(id, std::move(img));
}

} // namespace

std::shared_ptr<const gfx::HdImage>
far_map_picture(std::span<const FarMapTile> tiles, int columns, int rows,
                int tile_px, const std::vector<gfx::Color>& colors) {
  const int k = gfx::hd_scale();
  if (!far_map_input_fits(k, tiles, columns, rows, tile_px)) {
    return nullptr;
  }
  const uint64_t id = far_map_key(k, tiles, columns, rows, tile_px, colors);
  if (auto hit = map_picture_cache().find(id)) {
    return hit;
  }
  return paint_far_map(id, tiles, columns, rows, tile_px, k, colors, {});
}

std::shared_ptr<const gfx::HdImage>
far_map_picture_soon(PictureJob& job, std::span<const FarMapTile> tiles,
                     int columns, int rows, int tile_px,
                     const std::vector<gfx::Color>& colors) {
  const int k = gfx::hd_scale();
  if (!far_map_input_fits(k, tiles, columns, rows, tile_px)) {
    return nullptr;
  }
  const uint64_t id = far_map_key(k, tiles, columns, rows, tile_px, colors);
  if (auto hit = map_picture_cache().find(id)) {
    job.cancel(); // whatever it was painting is not wanted any more
    return hit;
  }
  // The worker gets copies: the caller's tiles and palette may change or go
  // away while it paints.
  job.start(id,
            [id, columns, rows, tile_px, k,
             own_tiles = std::vector<FarMapTile>(tiles.begin(), tiles.end()),
             own_colors = colors](std::stop_token stop) {
              return paint_far_map(id, own_tiles, columns, rows, tile_px, k,
                                   own_colors, stop);
            });
  return nullptr;
}

bool attach_far_map_picture(gfx::Surface& map,
                            std::shared_ptr<const gfx::HdImage> picture) {
  const int k = gfx::hd_scale();
  if (k <= 1 || !picture || picture->width != map.width() * k ||
      picture->height != map.height() * k) {
    return false;
  }
  map.attach_hd(std::move(picture));
  return true;
}

bool attach_far_map(gfx::Surface& map, std::span<const FarMapTile> tiles,
                    int columns, int rows, int tile_px,
                    const std::vector<gfx::Color>& colors) {
  if (gfx::hd_scale() <= 1 || map.width() != columns * tile_px ||
      map.height() != rows * tile_px) {
    return false;
  }
  return attach_far_map_picture(
      map, far_map_picture(tiles, columns, rows, tile_px, colors));
}

std::shared_ptr<const gfx::HdImage>
hatch_picture(gfx::Color base, gfx::Color line, int size_px, int k) {
  if (k <= 1 || size_px <= 0) {
    return nullptr;
  }
  KeyHash key;
  key.add(base);
  key.add(line);
  key.add(size_px);
  key.add(k);
  const uint64_t id = key.value();
  if (auto hit = hatch_picture_cache().find(id)) {
    return hit;
  }
  const int n = size_px * k;
  gfx::HdImage img(n, n);
  const Rgb b = rgb(base);
  const Rgb l = rgb(line);
  // The indexed pattern: 2x2 dots centred on the lines x + y = 8m. The same
  // ink as a stripe sqrt(2) pixels wide, i.e. |x + y - 8m| < 1.
  const float kf = static_cast<float>(k);
  const float inv_sqrt2 = 0.70710678f;
  for (int ty = 0; ty < n; ++ty) {
    uint32_t* out = img.row(ty);
    const float v = (static_cast<float>(ty) + 0.5f) / kf;
    for (int tx = 0; tx < n; ++tx) {
      const float u = (static_cast<float>(tx) + 0.5f) / kf;
      const float s = u + v;
      const float t = s - 8.0f * std::round(s / 8.0f);
      const float distance = std::fabs(t) * inv_sqrt2; // engine pixels
      const float alpha =
          std::clamp((inv_sqrt2 - distance) * kf + 0.5f, 0.0f, 1.0f);
      out[tx] = opaque(mix(b, l, alpha));
    }
  }
  auto shared = std::make_shared<const gfx::HdImage>(std::move(img));
  prefill_opacity(*shared, k);
  return hatch_picture_cache().add(id, std::move(shared));
}

void hatch_cell(gfx::Surface& map, ViewRect cell, int phase_x, int phase_y,
                gfx::Color base, gfx::Color line) {
  const int k = gfx::hd_scale();
  if (k <= 1 || cell.w <= 0 || cell.h <= 0) {
    return;
  }
  // The stripes depend on x + y only, so both phases fold into x.
  const int src_x = mod8(phase_x + phase_y);
  const int need = std::max(cell.w + src_x, cell.h);
  // Few sizes, so few pictures: far-zoom cells are 16 pixels, medium 64.
  const int size_px = need <= 24 ? 24 : need <= 72 ? 72 : (need + 7) / 8 * 8;
  auto img = hatch_picture(base, line, size_px, k);
  if (!img) {
    return;
  }
  gfx::HdCmd cmd;
  cmd.img = std::move(img);
  cmd.sx = src_x * k;
  cmd.sy = 0;
  cmd.sw = cell.w * k;
  cmd.sh = cell.h * k;
  cmd.dx = cell.x * k;
  cmd.dy = cell.y * k;
  map.hd_overlay(std::move(cmd), cell.x * k, cell.y * k, (cell.x + cell.w) * k,
                 (cell.y + cell.h) * k);
}

} // namespace views::hd
