/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/audio/AudioManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "engine/Logger.h"
#include "engine/SDL_include.h"
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

namespace {

// A request word: play/loop flags in the low bits, a tag in the rest.
constexpr uint32_t PLAY_BIT = 1;
constexpr uint32_t LOOP_BIT = 2;
constexpr uint32_t TAG_SHIFT = 2;
constexpr uint32_t TAG_COUNT = UINT32_MAX >> TAG_SHIFT;

constexpr float INT16_TO_FLOAT = 1.0f / 32768.0f;

// Clamps to 0..1; NaN becomes 0.
float unit_range(float v) noexcept {
  return v >= 0.0f ? std::min(v, 1.0f) : 0.0f;
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

bool Mixer::play(std::size_t slot, bool loop) {
  return post(slot, PLAY_BIT | (loop ? LOOP_BIT : 0u));
}

bool Mixer::stop(std::size_t slot) { return post(slot, 0); }

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

void Mixer::take_request(Effect& e) {
  uint32_t request = e.request.load(std::memory_order_acquire);
  if (request == 0) {
    return;
  }
  if ((request & PLAY_BIT) != 0) {
    e.position = e.start_offset;
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
}

void Mixer::mix_effect(Effect& e, std::span<float> acc, float gain_from,
                       float gain_to) {
  const std::size_t total = acc.size();
  const std::span<const int16_t> samples(e.samples);
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
      if (e.loop && e.start_offset < samples.size()) {
        e.position = e.start_offset;
      }
      else {
        e.playing = false;
        break;
      }
    }
    const std::size_t n = std::min(total - done, samples.size() - e.position);
    mix_add(acc.subspan(done, n), samples.subspan(e.position, n), gain_at(done),
            gain_at(done + n));
    e.position += n;
    done += n;
  }
  // A sound that ends with the block is done now, not a block later.
  if (e.playing && !e.loop && e.position >= samples.size()) {
    e.playing = false;
  }
}

void Mixer::mix_block(std::span<int16_t> out, std::span<const int16_t> music) {
  const std::span<float> acc(acc_.data(), out.size());
  std::fill(acc.begin(), acc.end(), 0.0f);

  const float music_target =
      music_enabled() ? music_volume_.load(std::memory_order_relaxed) : 0.0f;
  mix_add(acc, music, music_gain_, music_target);
  music_gain_ = music_target;

  const float sound_target = sound_volume_.load(std::memory_order_relaxed);
  const std::size_t count = effect_count();
  for (std::size_t i = 0; i < count; ++i) {
    Effect& e = effects_[i];
    take_request(e);
    if (e.playing) {
      mix_effect(e, acc, sound_gain_, sound_target);
      if (!e.playing) {
        e.active.store(false, std::memory_order_release);
      }
    }
  }
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

/* AudioManager */

#if PLEGACY_ENABLE_MUSIC
void AudioManager::ADLMIDIDeleter::operator()(ADL_MIDIPlayer* p) {
  if (p) {
    adl_close(p);
  }
}
#endif

AudioManager::AudioManager()
    : music_data_(static_cast<size_t>(MusicId::COUNT)) {}

AudioManager::~AudioManager() {
  // Stops the callback before any member it uses goes away.
  if (audio_stream_) {
    SDL_DestroyAudioStream(audio_stream_);
  }
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

  SDL_AudioSpec spec{
      .format = SDL_AUDIO_S16,
      .channels = CHANNELS,
      .freq = SAMPLE_RATE,
  };

  audio_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                            &spec, audio_callback, this);
  if (!audio_stream_) {
    Logger::warn("Failed to open SDL audio device: {}", SDL_GetError());
    audio_enabled_ = false;
    initialized_ = true;
    return;
  }

  audio_enabled_ = true;
  initialized_ = true;

  // Device starts paused
  SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audio_stream_));

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
      if (!mixer_.add_effect(std::move(voc_audio.samples), start_offset)) {
        Logger::warn("Too many sound effects, ignoring VOC from {}",
                     m_file_path);
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

void AudioManager::fill_block(std::span<int16_t> out) {
#if PLEGACY_ENABLE_MUSIC
  const std::span<int16_t> music(music_block_.data(), out.size());
  render_music(music);
  mixer_.render(out, music);
#else
  mixer_.render(out);
#endif
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
