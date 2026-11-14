/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/audio/AudioManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <utility>

#include "core/content/Sha256.h"
#include "engine/Logger.h"
#include "engine/SDL_include.h"
#include "engine/audio/EffectPack.h"
#include "engine/audio/VocDecoder.h"
#include "resources/MFileLoader.h"

#if PLEGACY_ENABLE_MUSIC
#include <adlmidi.h>
#endif

namespace audio {

// The callback relies on these never taking a lock (see AudioManager.h).
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::size_t>::is_always_lock_free);
static_assert(std::atomic<MusicState>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);

namespace {

// A request word: play/loop flags and the variant in the low bits, a tag in
// the rest.
constexpr uint32_t PLAY_BIT = 1;
constexpr uint32_t LOOP_BIT = 2;
constexpr uint32_t VARIANT_SHIFT = 2;
constexpr uint32_t VARIANT_MASK = 7;
static_assert(Mixer::MAX_VARIANTS == VARIANT_MASK + 1);
constexpr uint32_t TAG_SHIFT = 5;
constexpr uint32_t TAG_COUNT = UINT32_MAX >> TAG_SHIFT;

constexpr float INT16_TO_FLOAT = 1.0f / 32768.0f;

// Clamps to 0..1; NaN becomes 0.
float unit_range(float v) noexcept {
  return v >= 0.0f ? std::min(v, 1.0f) : 0.0f;
}

constexpr uint64_t route_word(music::Source source, uint32_t generation) {
  return (uint64_t{generation} << 32) | static_cast<uint64_t>(source);
}

constexpr music::Source route_source(uint64_t route) {
  return static_cast<music::Source>(route & 0xFF);
}

constexpr uint32_t route_generation(uint64_t route) {
  return static_cast<uint32_t>(route >> 32);
}

int16_t to_s16(float v) {
  return static_cast<int16_t>(std::lrint(std::clamp(v, -32768.0f, 32767.0f)));
}

// Scales block by a gain that moves in a straight line from gain_from to
// gain_to across its frames, reaching gain_to on the last one.
void scale_block(std::span<int16_t> block, float gain_from, float gain_to) {
  const std::size_t frames = block.size() / MIX_CHANNELS;
  if (frames == 0) {
    return;
  }
  const float step = (gain_to - gain_from) / static_cast<float>(frames);
  for (std::size_t f = 0; f < frames; ++f) {
    const float g = f + 1 == frames
                        ? gain_to
                        : gain_from + step * static_cast<float>(f + 1);
    for (std::size_t c = 0; c < MIX_CHANNELS; ++c) {
      int16_t& s = block[f * MIX_CHANNELS + c];
      s = to_s16(static_cast<float>(s) * g);
    }
  }
}

// The cut of a route change: the last music frame heard fades to zero over
// the first frames of block; the rest is silence.
void declick_block(std::span<int16_t> block,
                   const std::array<int16_t, MIX_CHANNELS>& last,
                   std::size_t fade_frames) {
  std::fill(block.begin(), block.end(), int16_t{0});
  const std::size_t frames = std::min(block.size() / MIX_CHANNELS, fade_frames);
  for (std::size_t f = 0; f < frames; ++f) {
    const float g = static_cast<float>(fade_frames - f - 1) /
                    static_cast<float>(fade_frames);
    for (std::size_t c = 0; c < MIX_CHANNELS; ++c) {
      block[f * MIX_CHANNELS + c] = to_s16(static_cast<float>(last[c]) * g);
    }
  }
}

} // namespace

/* Mixing */

void mix_add(std::span<float> acc, std::span<const int16_t> src,
             float gain_from, float gain_to) {
  const std::size_t n = std::min(acc.size(), src.size());
  if (n == 0) {
    return;
  }
  if (gain_from == gain_to) {
    const float g = gain_to * INT16_TO_FLOAT;
    for (std::size_t i = 0; i < n; ++i) {
      acc[i] += static_cast<float>(src[i]) * g;
    }
    return;
  }
  // Both samples of a frame get the same gain.
  const std::size_t frames = (n + MIX_CHANNELS - 1) / MIX_CHANNELS;
  const float step = (gain_to - gain_from) / static_cast<float>(frames);
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t frame = i / MIX_CHANNELS + 1;
    const float g = frame == frames
                        ? gain_to
                        : gain_from + step * static_cast<float>(frame);
    acc[i] += static_cast<float>(src[i]) * g * INT16_TO_FLOAT;
  }
}

void mix_add_panned(std::span<float> acc, std::span<const int16_t> src,
                    float gain_from, float gain_to, float left, float right) {
  if (left == 1.0f && right == 1.0f) {
    mix_add(acc, src, gain_from, gain_to);
    return;
  }
  const std::size_t n = std::min(acc.size(), src.size());
  if (n == 0) {
    return;
  }
  const std::array<float, MIX_CHANNELS> side{left, right};
  const std::size_t frames = (n + MIX_CHANNELS - 1) / MIX_CHANNELS;
  const float step = (gain_to - gain_from) / static_cast<float>(frames);
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t frame = i / MIX_CHANNELS + 1;
    const float g = gain_from == gain_to || frame == frames
                        ? gain_to
                        : gain_from + step * static_cast<float>(frame);
    acc[i] += static_cast<float>(src[i]) * g * side[i % MIX_CHANNELS] *
              INT16_TO_FLOAT;
  }
}

Placement screen_placement(float x, float y, float width, float height) {
  if (!(width > 0.0f) || !(height > 0.0f) || !std::isfinite(x) ||
      !std::isfinite(y)) {
    return {};
  }
  const float pan = std::clamp((x / width) * 2.0f - 1.0f, -1.0f, 1.0f);
  // How far outside the screen, in screen widths.
  const float dx = std::max({0.0f, -x, x - width});
  const float dy = std::max({0.0f, -y, y - height});
  const float away = std::min(std::sqrt(dx * dx + dy * dy) / width, 1.0f);
  return {.pan = pan * PAN_LIMIT,
          .gain = 1.0f - (1.0f - OFF_SCREEN_GAIN) * away};
}

void mix_to_s16(std::span<const float> acc, std::span<int16_t> out,
                float master) {
  const std::size_t n = std::min(acc.size(), out.size());
  const float k = master * 32768.0f;
  for (std::size_t i = 0; i < n; ++i) {
    const float v = std::clamp(acc[i] * k, -32768.0f, 32767.0f);
    out[i] = static_cast<int16_t>(std::lrint(v));
  }
}

/* Mixer */

std::optional<std::size_t> Mixer::add_effect(std::vector<int16_t> samples,
                                             std::size_t start_offset) {
  const std::lock_guard lock(add_mutex_);
  const std::size_t slot = effect_count_.load(std::memory_order_relaxed);
  if (slot >= MAX_EFFECTS) {
    return std::nullopt;
  }
  samples.resize(samples.size() - samples.size() % MIX_CHANNELS);
  start_offset -= start_offset % MIX_CHANNELS;
  Effect& e = effects_[slot];
  e.start_offset = start_offset < samples.size() ? start_offset : 0;
  e.position = e.start_offset;
  e.samples = std::move(samples);
  // Publishes the slot: the audio thread reads it only after this store.
  effect_count_.store(slot + 1, std::memory_order_release);
  return slot;
}

std::size_t Mixer::effect_count() const noexcept {
  return effect_count_.load(std::memory_order_acquire);
}

bool Mixer::post(std::size_t slot, uint32_t flags) {
  if (slot >= effect_count()) {
    return false;
  }
  // Each request gets its own nonzero tag, so the audio thread clears only
  // the one it applied and never loses a newer one.
  const uint32_t tag =
      (next_request_.fetch_add(1, std::memory_order_relaxed) % TAG_COUNT + 1)
      << TAG_SHIFT;
  effects_[slot].request.store(tag | flags, std::memory_order_release);
  return true;
}

bool Mixer::play(std::size_t slot, bool loop, Placement place) {
  if (slot >= effect_count()) {
    return false;
  }
  const std::lock_guard lock(bank_mutex_);
  // Another variant than last time, all others equally likely.
  std::size_t variant = 0;
  const EffectBank* bank = current_bank_.get();
  if (bank && bank->slots[slot]) {
    const std::size_t n =
        std::min(bank->slots[slot]->variants.size(), MAX_VARIANTS);
    if (n > 1) {
      const std::size_t last = last_variant_[slot] % n;
      variant = random_.below(n - 1);
      if (variant >= last) {
        ++variant;
      }
    }
  }
  last_variant_[slot] = variant;
  const float gain =
      std::isfinite(place.gain) ? std::clamp(place.gain, 0.0f, 1.0f) : 1.0f;
  const float pan =
      std::isfinite(place.pan) ? std::clamp(place.pan, -1.0f, 1.0f) : 0.0f;
  Effect& e = effects_[slot];
  e.place_left.store(gain * (pan > 0.0f ? 1.0f - pan : 1.0f),
                     std::memory_order_relaxed);
  e.place_right.store(gain * (pan < 0.0f ? 1.0f + pan : 1.0f),
                      std::memory_order_relaxed);
  return post(slot, PLAY_BIT | (loop ? LOOP_BIT : 0u) |
                        (static_cast<uint32_t>(variant) << VARIANT_SHIFT));
}

std::size_t Mixer::last_variant(std::size_t slot) const {
  return slot < MAX_EFFECTS ? last_variant_[slot] : 0;
}

void Mixer::seed(uint64_t seed) {
  const std::lock_guard lock(bank_mutex_);
  random_.seed(seed);
}

const Mixer::EffectBank* Mixer::effect_bank() const {
  const std::lock_guard lock(bank_mutex_);
  return current_bank_.get();
}

void Mixer::priority(std::size_t slot, Priority priority) noexcept {
  if (slot < MAX_EFFECTS) {
    effects_[slot].priority.store(static_cast<uint8_t>(priority),
                                  std::memory_order_relaxed);
  }
}

Priority Mixer::priority(std::size_t slot) const noexcept {
  return slot < MAX_EFFECTS
             ? static_cast<Priority>(
                   effects_[slot].priority.load(std::memory_order_relaxed))
             : Priority::AMBIENCE;
}

Priority Mixer::priority_of(std::size_t slot,
                            const EffectBank* bank) const noexcept {
  if (bank && bank->priorities[slot]) {
    return *bank->priorities[slot];
  }
  return priority(slot);
}

void Mixer::voice_limit(std::size_t limit) noexcept {
  voice_limit_.store(std::max<std::size_t>(limit, 1),
                     std::memory_order_relaxed);
}

std::size_t Mixer::voice_limit() const noexcept {
  return voice_limit_.load(std::memory_order_relaxed);
}

void Mixer::ducking(bool enabled) noexcept {
  ducking_.store(enabled, std::memory_order_relaxed);
}

bool Mixer::ducking() const noexcept {
  return ducking_.load(std::memory_order_relaxed);
}

float Mixer::duck_gain() const noexcept {
  return duck_seen_.load(std::memory_order_relaxed);
}

bool Mixer::play_extra(const EffectBank::Sound* sound, Placement place) {
  const std::lock_guard lock(bank_mutex_);
  if (!sound || !current_bank_) {
    return false;
  }
  const std::size_t head = extra_head_.load(std::memory_order_relaxed);
  if (head - extra_tail_.load(std::memory_order_acquire) >= EXTRA_QUEUE) {
    dropped_voices_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const float gain = std::clamp(place.gain, 0.0f, 1.0f);
  const float pan = std::clamp(place.pan, -1.0f, 1.0f);
  extra_queue_[head % EXTRA_QUEUE] =
      ExtraRequest{.sound = sound,
                   .bank = current_bank_->number,
                   .left = gain * (pan > 0.0f ? 1.0f - pan : 1.0f),
                   .right = gain * (pan < 0.0f ? 1.0f + pan : 1.0f)};
  extra_head_.store(head + 1, std::memory_order_release);
  return true;
}

std::size_t Mixer::extra_voices() const noexcept {
  return extra_count_.load(std::memory_order_relaxed);
}

uint64_t Mixer::dropped_voices() const noexcept {
  return dropped_voices_.load(std::memory_order_relaxed);
}

bool Mixer::stop(std::size_t slot) { return post(slot, 0); }

bool Mixer::playing(std::size_t slot) const noexcept {
  if (slot >= effect_count()) {
    return false;
  }
  const Effect& e = effects_[slot];
  const uint32_t request = e.request.load(std::memory_order_acquire);
  return request != 0 ? (request & PLAY_BIT) != 0
                      : e.active.load(std::memory_order_acquire);
}

bool Mixer::any_effect_playing() const noexcept {
  const std::size_t count = effect_count();
  for (std::size_t i = 0; i < count; ++i) {
    const Effect& e = effects_[i];
    // A pending request says what the effect is about to do. Once the audio
    // thread clears it, active already holds the result.
    const uint32_t request = e.request.load(std::memory_order_acquire);
    const bool playing = request != 0
                             ? (request & PLAY_BIT) != 0
                             : e.active.load(std::memory_order_acquire);
    if (playing) {
      return true;
    }
  }
  return false;
}

void Mixer::music_volume(float volume) noexcept {
  music_volume_.store(unit_range(volume), std::memory_order_relaxed);
}

void Mixer::sound_volume(float volume) noexcept {
  sound_volume_.store(unit_range(volume), std::memory_order_relaxed);
}

void Mixer::music_enabled(bool enabled) noexcept {
  music_enabled_.store(enabled, std::memory_order_relaxed);
}

float Mixer::music_volume() const noexcept {
  return music_volume_.load(std::memory_order_relaxed);
}

float Mixer::sound_volume() const noexcept {
  return sound_volume_.load(std::memory_order_relaxed);
}

bool Mixer::music_enabled() const noexcept {
  return music_enabled_.load(std::memory_order_relaxed);
}

void Mixer::master_volume(float volume) noexcept {
  master_volume_.store(unit_range(volume), std::memory_order_relaxed);
}

float Mixer::master_volume() const noexcept {
  return master_volume_.load(std::memory_order_relaxed);
}

void Mixer::output_muted(bool muted) noexcept {
  output_muted_.store(muted, std::memory_order_relaxed);
}

bool Mixer::output_muted() const noexcept {
  return output_muted_.load(std::memory_order_relaxed);
}

bool Mixer::take_request(std::size_t slot, const EffectBank* bank) {
  Effect& e = effects_[slot];
  uint32_t request = e.request.load(std::memory_order_acquire);
  if (request == 0) {
    return false;
  }
  const bool start = (request & PLAY_BIT) != 0;
  if (start) {
    e.variant = (request >> VARIANT_SHIFT) & VARIANT_MASK;
    e.left = e.place_left.load(std::memory_order_relaxed);
    e.right = e.place_right.load(std::memory_order_relaxed);
    e.position = sound_of(slot, bank).start_offset;
    e.playing = true;
    e.loop = (request & LOOP_BIT) != 0;
  }
  else {
    e.playing = false;
  }
  e.active.store(e.playing, std::memory_order_release);
  // A newer request than this one stays for the next block.
  static_cast<void>(e.request.compare_exchange_strong(
      request, 0, std::memory_order_acq_rel, std::memory_order_relaxed));
  return start;
}

std::size_t Mixer::voices_now() const noexcept {
  std::size_t n = 0;
  const std::size_t count = effect_count();
  for (std::size_t i = 0; i < count; ++i) {
    n += effects_[i].playing ? 1 : 0;
  }
  for (const Extra& x : extras_) {
    n += x.sound ? 1 : 0;
  }
  return n;
}

void Mixer::cut_extras() {
  std::size_t voices = voices_now();
  const std::size_t limit = voice_limit();
  while (voices > limit) {
    Extra* oldest = nullptr;
    for (Extra& x : extras_) {
      if (x.sound && (!oldest || x.started < oldest->started)) {
        oldest = &x;
      }
    }
    if (!oldest) {
      return; // the game's own effects are never cut
    }
    oldest->sound = nullptr;
    dropped_voices_.fetch_add(1, std::memory_order_relaxed);
    --voices;
  }
}

void Mixer::start_extras(const EffectBank* bank) {
  const std::size_t head = extra_head_.load(std::memory_order_acquire);
  std::size_t tail = extra_tail_.load(std::memory_order_relaxed);
  std::size_t voices = voices_now();
  const std::size_t limit = voice_limit();
  for (; tail != head; ++tail) {
    const ExtraRequest& r = extra_queue_[tail % EXTRA_QUEUE];
    // Only sounds of the bank in use; an older bank may be freed.
    Extra* free_voice = nullptr;
    if (bank && r.bank == bank->number && voices < limit) {
      for (Extra& x : extras_) {
        if (!x.sound) {
          free_voice = &x;
          break;
        }
      }
    }
    if (!free_voice) {
      dropped_voices_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    *free_voice = Extra{.sound = r.sound,
                        .position = 0,
                        .left = r.left,
                        .right = r.right,
                        .started = blocks_};
    ++voices;
  }
  extra_tail_.store(tail, std::memory_order_release);
}

void Mixer::mix_extras(std::span<float> acc, float gain_from, float gain_to) {
  const std::size_t total = acc.size();
  std::size_t count = 0;
  for (Extra& x : extras_) {
    if (!x.sound) {
      continue;
    }
    const std::span<const int16_t> samples = x.sound->samples;
    const std::size_t n = std::min(total, samples.size() - x.position);
    const float to =
        gain_from == gain_to
            ? gain_to
            : gain_from + (gain_to - gain_from) * (static_cast<float>(n) /
                                                   static_cast<float>(total));
    mix_add_panned(acc.subspan(0, n), samples.subspan(x.position, n), gain_from,
                   to, x.left, x.right);
    x.position += n;
    if (x.position >= samples.size()) {
      x.sound = nullptr;
    }
    else {
      ++count;
    }
  }
  extra_count_.store(count, std::memory_order_relaxed);
}

void Mixer::mix_effect(Effect& e, const Sound& sound, std::span<float> acc,
                       float gain_from, float gain_to) {
  const std::size_t total = acc.size();
  const std::span<const int16_t> samples = sound.samples;
  // The block's gain ramp, at a sample position within the block.
  const auto gain_at = [&](std::size_t at) {
    return gain_from == gain_to ? gain_to
                                : gain_from + (gain_to - gain_from) *
                                                  (static_cast<float>(at) /
                                                   static_cast<float>(total));
  };
  std::size_t done = 0;
  while (e.playing && done < total) {
    if (e.position >= samples.size()) {
      // Loop without a gap, back to the start offset, in the same block.
      if (e.loop && sound.start_offset < samples.size()) {
        e.position = sound.start_offset;
      }
      else {
        e.playing = false;
        break;
      }
    }
    const std::size_t n = std::min(total - done, samples.size() - e.position);
    mix_add_panned(acc.subspan(done, n), samples.subspan(e.position, n),
                   gain_at(done), gain_at(done + n), e.left, e.right);
    e.position += n;
    done += n;
  }
  // A sound that ends with the block is done now, not a block later.
  if (e.playing && !e.loop && e.position >= samples.size()) {
    e.playing = false;
  }
}

Mixer::Sound Mixer::sound_of(std::size_t slot, const EffectBank* bank) const {
  if (bank && bank->slots[slot] && !bank->slots[slot]->variants.empty()) {
    const auto& variants = bank->slots[slot]->variants;
    // A bank with fewer variants plays its first.
    const std::size_t v = effects_[slot].variant;
    const EffectBank::Sound& r = variants[v < variants.size() ? v : 0];
    return Sound{.samples = r.samples, .start_offset = r.start_offset};
  }
  const Effect& e = effects_[slot];
  return Sound{.samples = e.samples, .start_offset = e.start_offset};
}

void Mixer::effect_bank(std::unique_ptr<EffectBank> bank) {
  const std::lock_guard lock(bank_mutex_);
  last_variant_.fill(0);
  // Every bank handed over gets a higher number, the originals (null)
  // included, so the audio thread can say which ones it has moved past.
  ++banks_made_;
  if (bank) {
    bank->number = banks_made_;
  }
  if (current_bank_) {
    retired_banks_.push_back(std::move(current_bank_));
  }
  current_bank_ = std::move(bank);
  bank_.store(current_bank_.get(), std::memory_order_release);
}

bool Mixer::has_effect_bank() const noexcept {
  return bank_.load(std::memory_order_acquire) != nullptr;
}

void Mixer::collect_banks(bool no_callback) {
  const std::lock_guard lock(bank_mutex_);
  const uint64_t seen = bank_seen_.load(std::memory_order_acquire);
  // The audio thread picked up a bank made after these, and banks only move
  // forward, so it never uses them again.
  std::erase_if(retired_banks_, [&](const std::unique_ptr<EffectBank>& b) {
    return no_callback || b->number < seen;
  });
}

void Mixer::mix_block(std::span<int16_t> out, std::span<const int16_t> music) {
  const std::span<float> acc(acc_.data(), out.size());
  std::fill(acc.begin(), acc.end(), 0.0f);

  // The effect sounds of this block. Returning to the originals (null) frees
  // nothing the audio thread holds: retired banks wait for a newer number.
  const EffectBank* bank = bank_.load(std::memory_order_acquire);
  if (bank) {
    bank_seen_.store(bank->number, std::memory_order_release);
  }
  if (bank != playing_bank_) {
    // Sounds that play on switch over in place; past the new sound's end a
    // loop goes back to its start and a one-shot ends. Ambience one-shots
    // belong to their bank and end with it.
    playing_bank_ = bank;
    for (Extra& x : extras_) {
      x.sound = nullptr;
    }
    const std::size_t n = effect_count();
    for (std::size_t i = 0; i < n; ++i) {
      Effect& e = effects_[i];
      const Sound sound = sound_of(i, bank);
      e.position -= e.position % MIX_CHANNELS;
      if (e.playing && e.position >= sound.samples.size()) {
        e.position = sound.start_offset;
        e.playing = e.loop && sound.start_offset < sound.samples.size();
        e.active.store(e.playing, std::memory_order_release);
      }
    }
  }

  // Requests first: what starts this block decides the voices and the duck.
  const std::size_t count = effect_count();
  bool cut = false;
  bool alert = false;
  for (std::size_t i = 0; i < count; ++i) {
    const bool started = take_request(i, bank);
    const Priority p = priority_of(i, bank);
    cut = cut || (started && p != Priority::AMBIENCE);
    alert = alert || (effects_[i].playing && p == Priority::ALERT);
  }
  if (cut) {
    cut_extras();
  }
  start_extras(bank);
  ++blocks_;

  // Under an alert the music dips; it moves at most one step a block, in a
  // straight line, and the block's gain ramp smooths that step.
  const float frames = static_cast<float>(out.size() / MIX_CHANNELS);
  const float duck_target = alert && ducking() ? DUCK_GAIN : 1.0f;
  if (duck_ > duck_target) {
    duck_ = std::max(duck_target,
                     duck_ - (1.0f - DUCK_GAIN) * frames /
                                 (DUCK_ATTACK_S * static_cast<float>(RATE)));
  }
  else if (duck_ < duck_target) {
    duck_ = std::min(duck_target,
                     duck_ + (1.0f - DUCK_GAIN) * frames /
                                 (DUCK_RELEASE_S * static_cast<float>(RATE)));
  }
  duck_seen_.store(duck_, std::memory_order_relaxed);

  // The master volume and the mute fold into both gains, so they ramp too.
  const float master = output_muted() ? 0.0f : master_volume();
  const float music_target =
      music_enabled()
          ? music_volume_.load(std::memory_order_relaxed) * master * duck_
          : 0.0f;
  mix_add(acc, music, music_gain_, music_target);
  music_gain_ = music_target;

  const float sound_target =
      sound_volume_.load(std::memory_order_relaxed) * master;
  for (std::size_t i = 0; i < count; ++i) {
    Effect& e = effects_[i];
    if (e.playing) {
      mix_effect(e, sound_of(i, bank), acc, sound_gain_, sound_target);
      if (!e.playing) {
        e.active.store(false, std::memory_order_release);
      }
    }
  }
  mix_extras(acc, sound_gain_, sound_target);
  sound_gain_ = sound_target;

  mix_to_s16(acc, out, MASTER_VOLUME);
}

void Mixer::render(std::span<int16_t> out, std::span<const int16_t> music) {
  // Whole frames only; a stray odd sample stays silent.
  const std::size_t usable = out.size() - out.size() % MIX_CHANNELS;
  if (usable < out.size()) {
    out.back() = 0;
  }
  for (std::size_t done = 0; done < usable; done += MIX_BLOCK_SAMPLES) {
    const std::size_t n = std::min(MIX_BLOCK_SAMPLES, usable - done);
    const std::span<const int16_t> music_part =
        music.size() > done
            ? music.subspan(done, std::min(n, music.size() - done))
            : std::span<const int16_t>{};
    mix_block(out.subspan(done, n), music_part);
  }
}

/* Device changes */

DeviceAction device_event_action(uint32_t event_type, bool recording,
                                 uint32_t which, uint32_t open_device,
                                 int live_devices) noexcept {
  if (recording) {
    return DeviceAction::NONE;
  }
  if (event_type == SDL_EVENT_AUDIO_DEVICE_REMOVED) {
    if (open_device == 0) {
      return DeviceAction::NONE;
    }
    // With nothing to play on, SDL would keep the stream running into a
    // silent stand-in device and the song would move on unheard.
    if (live_devices <= 0) {
      return DeviceAction::CLOSE;
    }
    // Our own device is only reported gone when SDL could not move it to
    // the new default device.
    return which == open_device ? DeviceAction::REOPEN : DeviceAction::NONE;
  }
  if (event_type == SDL_EVENT_AUDIO_DEVICE_ADDED) {
    return open_device == 0 ? DeviceAction::REOPEN : DeviceAction::NONE;
  }
  return DeviceAction::NONE;
}

/* AudioManager */

#if PLEGACY_ENABLE_MUSIC
void AudioManager::ADLMIDIDeleter::operator()(ADL_MIDIPlayer* p) {
  if (p) {
    adl_close(p);
  }
}
#endif

AudioManager::AudioManager()
    : music_data_(static_cast<size_t>(MusicId::COUNT)) {
  for (std::size_t i = 0; i < static_cast<std::size_t>(SoundId::COUNT); ++i) {
    mixer_.priority(i, default_priority(static_cast<SoundId>(i)));
  }
  // Variants and ambience differ from one run to the next.
  const uint64_t seed =
      (uint64_t{std::random_device{}()} << 32) ^ std::random_device{}();
  mixer_.seed(seed);
  ambience_random_.seed(~seed);
}

AudioManager::~AudioManager() {
  // Stops the callback before any member it uses goes away.
  close_stream();
}

bool AudioManager::open_stream() {
  SDL_AudioSpec spec{
      .format = SDL_AUDIO_S16,
      .channels = CHANNELS,
      .freq = SAMPLE_RATE,
  };
  audio_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                            &spec, audio_callback, this);
  if (!audio_stream_) {
    Logger::warn("Failed to open SDL audio device: {}", SDL_GetError());
    return false;
  }
  device_id_ = SDL_GetAudioStreamDevice(audio_stream_);
  // Device starts paused
  SDL_ResumeAudioDevice(device_id_);
  return true;
}

void AudioManager::close_stream() {
  if (audio_stream_) {
    // Returns once a callback that is running has finished.
    SDL_DestroyAudioStream(audio_stream_);
    audio_stream_ = nullptr;
  }
  device_id_ = 0;
}

bool AudioManager::reopen_device() {
  if (!audio_enabled_) {
    return false;
  }
  close_stream();
  if (!open_stream()) {
    Logger::warn("No audio device; the sound comes back when one is added");
    return false;
  }
  Logger::info("Audio device {} opened", device_id_);
  return true;
}

void AudioManager::handle_event(const SDL_Event& event) {
  switch (event.type) {
  case SDL_EVENT_WINDOW_FOCUS_GAINED:
    window_focused_ = true;
    update_focus_mute();
    return;
  case SDL_EVENT_WINDOW_FOCUS_LOST:
    window_focused_ = false;
    update_focus_mute();
    return;
  case SDL_EVENT_AUDIO_DEVICE_ADDED:
  case SDL_EVENT_AUDIO_DEVICE_REMOVED:
    break;
  default:
    return;
  }
  if (!audio_enabled_) {
    return;
  }
  int live_devices = 0;
  SDL_free(SDL_GetAudioPlaybackDevices(&live_devices));
  switch (device_event_action(event.type, event.adevice.recording,
                              event.adevice.which, device_id_, live_devices)) {
  case DeviceAction::NONE:
    break;
  case DeviceAction::REOPEN:
    reopen_device();
    break;
  case DeviceAction::CLOSE:
    Logger::warn("No audio device left; the sound pauses until one is added");
    close_stream();
    break;
  }
}

void AudioManager::update_focus_mute() {
  mixer_.output_muted(mute_when_unfocused_ && !window_focused_);
}

void AudioManager::mute_when_unfocused(bool enabled) {
  mute_when_unfocused_ = enabled;
  update_focus_mute();
}

bool AudioManager::focus_muted() const { return mixer_.output_muted(); }

void AudioManager::master_volume(float volume) { mixer_.master_volume(volume); }

void AudioManager::load_music(MusicId id, std::vector<uint8_t> data) {
  const auto slot = static_cast<size_t>(id);
  if (slot < music_data_.size()) {
    music_data_[slot] = std::move(data);
  }
}

double AudioManager::music_position() const {
#if PLEGACY_ENABLE_MUSIC
  return music_position_ms_.load(std::memory_order_relaxed) / 1000.0;
#else
  return 0.0;
#endif
}

void AudioManager::init() {
  Logger::info("AudioManager initializing...");
  if (initialized_) {
    Logger::warn("AudioManager already initialized");
    return;
  }

#if PLEGACY_ENABLE_MUSIC
  // Initialize libADLMIDI
  midi_player_.reset(adl_init(SAMPLE_RATE));
  if (!midi_player_) {
    Logger::warn("Failed to initialize libADLMIDI");
    audio_enabled_ = false;
    initialized_ = true;
    return;
  }

  adl_setVolumeRangeModel(midi_player_.get(), ADLMIDI_VolumeModel_CMF);
  adl_setNumChips(midi_player_.get(), 1);
#endif

  initialized_ = true;
  if (!open_stream()) {
    audio_enabled_ = false;
    return;
  }
  audio_enabled_ = true;

  Logger::info("AudioManager initialized: {}Hz, {} channels", SAMPLE_RATE,
               CHANNELS);
}

void AudioManager::load_default_assets() {
  load_m_container("sound/m0");  // Restaurant noise (VOC)
  load_m_container("sound/m1");  // Tech The Tronic (CMF)
  load_m_container("sound/m2");  // Jam The Scratch (CMF)
  load_m_container("sound/m3");  // Heavy Banger (CMF)
  load_m_container("sound/m4");  // Amore Mio (CMF)
  load_m_container("sound/m5");  // Elevator Music (CMF)
  load_m_container("sound/m6");  // Tea Shoppe (CMF)
  load_m_container("sound/m7");  // Traffic (VOC)
  load_m_container("sound/m8");  // Terminal (VOC)
  load_m_container("sound/m9");  // Hoover (VOC)
  load_m_container("sound/m10"); // Break-in music + burglary sounds (CMF + VOC)
  load_m_container("sound/m11"); // Break-in music (CMF)
  effects_loaded();
}

void AudioManager::effects_loaded() {
  // A selection made before the effects were there takes effect now.
  const std::lock_guard lock(effects_mutex_);
  effects_loaded_ = true;
  if (wanted_effects_ != effects_) {
    if (const EffectsChange done = apply_effects(); !done) {
      Logger::warn("Enhanced effects not used: {}", done.error);
      effects_problem_ = done.error;
    }
  }
}

void AudioManager::effects_pack(std::filesystem::path folder) {
  const std::lock_guard lock(effects_mutex_);
  effects_pack_ = std::move(folder);
}

AudioManager::EffectsChange AudioManager::select_effects(EffectsSet set) {
  const std::lock_guard lock(effects_mutex_);
  const EffectsSet before = wanted_effects_;
  wanted_effects_ = set;
  if (!effects_loaded_ && mixer_.effect_count() == 0) {
    return {}; // applied by load_default_assets()
  }
  EffectsChange done = apply_effects();
  if (!done) {
    wanted_effects_ = before;
  }
  return done;
}

AudioManager::EffectsChange AudioManager::apply_effects() {
  EffectsChange done;
  if (wanted_effects_ == EffectsSet::ORIGINAL) {
    mixer_.effect_bank(nullptr);
    effects_ = EffectsSet::ORIGINAL;
  }
  else {
    std::optional<EffectPack> pack = load_effect_pack(
        effects_pack_, effect_sources_, EffectLimits{}, &done.error);
    if (!pack) {
      // The previous selection stays as it was.
      wanted_effects_ = effects_;
      return done;
    }
    done.outcomes = std::move(pack->outcomes);
    mixer_.effect_bank(std::move(pack->bank));
    effects_ = EffectsSet::ENHANCED;
  }
  mixer_.collect_banks(audio_stream_ == nullptr);
  return done;
}

std::string AudioManager::take_effects_problem() {
  const std::lock_guard lock(effects_mutex_);
  return std::exchange(effects_problem_, {});
}

EffectsSet AudioManager::effects() const {
  const std::lock_guard lock(effects_mutex_);
  return effects_;
}

std::string AudioManager::effect_source_sha256(SoundId id) const {
  const std::lock_guard lock(effects_mutex_);
  const auto i = static_cast<std::size_t>(id);
  return i < effect_sources_.size() ? effect_sources_[i] : std::string{};
}

void AudioManager::load_m_container(const std::string& m_file_path) {
  if (!audio_enabled_) {
    Logger::debug("Audio disabled, skipping load of {}", m_file_path);
    return;
  }

  std::vector<AudioAsset> assets = MFileLoader::load(m_file_path);

  for (auto& asset : assets) {
    if (asset.type == AudioAssetType::CMF) {
#if PLEGACY_ENABLE_MUSIC
      if (next_music_slot_ >= static_cast<size_t>(MusicId::COUNT)) {
        Logger::warn("Too many music tracks, ignoring CMF from {}",
                     m_file_path);
        continue;
      }

      music_data_[next_music_slot_] = std::move(asset.data);
      next_music_slot_++;
#endif
    }
    else if (asset.type == AudioAssetType::VOC) {
      VocAudioData voc_audio = VocDecoder::decode(asset.data, SAMPLE_RATE);

      // Skip the static noise at the start of the chopper sound: playback
      // starts, and loops back, past it.
      constexpr size_t chopper_skip_samples = 500;
      const bool chopper =
          mixer_.effect_count() == static_cast<size_t>(SoundId::CHOPPER);
      const size_t start_offset =
          chopper && voc_audio.samples.size() > chopper_skip_samples
              ? chopper_skip_samples
              : 0;
      const std::size_t slot = mixer_.effect_count();
      if (!mixer_.add_effect(std::move(voc_audio.samples), start_offset)) {
        Logger::warn("Too many sound effects, ignoring VOC from {}",
                     m_file_path);
      }
      else if (slot < effect_sources_.size()) {
        // What an enhanced pack names as the original it replaces.
        const std::string hash = core::content::Sha256::hex(
            std::string_view(reinterpret_cast<const char*>(asset.data.data()),
                             asset.data.size()));
        const std::lock_guard lock(effects_mutex_);
        effect_sources_[slot] = hash;
      }
    }
  }
}

void AudioManager::play_sound(SoundId id, LoopMode loop) {
  if (!audio_enabled_) {
    return;
  }

  const auto slot = static_cast<size_t>(id);
  if (!mixer_.play(slot, loop == LoopMode::Loop)) {
    Logger::warn("Sound slot {} out of range (have {} sounds)", slot,
                 mixer_.effect_count());
  }
}

void AudioManager::play_sound_at(SoundId id, Placement place, LoopMode loop) {
  if (!audio_enabled_) {
    return;
  }
  const auto slot = static_cast<size_t>(id);
  if (!mixer_.play(slot, loop == LoopMode::Loop, place)) {
    Logger::warn("Sound slot {} out of range (have {} sounds)", slot,
                 mixer_.effect_count());
  }
}

void AudioManager::ducking(bool enabled) { mixer_.ducking(enabled); }

void AudioManager::voice_limit(std::size_t limit) { mixer_.voice_limit(limit); }

void AudioManager::effects_seed(uint64_t seed) {
  mixer_.seed(seed);
  const std::lock_guard lock(effects_mutex_);
  ambience_random_.seed(~seed);
}

void AudioManager::tick(uint32_t dt_ms) {
  const std::lock_guard lock(effects_mutex_);
  now_ms_ += dt_ms;
  const Mixer::EffectBank* bank = mixer_.effect_bank();
  if (!bank || bank->scenes.empty()) {
    ambience_.clear();
    ambience_bank_ = 0;
    return;
  }
  if (bank->number != ambience_bank_) {
    ambience_bank_ = bank->number;
    ambience_.assign(bank->scenes.size(), {});
    for (std::size_t s = 0; s < bank->scenes.size(); ++s) {
      ambience_[s].assign(bank->scenes[s].layers.size(), LayerClock{});
    }
  }
  for (std::size_t s = 0; s < bank->scenes.size(); ++s) {
    const Mixer::EffectBank::Scene& scene = bank->scenes[s];
    const bool bed = mixer_.playing(scene.bed);
    for (std::size_t l = 0; l < scene.layers.size(); ++l) {
      const Mixer::EffectBank::Layer& layer = scene.layers[l];
      LayerClock& clock = ambience_[s][l];
      if (!bed || layer.variants.empty()) {
        clock.armed = false; // starts again from a full interval
        continue;
      }
      const auto interval = [&] {
        return ambience_random_.between(layer.min_interval_ms,
                                        layer.max_interval_ms);
      };
      if (!clock.armed) {
        clock.armed = true;
        clock.due_ms = now_ms_ + interval();
        continue;
      }
      if (now_ms_ < clock.due_ms) {
        continue;
      }
      // One at a time: a long frame never brings a burst.
      std::size_t v = 0;
      const std::size_t n = layer.variants.size();
      if (n > 1) {
        v = ambience_random_.below(n - 1);
        if (v >= clock.last_variant % n) {
          ++v;
        }
      }
      clock.last_variant = v;
      static_cast<void>(mixer_.play_extra(
          &layer.variants[v], Placement{.pan = ambience_random_.uniform(
                                            layer.min_pan, layer.max_pan)}));
      clock.due_ms = now_ms_ + interval();
    }
  }
}

void AudioManager::stop_sound(SoundId id) {
  if (!audio_enabled_) {
    return;
  }
  mixer_.stop(static_cast<size_t>(id));
}

void AudioManager::play_sound_if_changed(SoundId id, LoopMode loop) {
  if (current_indexed_sound_ == id) {
    return;
  }
  if (current_indexed_sound_) {
    stop_sound(*current_indexed_sound_);
  }
  play_sound(id, loop);
  current_indexed_sound_ = id;
}

void AudioManager::stop_current_indexed_sound() {
  if (current_indexed_sound_) {
    stop_sound(*current_indexed_sound_);
    current_indexed_sound_.reset();
  }
}

void AudioManager::play_music(MusicId id, LoopMode loop) {
#if PLEGACY_ENABLE_MUSIC
  if (!audio_enabled_) {
    return;
  }

  if (!mixer_.music_enabled()) {
    return;
  }

  auto slot = static_cast<size_t>(id);
  if (slot >= music_data_.size() || music_data_[slot].empty()) {
    Logger::warn("Music {} not loaded", slot);
    return;
  }

  // The callback does not wait for this lock: while the song loads it plays
  // the music as silence. The old song is cut here.
  const std::lock_guard lock(music_mutex_);
  music_stop_requested_.store(false);

  adl_reset(midi_player_.get());

  int result =
      adl_openData(midi_player_.get(), music_data_[slot].data(),
                   static_cast<unsigned long>(music_data_[slot].size()));

  music_position_ms_.store(0, std::memory_order_relaxed);
  if (result < 0) {
    Logger::warn("Failed to load music data: {}",
                 adl_errorInfo(midi_player_.get()));
    music_state_.store(MusicState::STOPPED);
  }
  else {
    music_loop_ = (loop == LoopMode::Loop);
    music_state_.store(MusicState::PLAYING);
  }
#else
  (void)id;
  (void)loop;
#endif
}

void AudioManager::stop_music() {
#if PLEGACY_ENABLE_MUSIC
  if (!audio_enabled_) {
    return;
  }
  // The callback starts the fade-out at its next block (render_music()).
  if (music_state_.load() == MusicState::PLAYING) {
    music_stop_requested_.store(true);
  }
#endif
}

void AudioManager::music_volume(float volume) { mixer_.music_volume(volume); }

void AudioManager::sound_volume(float volume) { mixer_.sound_volume(volume); }

void AudioManager::music_enabled(bool enabled) {
  mixer_.music_enabled(enabled);
}

bool AudioManager::is_playing() const {
  if (!audio_enabled_) {
    return false;
  }

#if PLEGACY_ENABLE_MUSIC
  // The flag first: the callback stores the new state before clearing it.
  if (!music_stop_requested_.load() &&
      music_state_.load() == MusicState::PLAYING) {
    return true;
  }
#endif

  return mixer_.any_effect_playing();
}

void AudioManager::audio_callback(void* userdata, SDL_AudioStream* stream,
                                  int additional_amount, int /*total_amount*/) {
  auto* manager = static_cast<AudioManager*>(userdata);
  if (additional_amount <= 0 || !manager) {
    // No data results in silence
    return;
  }

  // additional_amount is in bytes; mix whole interleaved sample frames, one
  // fixed-size block at a time.
  std::size_t frames = static_cast<std::size_t>(additional_amount) /
                       (sizeof(int16_t) * MIX_CHANNELS);
  while (frames > 0) {
    const std::size_t n = std::min(frames, MIX_BLOCK_FRAMES);
    const std::span<int16_t> block(manager->out_block_.data(),
                                   n * MIX_CHANNELS);
    manager->fill_block(block);
    SDL_PutAudioStreamData(stream, block.data(),
                           static_cast<int>(block.size_bytes()));
    frames -= n;
  }
}

void AudioManager::music_route(music::Source source, uint32_t generation) {
  music_route_.store(route_word(source, generation), std::memory_order_release);
}

music::Source AudioManager::music_source() const {
  return route_source(music_route_.load(std::memory_order_acquire));
}

uint32_t AudioManager::music_generation() const {
  return route_generation(music_route_.load(std::memory_order_acquire));
}

bool AudioManager::render_source(music::Source source, uint32_t generation,
                                 std::span<int16_t> block) {
  switch (source) {
  case music::Source::ORIGINAL:
    music_ring_.drop_stale(generation + 1);
#if PLEGACY_ENABLE_MUSIC
    render_music(block);
    return true;
#else
    return false;
#endif
  case music::Source::MY_MUSIC:
    music_ring_.read(block, generation);
    return true;
  case music::Source::OFF:
    music_ring_.drop_stale(generation + 1);
    return false;
  }
  return false;
}

void AudioManager::fill_block(std::span<int16_t> out) {
  mix_next(out);
  // The game's own sound only, as the device gets it.
  mix_tap_.write(out);
}

void AudioManager::mix_next(std::span<int16_t> out) {
  const std::span<int16_t> music(music_block_.data(), out.size());
  const uint64_t route = music_route_.load(std::memory_order_acquire);
  const music::Source source = route_source(route);
  const uint32_t generation = route_generation(route);

  if (route != playing_route_) {
    // The old source stops here: none of its audio is played from now on.
    // Its last frame fades out so the cut does not click, and the new source
    // fades in from the next block. Stale ring audio is dropped meanwhile.
    playing_route_ = route;
    fade_in_ = true;
    music_ring_.drop_stale(generation);
    declick_block(music, last_music_, DECLICK_FRAMES);
    last_music_.fill(0);
    mixer_.render(out, music);
    return;
  }

  if (!render_source(source, generation, music)) {
    fade_in_ = false;
    last_music_.fill(0);
    mixer_.render(out);
    return;
  }
  if (source == music::Source::MY_MUSIC) {
    scale_block(music, fade_in_ ? 0.0f : MY_MUSIC_GAIN, MY_MUSIC_GAIN);
  }
  else if (fade_in_) {
    scale_block(music, 0.0f, 1.0f);
  }
  fade_in_ = false;
  if (music.size() >= MIX_CHANNELS) {
    std::copy(music.end() - MIX_CHANNELS, music.end(), last_music_.begin());
  }
  mixer_.render(out, music);
}

#if PLEGACY_ENABLE_MUSIC
void AudioManager::render_music(std::span<int16_t> block) {
  const int count = static_cast<int>(block.size());
  short* out = reinterpret_cast<short*>(block.data());
  int generated = 0;

  // Never wait here: play_music() holds the lock while it loads a song.
  std::unique_lock lock(music_mutex_, std::try_to_lock);
  if (lock.owns_lock() && midi_player_) {
    ADL_MIDIPlayer* player = midi_player_.get();

    if (music_stop_requested_.load()) {
      if (music_state_.load() == MusicState::PLAYING) {
        // Trigger the release phase of the active notes so FM tails fade out
        adl_panic(player);
        drain_samples_ = 0;
        music_state_.store(MusicState::DRAINING);
      }
      music_stop_requested_.store(false);
    }

    const MusicState state = music_state_.load();
    if (state == MusicState::PLAYING) {
      generated = std::max(adl_play(player, count, out), 0);

      // Detect end-of-song (adl_play returns fewer samples than requested)
      if (generated < count) {
        if (music_loop_) {
          adl_positionRewind(player);
          generated +=
              std::max(adl_play(player, count - generated, out + generated), 0);
        }
        else {
          adl_panic(player);
          drain_samples_ = 0;
          music_state_.store(MusicState::DRAINING);
        }
      }
    }
    else if (state == MusicState::DRAINING) {
      // Process the OPL3 chip's release envelopes without advancing the
      // MIDI sequencer or triggering new notes.
      // This makes it sound the same as the original
      generated = std::max(adl_generate(player, count, out), 0);

      drain_samples_ += count / CHANNELS;

      // Check for silence to end the drain
      bool is_silent = true;
      for (int i = 0; i < std::min(generated, count); ++i) {
        if (std::abs(block[static_cast<size_t>(i)]) > 15) { // FM noise floor
          is_silent = false;
          break;
        }
      }

      if (is_silent || drain_samples_ > MAX_DRAIN_SAMPLES) {
        music_state_.store(MusicState::STOPPED);
        adl_reset(player);
      }
    }

    // For music_position(): the song's place after this block.
    const double at = music_state_.load() == MusicState::STOPPED
                          ? 0.0
                          : adl_positionTell(player);
    music_position_ms_.store(
        at > 0.0 ? static_cast<uint32_t>(std::min(at * 1000.0, 4.0e9)) : 0u,
        std::memory_order_relaxed);
  }

  const auto written = static_cast<size_t>(std::clamp(generated, 0, count));
  // Boost raw OPL output to get the audio louder (too soft otherwise)
  for (size_t i = 0; i < written; ++i) {
    const float boosted = static_cast<float>(block[i]) * MUSIC_BOOST;
    block[i] = static_cast<int16_t>(std::clamp(boosted, -32768.0f, 32767.0f));
  }
  // Zero out any samples not written by the emulator
  std::fill(block.begin() + static_cast<std::ptrdiff_t>(written), block.end(),
            int16_t{0});
}
#endif

} // namespace audio
