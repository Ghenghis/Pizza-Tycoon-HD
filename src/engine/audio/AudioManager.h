/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#if PLEGACY_ENABLE_MUSIC
struct ADL_MIDIPlayer;
#endif

struct SDL_AudioStream;

/* Threading model

   Two threads run audio code: the game thread, which makes every public
   AudioManager call, and SDL's audio thread, which runs the audio callback.
   The callback must never wait for the game thread, so:

   - Volumes, the music switch and sound effect play/stop requests are
     atomics. Setters only store them; the callback reads them once per
     block of output.
   - A sound effect's samples are written once, before its slot is published
     with a release store of the effect count. The callback only reads
     published slots, and they never change or move afterwards.
   - The FM music player is guarded by music_mutex_. play_music() holds it
     while it resets the player and parses the song; the callback only
     try_locks it, and plays that block of music as silence rather than
     wait. stop_music() takes no lock: it posts a request for the callback.
   - Every buffer the callback uses is a fixed-size member, so it does not
     allocate. Larger requests are mixed in several MIX_BLOCK_FRAMES blocks.
*/

namespace audio {

enum class MusicState {
  STOPPED,
  PLAYING,
  DRAINING,
};

enum class SoundId {
  // M0
  RESTAURANT_NOISE_1 = 0,
  RESTAURANT_NOISE_2,
  RESTAURANT_NOISE_3,

  // M7
  TRAFFIC_1,
  TRAFFIC_2,

  // M8
  DIAL_TONE_0,
  DIAL_TONE_1,
  DIAL_TONE_2,
  DIAL_TONE_3,
  DIAL_TONE_4,
  DIAL_TONE_5,
  DIAL_TONE_6,
  DIAL_TONE_7,
  DIAL_TONE_8,
  DIAL_TONE_9,
  PHONE_RINGING, // m8[10]
  PHONE_PICKUP,  // m8[11]

  // M9
  CHOPPER, // m9[0]

  // M10
  WEAPON_1,     // m10[2] - First weapon sound
  WEAPON_2,     // m10[3]
  WEAPON_3,     // m10[4]
  WEAPON_4,     // m10[5]
  POLICE_SIREN, // m10[6]

  COUNT
};

enum class MusicId {
  TECH_THE_TRONIC = 0, // m1
  JAM_THE_SCRATCH,     // m2
  HEAVY_BANGER,        // m3
  AMORE_MIO,           // m4
  ELEVATOR_MUSIC,      // m5
  TEA_SHOPPE,          // m6

  // M10
  BREAK_IN_MUSIC_1, // m10[0]
  BREAK_IN_MUSIC_2, // m10[1]

  // M11
  BREAK_IN_MUSIC_3, // m11[0]

  COUNT
};

enum class LoopMode {
  Once,
  Loop,
};

// All audio is interleaved signed 16-bit stereo. The mixer works in blocks
// of at most this many frames (one sample per channel each).
inline constexpr std::size_t MIX_CHANNELS = 2;
inline constexpr std::size_t MIX_BLOCK_FRAMES = 1024;
inline constexpr std::size_t MIX_BLOCK_SAMPLES =
    MIX_BLOCK_FRAMES * MIX_CHANNELS;

// Adds src into acc. acc is float with 1.0 as full scale; src is int16. The
// gain moves in a straight line from gain_from to gain_to across the block's
// frames (reaching gain_to on the last one), so a volume change does not
// click; equal gains give a constant gain. Mixes min(acc, src) samples.
void mix_add(std::span<float> acc, std::span<const int16_t> src,
             float gain_from, float gain_to);

// Converts a float mix to int16: scales by master, rounds to the nearest
// value and saturates at -32768..32767. Converts min(acc, out) samples.
void mix_to_s16(std::span<const float> acc, std::span<int16_t> out,
                float master);

// The software mixer behind the audio callback: every playing sound effect
// plus a block of music, summed in float with separate music and effect
// gains, then converted to int16 once. See the threading model above.
class Mixer {
public:
  static constexpr std::size_t MAX_EFFECTS = 64;
  // Headroom so music and several effects together rarely clip.
  static constexpr float MASTER_VOLUME = 0.8f;

  Mixer() = default;
  Mixer(const Mixer&) = delete;
  Mixer& operator=(const Mixer&) = delete;

  // Game thread. Stores a sound effect and returns its slot, or nullopt when
  // every slot is taken. Playback starts at start_offset samples and loops
  // back there. Odd trailing samples are dropped, to keep whole frames.
  std::optional<std::size_t> add_effect(std::vector<int16_t> samples,
                                        std::size_t start_offset = 0);
  [[nodiscard]] std::size_t effect_count() const noexcept;

  // Any thread. Takes effect at the start of the next block. False when the
  // slot holds no effect.
  bool play(std::size_t slot, bool loop);
  bool stop(std::size_t slot);
  // Playing, or asked to play and not yet started.
  [[nodiscard]] bool any_effect_playing() const noexcept;

  // Any thread. Volumes are clamped to 0..1 (NaN counts as 0). Turning the
  // music off mutes it; the song keeps its place.
  void music_volume(float volume) noexcept;
  void sound_volume(float volume) noexcept;
  void music_enabled(bool enabled) noexcept;
  [[nodiscard]] float music_volume() const noexcept;
  [[nodiscard]] float sound_volume() const noexcept;
  [[nodiscard]] bool music_enabled() const noexcept;

  // Audio thread only. Fills out with the effects plus music (empty for
  // none, otherwise the same size as out). Any even size works.
  void render(std::span<int16_t> out, std::span<const int16_t> music = {});

private:
  struct Effect {
    // Written once by add_effect(), before the slot is published.
    std::vector<int16_t> samples;
    std::size_t start_offset = 0;
    // Latest play or stop request, 0 when none (see post()).
    std::atomic<uint32_t> request{0};
    // Whether the audio thread is playing it.
    std::atomic<bool> active{false};
    // Audio thread only.
    std::size_t position = 0;
    bool playing = false;
    bool loop = false;
  };

  bool post(std::size_t slot, uint32_t flags);
  static void take_request(Effect& e);
  static void mix_effect(Effect& e, std::span<float> acc, float gain_from,
                         float gain_to);
  void mix_block(std::span<int16_t> out, std::span<const int16_t> music);

  std::array<Effect, MAX_EFFECTS> effects_;
  std::atomic<std::size_t> effect_count_{0};
  std::mutex add_mutex_; // one add_effect() at a time; never the audio thread
  std::atomic<uint32_t> next_request_{0};

  std::atomic<float> music_volume_{1.0f};
  std::atomic<float> sound_volume_{0.9f};
  std::atomic<bool> music_enabled_{true};

  // Audio thread only: gains reached at the end of the last block, and the
  // float mix of the current one.
  float music_gain_ = 1.0f;
  float sound_gain_ = 0.9f;
  std::array<float, MIX_BLOCK_SAMPLES> acc_{};
};

class AudioManager {
public:
  AudioManager();
  ~AudioManager();

  AudioManager(const AudioManager&) = delete;
  AudioManager& operator=(const AudioManager&) = delete;

  void play_sound(SoundId id, LoopMode loop = LoopMode::Once);
  void play_sound_if_changed(SoundId id, LoopMode loop = LoopMode::Once);
  void stop_current_indexed_sound();
  void play_music(MusicId id, LoopMode loop = LoopMode::Loop);
  void stop_music();
  void stop_sound(SoundId id);

  // Volume control (0.0 to 1.0)
  void music_volume(float volume);
  void sound_volume(float volume);
  void music_enabled(bool enabled);

  void init();
  void load_default_assets();

  // automatically distributes CMF/VOC to appropriate storage
  void load_m_container(const std::string& m_file_path);

  bool is_initialized() const { return initialized_; }
  bool is_enabled() const { return audio_enabled_; }
  bool is_playing() const;

private:
  static void audio_callback(void* userdata, SDL_AudioStream* stream,
                             int additional_amount, int total_amount);
  // Audio thread: one block of output, at most MIX_BLOCK_SAMPLES.
  void fill_block(std::span<int16_t> out);

  static constexpr int SAMPLE_RATE = 44100;
  static constexpr int CHANNELS = static_cast<int>(MIX_CHANNELS);

#if PLEGACY_ENABLE_MUSIC
  // Audio thread: the next block of FM music, boosted, into block.
  void render_music(std::span<int16_t> block);

  struct ADLMIDIDeleter {
    void operator()(ADL_MIDIPlayer* p);
  };
  std::unique_ptr<ADL_MIDIPlayer, ADLMIDIDeleter> midi_player_;
  // Guards midi_player_ once the device runs, music_loop_ and
  // drain_samples_.
  std::mutex music_mutex_;
  std::atomic<MusicState> music_state_{MusicState::STOPPED};
  std::atomic<bool> music_stop_requested_{false};
  bool music_loop_ = false;
  int drain_samples_ = 0;
  static constexpr int MAX_DRAIN_SAMPLES = SAMPLE_RATE * 2; // 2 seconds max
  // Pre-volume boost for OPL output (otherwise was too quiet):
  static constexpr float MUSIC_BOOST = 6.0f;
  std::array<int16_t, MIX_BLOCK_SAMPLES> music_block_{}; // audio thread
  size_t next_music_slot_ = 0;
#endif
  // Indexed by MusicId:
  std::vector<std::vector<uint8_t>> music_data_;

  Mixer mixer_;
  // Selection for play_sound_if_changed.
  std::optional<SoundId> current_indexed_sound_;

  SDL_AudioStream* audio_stream_{nullptr};
  std::array<int16_t, MIX_BLOCK_SAMPLES> out_block_{}; // audio thread

  bool initialized_ = false;
  bool audio_enabled_ = false;
};

} // namespace audio
