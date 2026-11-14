/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/music/AudioHeaders.h"
#include "engine/music/MusicTypes.h"

namespace audio {
class AudioManager;
}

namespace music {

class MusicLibrary;
struct Playlist;

// The one owner of the game's music. It alone sets the music route
// (AudioManager::music_route()), so at most one source is ever heard:
// Original (the FM songs), the player's music (Christmas background or the
// North Pole jukebox, one backend for both) or nothing.
//
// Every change of what plays (mode, play, pause, stop, skip, seek, a new
// queue) takes a new playback generation, a number that only goes up. The
// route switches to it at once; the audio callback then cuts the old source
// and drops every block of decoded audio made for an older generation,
// including any that a decoder hands over late. There is no crossfade
// between modes, and effects are never touched.
//
// Mute (AudioManager::music_enabled(false)) is not a mode: the music plays
// on silently and keeps its place. Pause holds the place of the player's
// music and makes no sound; Off plays no music at all.
//
// All public calls are for the game thread. Decoding runs on one worker
// thread, which this object owns: destroy it before SDL_Quit().
class MusicPlayback {
public:
  // The worker wakes this often to top up the ring, and on every command.
  static constexpr auto WAKE_INTERVAL = std::chrono::milliseconds(20);
  static constexpr std::size_t MAX_NOTICES = 32;
  // previous() restarts the track instead, past this far into it.
  static constexpr double RESTART_AFTER_S = 3.0;

  // When the player's music starts and stops by itself (see on_region()).
  struct Policy {
    // Set once the player has chosen a Christmas mode themselves: from then
    // on entering the North Pole starts it.
    bool autostart_north_pole = false;
    // Keep the Christmas music on after leaving the North Pole.
    bool keep_everywhere = false;
    // Stopping the jukebox goes back to the background music, when that is
    // what played before; otherwise the music stays stopped.
    bool background_after_jukebox = false;
    Mode north_pole_mode = Mode::CHRISTMAS_BACKGROUND;
  };

  // What the mini-player shows.
  struct Status {
    Mode mode = Mode::ORIGINAL;
    PlayState state = PlayState::STOPPED;
    std::uint32_t generation = 0;
    TrackId track;
    std::string title;
    std::string artist;
    std::string album;
    std::size_t queue_index = 0;
    std::size_t queue_size = 0;
    double position_s = 0.0;
    double duration_s = 0.0; // 0 when not known yet
    bool duration_exact = false;
    std::uint32_t underruns = 0;
    std::string backend; // the decoder of the track playing
    StreamInfo stream;   // its source format, for advanced settings
    bool ambience_suppressed = false;
  };

  // Where the player's music was, to resume or to save.
  struct Place {
    std::size_t index = 0;
    TrackId track;
    double position_s = 0.0;
  };

  // library (may be null) gives queue_playlist() its files and has its
  // folder scans run on the worker. Both must outlive this object.
  explicit MusicPlayback(audio::AudioManager& audio,
                         MusicLibrary* library = nullptr);
  ~MusicPlayback();
  MusicPlayback(const MusicPlayback&) = delete;
  MusicPlayback& operator=(const MusicPlayback&) = delete;

  /* Modes */

  // Selects what plays. Choosing background turns the jukebox off and the
  // other way round, being one setting. A Christmas mode resumes the queue
  // where it was (playing, unless the player had paused or stopped it).
  void set_mode(Mode mode);
  [[nodiscard]] Mode mode() const { return mode_; }

  /* The player's music: ignored outside the Christmas modes */

  void play();
  void pause();
  void toggle_pause();
  // Back to the start of the track; play() goes on from there. In jukebox
  // mode, with Policy::background_after_jukebox and background before it,
  // the background music comes back instead.
  void stop();
  void next();
  void previous();
  void seek(double seconds);
  [[nodiscard]] PlayState play_state() const;

  // Replaces the queue and starts at first, position_s into it.
  void set_queue(std::vector<QueuedTrack> tracks, std::size_t first = 0,
                 double position_s = 0.0);
  // Queues a playlist's tracks; ones the library has no copy of stay in the
  // queue and are skipped with a notice when their turn comes.
  void queue_playlist(const Playlist& playlist, std::size_t first = 0);
  // Changes the queue without interrupting the song playing: when its id is
  // still in tracks it plays on, and the queue goes on from there.
  void edit_queue(std::vector<QueuedTrack> tracks);
  [[nodiscard]] const std::vector<QueuedTrack>& queue() const { return queue_; }
  void set_repeat(Repeat repeat);
  [[nodiscard]] Repeat repeat() const { return repeat_; }
  // Shuffles the tracks after the one playing; off keeps the order as it is.
  void set_shuffle(bool shuffle);
  [[nodiscard]] bool shuffle() const { return shuffle_; }
  [[nodiscard]] Place place() const;

  void set_backend(Backend backend);
  void set_limits(const Limits& limits);
  // AudioManager::music_volume, for every mode.
  void volume(float volume);

  /* Places in the game */

  // A restaurant view opens with its jukebox on original song choice (0 for
  // none, 1 to 6), or the jukebox popup changes it, or the view closes.
  // They drive the FM player exactly as the views always did; in the other
  // modes the FM player is not heard, so the player's music is never
  // stopped or replaced by a restaurant.
  void on_restaurant_enter(int original_choice);
  void on_restaurant_jukebox(int original_choice);
  void on_restaurant_leave();
  // Whether restaurant ambience should be quiet: an original jukebox song
  // playing in Original mode, as before, or the jukebox mode playing.
  [[nodiscard]] bool ambience_suppressed() const;

  // The player arrives in the North Pole region (true) or anywhere else.
  // Entering the same region again changes nothing, so a song never
  // restarts at a boundary. Off and a paused or stopped queue are left
  // alone.
  void on_region(bool north_pole);
  void set_policy(const Policy& policy) { policy_ = policy; }
  [[nodiscard]] const Policy& policy() const { return policy_; }

  /* State */

  [[nodiscard]] Status status() const;
  [[nodiscard]] std::uint32_t generation() const { return generation_; }
  // Track changes, skips and stops since the last call, oldest first.
  [[nodiscard]] std::vector<Notice> poll_notices();
  // Notices dropped because nobody polled them in time.
  [[nodiscard]] std::uint32_t dropped_notices() const;

private:
  struct Worker;

  // chosen: the player picked the mode, rather than a region policy.
  void switch_mode(Mode mode, bool chosen);
  // A new generation, routed to the audio thread before the worker hears
  // of it.
  std::uint32_t next_generation(Source source);
  void start(std::size_t index, std::uint64_t frame);
  void halt();
  void send_queue(std::size_t keep_index);
  // Takes the worker's news: a queue played to its end.
  void sync();
  [[nodiscard]] Place heard_place() const;
  [[nodiscard]] static bool christmas(Mode mode) {
    return mode == Mode::CHRISTMAS_BACKGROUND || mode == Mode::JUKEBOX;
  }

  audio::AudioManager& audio_;
  MusicLibrary* library_;
  std::unique_ptr<Worker> worker_;

  Mode mode_ = Mode::ORIGINAL;
  PlayState state_ = PlayState::STOPPED;
  std::uint32_t generation_ = 0;
  std::vector<QueuedTrack> queue_;
  Repeat repeat_ = Repeat::ALL;
  bool shuffle_ = false;
  // Where the last start() began; the place until something is heard.
  Place started_;
  // The place kept while the player's music is not producing.
  Place held_;
  Policy policy_;
  // The mode before the North Pole switched it, to go back to.
  std::optional<Mode> outside_mode_;
  std::optional<bool> in_north_pole_;
  // The mode before the jukebox, for Policy::background_after_jukebox.
  Mode before_jukebox_ = Mode::ORIGINAL;
  bool in_restaurant_ = false;
  int restaurant_choice_ = 0;
};

} // namespace music
