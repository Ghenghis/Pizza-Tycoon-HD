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
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/music/MusicTypes.h"
#include "engine/music/PcmRing.h"

#if PLEGACY_ENABLE_MUSIC
struct ADL_MIDIPlayer;
#endif

struct SDL_AudioStream;
union SDL_Event;

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
   - Device changes (handle_event(), reopen_device()) run on the game thread.
     Destroying the old stream waits for a callback that is running, before
     the new stream opens, so two callbacks never overlap. The music player
     and the mixer belong to AudioManager, not to the stream, so the song and
     the effects carry on from where they were.
   - The music route (which source makes the music block, and the playback
     generation) is one atomic word, set by the music owner
     (music::MusicPlayback). The FM player runs only while the route is
     ORIGINAL. The player's own music comes from music_ring_, a lock-free
     single-producer single-consumer ring that the music worker fills; the
     callback only reads it, never waits for it, and plays a shortfall as
     silence. Audio of an older generation is dropped unplayed. The callback
     never opens files, decodes or signals the worker.
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

// Which effect sounds play: the 1994 ones, or an enhanced pack's
// replacements (with the original for every event it does not replace).
enum class EffectsSet : uint8_t { ORIGINAL, ENHANCED };
[[nodiscard]] std::string to_string(EffectsSet set);

// What became of one entry of an effects pack.
struct EffectOutcome {
  std::string event;     // as the manifest names it
  int slot = -1;         // the SoundId, -1 when the event is unknown
  bool replaced = false; // false: the original plays, for reason
  std::string reason;
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

  // Any thread. The master volume scales music and effects together; muting
  // the output silences both, and everything keeps playing underneath. Both
  // ramp over a block like the other volumes.
  void master_volume(float volume) noexcept;
  [[nodiscard]] float master_volume() const noexcept;
  void output_muted(bool muted) noexcept;
  [[nodiscard]] bool output_muted() const noexcept;

  // Audio thread only. Fills out with the effects plus music (empty for
  // none, otherwise the same size as out). Any even size works.
  void render(std::span<int16_t> out, std::span<const int16_t> music = {});

  // Replacements for some effect slots (an enhanced sound pack), complete
  // before they are handed over and never changed afterwards. A slot left
  // empty plays its original.
  struct EffectBank {
    struct Sound {
      std::vector<int16_t> samples; // interleaved stereo, 44100 Hz
      std::size_t start_offset = 0; // where playback starts and loops back
    };
    std::array<std::unique_ptr<const Sound>, MAX_EFFECTS> slots{};
    uint64_t number = 0; // set by effect_bank()
  };
  // Game thread. Every slot changes over at once, at the next block: bank,
  // or null for the originals. Effects that are playing carry on in the new
  // sound. A bank the audio thread may still be using is kept until it has
  // moved on; the audio thread never frees one.
  void effect_bank(std::unique_ptr<EffectBank> bank);
  [[nodiscard]] bool has_effect_bank() const noexcept;
  // Game thread: frees replaced banks the audio thread is done with. With
  // no_callback (no device open), all of them.
  void collect_banks(bool no_callback);

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

  // What a slot plays this block: its original or its replacement.
  struct Sound {
    std::span<const int16_t> samples;
    std::size_t start_offset = 0;
  };
  bool post(std::size_t slot, uint32_t flags);
  static void take_request(Effect& e, const Sound& sound);
  static void mix_effect(Effect& e, const Sound& sound, std::span<float> acc,
                         float gain_from, float gain_to);
  [[nodiscard]] Sound sound_of(std::size_t slot, const EffectBank* bank) const;
  void mix_block(std::span<int16_t> out, std::span<const int16_t> music);

  std::array<Effect, MAX_EFFECTS> effects_;
  std::atomic<std::size_t> effect_count_{0};
  std::mutex add_mutex_; // one add_effect() at a time; never the audio thread
  std::atomic<uint32_t> next_request_{0};

  std::atomic<float> music_volume_{1.0f};
  std::atomic<float> sound_volume_{0.9f};
  std::atomic<bool> music_enabled_{true};
  std::atomic<float> master_volume_{1.0f};
  std::atomic<bool> output_muted_{false};

  // The bank in use (null: the originals), and the number of the bank the
  // audio thread last picked up.
  std::atomic<const EffectBank*> bank_{nullptr};
  std::atomic<uint64_t> bank_seen_{0};
  // Game thread: the current bank and the replaced ones not yet freed.
  std::unique_ptr<EffectBank> current_bank_;
  std::vector<std::unique_ptr<EffectBank>> retired_banks_;
  uint64_t banks_made_ = 0;
  // Audio thread only: the bank of the last block.
  const EffectBank* playing_bank_ = nullptr;

  // Audio thread only: gains reached at the end of the last block, and the
  // float mix of the current one.
  float music_gain_ = 1.0f;
  float sound_gain_ = 0.9f;
  std::array<float, MIX_BLOCK_SAMPLES> acc_{};
};

// How AudioManager answers an audio device event.
enum class DeviceAction {
  NONE,   // nothing to do; SDL follows default device changes itself
  REOPEN, // close the stream and open the default device again
  CLOSE,  // no playback device is left: close the stream, hold the music
};

// The decision behind AudioManager::handle_event() for an SDL audio device
// event of event_type about device which. open_device is the playback device
// in use (0 while waiting for one); live_devices is how many playback
// devices SDL lists now.
[[nodiscard]] DeviceAction device_event_action(uint32_t event_type,
                                               bool recording, uint32_t which,
                                               uint32_t open_device,
                                               int live_devices) noexcept;

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
  // Over music and effects together.
  void master_volume(float volume);
  // Mute while the game window does not have focus. Off by default.
  void mute_when_unfocused(bool enabled);
  [[nodiscard]] bool focus_muted() const;

  // Game thread: call with every SDL event. Window focus drives
  // mute_when_unfocused(). When the playback device goes away the default
  // device is opened again, or, with no device left, audio pauses until one
  // is added; the music and effects carry on from where they were.
  void handle_event(const SDL_Event& event);
  // Closes the stream and opens the default playback device again. False
  // when none opens; audio then waits for a device to be added.
  bool reopen_device();
  // The playback device in use (SDL's id for this stream), 0 when none.
  [[nodiscard]] uint32_t device() const { return device_id_; }
  [[nodiscard]] bool has_device() const { return audio_stream_ != nullptr; }

  // Where the music block comes from, and the playback generation the ring
  // must match: set together, from any thread, by the music owner only.
  // ORIGINAL with generation 0 is the default, which is the game as it
  // always was. A change of either cuts the old music at once: no audio of
  // it is played after the block that sees the change, whose last music
  // sample fades to zero within DECLICK_FRAMES so the cut does not click.
  // The new source fades in over the next block. Effects carry on as they
  // were.
  void music_route(music::Source source, uint32_t generation);
  [[nodiscard]] music::Source music_source() const;
  [[nodiscard]] uint32_t music_generation() const;
  // The ring the player's music plays from. The music worker is its only
  // producer.
  [[nodiscard]] music::PcmRing& music_ring() { return music_ring_; }
  [[nodiscard]] const music::PcmRing& music_ring() const { return music_ring_; }
  // The player's music sits this much under the FM songs (-3 dB): mastered
  // tracks are close to full scale, and the FM songs get MUSIC_BOOST.
  static constexpr float MY_MUSIC_GAIN = 0.70794578f;
  static constexpr std::size_t DECLICK_FRAMES = 64;

  // Mixes the next block of output (at most MIX_BLOCK_SAMPLES) into out.
  // The audio callback calls it. Another thread may call it only while the
  // callback cannot run: before init(), or while the device is paused
  // (SDL_PauseAudioDevice(device()) holds the device's lock until a running
  // callback is done).
  void fill_block(std::span<int16_t> out);

  // Sound effects. The enhanced pack is read from this folder (its
  // manifest.json, see docs/studio/notes/sfx-overrides.md). Game thread.
  void effects_pack(std::filesystem::path folder);
  struct EffectsChange {
    std::string error; // empty: the selection is active
    std::vector<EffectOutcome> outcomes;
    explicit operator bool() const { return error.empty(); }
  };
  // Switches every effect at once, while they play. ENHANCED reads, checks
  // and decodes the whole pack first, off the audio thread; when the pack
  // cannot be used at all the selection stays as it was. An entry that is
  // missing or refused leaves its event on the original, and no other slot
  // moves. Asked for before the effects are loaded, it is applied once they
  // are. Game thread (or the thread that loads the assets).
  EffectsChange select_effects(EffectsSet set);
  [[nodiscard]] EffectsSet effects() const;
  // SHA-256 (hex) of each original effect's VOC data as loaded, for packs to
  // name what they replace; empty for a slot not loaded.
  [[nodiscard]] std::string effect_source_sha256(SoundId id) const;

  // Stores a song (CMF or standard MIDI data) for a music slot.
  void load_music(MusicId id, std::vector<uint8_t> data);
  // Seconds into the current song, as of the last mixed block.
  [[nodiscard]] double music_position() const;

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
  // Audio thread: the next block of source into block; false when the
  // source gives no music.
  bool render_source(music::Source source, uint32_t generation,
                     std::span<int16_t> block);
  // Game thread: opens the default playback device and starts the callback;
  // false (with the reason logged) when it cannot.
  bool open_stream();
  // Game thread: stops the callback and closes the device.
  void close_stream();
  void update_focus_mute();

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
  // Written by the callback after each block.
  std::atomic<uint32_t> music_position_ms_{0};
  size_t next_music_slot_ = 0;
#endif
  // Indexed by MusicId:
  std::vector<std::vector<uint8_t>> music_data_;

  // Applies the wanted effects; effects_mutex_ held.
  EffectsChange apply_effects();

  Mixer mixer_;
  // Effects selection: game thread and the asset loader, never the callback.
  mutable std::mutex effects_mutex_;
  std::filesystem::path effects_pack_;
  EffectsSet effects_ = EffectsSet::ORIGINAL;
  EffectsSet wanted_effects_ = EffectsSet::ORIGINAL;
  bool effects_loaded_ = false;
  std::array<std::string, static_cast<std::size_t>(SoundId::COUNT)>
      effect_sources_;
  // Selection for play_sound_if_changed.
  std::optional<SoundId> current_indexed_sound_;

  // The route: the generation in the high 32 bits, the source in the low.
  std::atomic<uint64_t> music_route_{0};
  music::PcmRing music_ring_;
  // Audio thread only: the route of the last block, whether the next block
  // fades in, and the last music frame handed to the mixer.
  uint64_t playing_route_ = 0;
  bool fade_in_ = false;
  std::array<int16_t, MIX_CHANNELS> last_music_{};
  std::array<int16_t, MIX_BLOCK_SAMPLES> music_block_{};

  SDL_AudioStream* audio_stream_{nullptr};
  uint32_t device_id_ = 0;
  std::array<int16_t, MIX_BLOCK_SAMPLES> out_block_{}; // audio thread

  bool initialized_ = false;
  bool audio_enabled_ = false;
  // Game thread.
  bool window_focused_ = true;
  bool mute_when_unfocused_ = false;
};

} // namespace audio
