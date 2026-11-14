/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "views/hd/WaterCycleHd.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
#include "core/util/Constants.h"
#include "engine/gfx/HdList.h"
#include "views/hd/PictureCache.h"

namespace views::hd {

namespace {

constexpr uint8_t multiplier = BASE_MAP_ZOOM_MULTIPLIER; // CLOSE zoom tiles
constexpr int tile_w = LegacyConstants::BASE_MAP_TILE_WIDTH * multiplier;
constexpr int tile_h = LegacyConstants::BASE_MAP_TILE_HEIGHT * multiplier;
constexpr int max_columns = 16; // slots per picture row

gfx::Color color_at(const std::vector<gfx::Color>& colors, uint8_t index) {
  return index < colors.size() ? colors[index] : gfx::Color{0, 0, 0};
}

bool same(gfx::Color a, gfx::Color b) {
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

// One channel: from the step-0 colour to this step's (see the header).
struct ChannelMap {
  float from;
  float to;
  float gain;
};

ChannelMap channel_map(uint8_t from, uint8_t to) {
  const auto f = static_cast<float>(from);
  const auto t = static_cast<float>(to);
  const float gain = from > 0 ? std::clamp(t / f, 0.5f, 2.0f) : 1.0f;
  return {f, t, gain};
}

struct Recolour {
  ChannelMap r, g, b;
};

Recolour recolour_for(gfx::Color from, gfx::Color to) {
  return {channel_map(from.r, to.r), channel_map(from.g, to.g),
          channel_map(from.b, to.b)};
}

uint32_t apply(const Recolour& m, uint32_t texel) {
  const uint32_t a = gfx::alpha(texel);
  if (a == 0) {
    return 0;
  }
  const auto channel = [](const ChannelMap& c, uint32_t v) {
    const float out = c.to + (static_cast<float>(v) - c.from) * c.gain;
    return static_cast<uint32_t>(std::clamp(std::lround(out), 0L, 255L));
  };
  return gfx::pack_rgba(channel(m.r, gfx::red(texel)),
                        channel(m.g, gfx::green(texel)),
                        channel(m.b, gfx::blue(texel)), a);
}

bool input_fits(const WaterCycleInput& in) {
  if (in.k <= 1 || !in.hd || in.width <= 0 || in.height <= 0 ||
      in.pixels.size() != static_cast<std::size_t>(in.width) * in.height ||
      in.hd->width != in.width * in.k || in.hd->height != in.height * in.k ||
      in.indices.empty() || in.cycles.empty()) {
    return false;
  }
  for (const auto& cycle : in.cycles) {
    if (cycle.size() != in.indices.size()) {
      return false;
    }
  }
  return true;
}

bool tile_inside(const ViewRect& r, const WaterCycleInput& in) {
  return r.x >= 0 && r.y >= 0 && r.x + r.w <= in.width &&
         r.y + r.h <= in.height;
}

} // namespace

int WaterFrames::tile_px() { return tile_w; }

bool WaterFrames::has_step(int step) const {
  return step >= 0 && static_cast<std::size_t>(step) < steps_.size() &&
         steps_[static_cast<std::size_t>(step)] != nullptr;
}

bool WaterFrames::overlay(gfx::Surface& dst, uint8_t type, ViewPoint dest,
                          int step) const {
  const int slot = slot_[type];
  if (slot < 0 || !has_step(step) || gfx::hd_scale() != k_) {
    return false;
  }
  const int tw = tile_w * k_;
  const int th = tile_h * k_;
  gfx::HdCmd cmd;
  cmd.img = steps_[static_cast<std::size_t>(step)];
  cmd.sx = (slot % columns_) * tw;
  cmd.sy = (slot / columns_) * th;
  cmd.sw = tw;
  cmd.sh = th;
  cmd.dx = dest.x * k_;
  cmd.dy = dest.y * k_;
  const int x0 = cmd.dx;
  const int y0 = cmd.dy;
  dst.hd_overlay(std::move(cmd), x0, y0, x0 + tw, y0 + th);
  return true;
}

std::size_t WaterFrames::bytes() const {
  std::set<const gfx::HdImage*> seen;
  std::size_t n = 0;
  for (const auto& img : steps_) {
    if (img && seen.insert(img.get()).second) {
      n += img->pixels.size() * sizeof(uint32_t);
    }
  }
  return n;
}

std::optional<WaterCycleInput>
water_cycle_input(const gfx::Surface& landsym,
                  const std::vector<gfx::Color>& colors,
                  const std::vector<uint8_t>& indices,
                  const std::vector<std::vector<uint8_t>>& cycles) {
  const int k = gfx::hd_scale();
  const gfx::HdList* list = landsym.hd_list();
  if (k <= 1 || !list || !list->is_leaf()) {
    return std::nullopt;
  }
  WaterCycleInput in;
  const auto px = landsym.pixels();
  in.pixels.assign(px.begin(), px.end());
  in.width = landsym.width();
  in.height = landsym.height();
  in.hd = list->cmds().front().img;
  in.colors = colors;
  in.indices = indices;
  in.cycles = cycles;
  in.k = k;
  if (!input_fits(in)) {
    return std::nullopt;
  }
  return in;
}

std::shared_ptr<const WaterFrames> build_water_frames(const WaterCycleInput& in,
                                                      std::stop_token stop) {
  if (!input_fits(in)) {
    return nullptr;
  }
  const int k = in.k;
  // Which cycled entry (if any) each palette index is.
  std::array<int, 256> entry{};
  entry.fill(-1);
  for (std::size_t n = 0; n < in.indices.size(); ++n) {
    entry[in.indices[n]] = static_cast<int>(n);
  }

  std::shared_ptr<WaterFrames> frames(new WaterFrames());
  frames->k_ = k;
  frames->slot_.fill(-1);
  for (int type = 0; type < 256; ++type) {
    const ViewRect r =
        landsym_source_rect(static_cast<uint8_t>(type), multiplier);
    if (!tile_inside(r, in)) {
      continue;
    }
    bool water = false;
    for (int y = r.y; y < r.y + r.h && !water; ++y) {
      const uint8_t* row = &in.pixels[static_cast<std::size_t>(y) * in.width];
      for (int x = r.x; x < r.x + r.w; ++x) {
        if (entry[row[x]] >= 0) {
          water = true;
          break;
        }
      }
    }
    if (water) {
      frames->slot_[static_cast<std::size_t>(type)] =
          static_cast<int16_t>(frames->slots_++);
    }
  }
  if (frames->slots_ == 0) {
    return nullptr;
  }
  frames->columns_ = std::min(frames->slots_, max_columns);
  const int picture_rows =
      (frames->slots_ + frames->columns_ - 1) / frames->columns_;
  const int tw = tile_w * k;
  const int th = tile_h * k;

  // Everything a step's picture depends on but the step's own colours.
  KeyHash base;
  base.add(uint32_t{0x77617472}); // "watr"
  add_picture_identity(base, *in.hd);
  base.add(in.pixels.data(), in.pixels.size());
  base.add(in.width);
  base.add(in.height);
  base.add(k);
  for (const uint8_t index : in.indices) {
    base.add(index);
    base.add(color_at(in.colors, index));
  }

  frames->steps_.resize(in.cycles.size());
  for (std::size_t s = 0; s < in.cycles.size(); ++s) {
    std::vector<Recolour> maps(in.indices.size());
    bool changes = false;
    KeyHash key = base;
    for (std::size_t n = 0; n < in.indices.size(); ++n) {
      const gfx::Color from = color_at(in.colors, in.indices[n]);
      const gfx::Color to = color_at(in.colors, in.cycles[s][n]);
      changes = changes || !same(from, to);
      maps[n] = recolour_for(from, to);
      key.add(to);
    }
    if (!changes) {
      continue; // the art as it is: nothing to draw
    }
    const uint64_t id = key.value();
    if (auto hit = map_picture_cache().find(id)) {
      frames->steps_[s] = std::move(hit);
      continue;
    }
    gfx::HdImage img(frames->columns_ * tw, picture_rows * th);
    for (int type = 0; type < 256; ++type) {
      const int slot = frames->slot_[static_cast<std::size_t>(type)];
      if (slot < 0) {
        continue;
      }
      if (stop.stop_requested()) {
        return nullptr;
      }
      const ViewRect r =
          landsym_source_rect(static_cast<uint8_t>(type), multiplier);
      const int ox = (slot % frames->columns_) * tw;
      const int oy = (slot / frames->columns_) * th;
      for (int py = 0; py < r.h; ++py) {
        const uint8_t* row =
            &in.pixels[static_cast<std::size_t>(r.y + py) * in.width];
        for (int px = 0; px < r.w; ++px) {
          const int n = entry[row[r.x + px]];
          if (n < 0) {
            continue; // not water: transparent, the tile's art shows
          }
          const Recolour& m = maps[static_cast<std::size_t>(n)];
          for (int ty = 0; ty < k; ++ty) {
            const uint32_t* src = in.hd->row((r.y + py) * k + ty) +
                                  static_cast<std::size_t>(r.x + px) * k;
            uint32_t* out = img.row(oy + py * k + ty) + ox +
                            static_cast<std::size_t>(px) * k;
            for (int tx = 0; tx < k; ++tx) {
              out[tx] = apply(m, src[tx]);
            }
          }
        }
      }
    }
    auto shared = std::make_shared<const gfx::HdImage>(std::move(img));
    prefill_opacity(*shared, k);
    frames->steps_[s] = map_picture_cache().add(id, std::move(shared));
  }
  return frames;
}

void start_water_frames(WaterJob& job, WaterCycleInput input) {
  KeyHash key;
  key.add(uint32_t{0x77617431}); // "wat1"
  key.add(input.hd.get());
  key.add(input.width);
  key.add(input.height);
  key.add(input.k);
  for (const uint8_t index : input.indices) {
    key.add(color_at(input.colors, index));
  }
  for (const auto& cycle : input.cycles) {
    key.add(cycle.data(), cycle.size());
  }
  job.start(key.value(), [in = std::move(input)](std::stop_token stop) {
    return build_water_frames(in, stop);
  });
}

} // namespace views::hd
