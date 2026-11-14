/*
 * SPDX-FileCopyrightText: 2026 ShadowByte
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "engine/music/MusicPlayback.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <stop_token>
#include <thread>

#include "engine/audio/AudioManager.h"
#include "engine/music/CommandQueue.h"
#include "engine/music/MusicLibrary.h"
#include "engine/music/MusicSettings.h"
#include "engine/music/PcmRing.h"
#include "engine/music/TrackDecoder.h"
#include "engine/music/VlcDecoder.h"

namespace music {

namespace {

constexpr double output_rate = TrackReader::OUTPUT_RATE;
// Frames decoded at a time; the ring is topped up to within this of full.
constexpr std::size_t chunk_frames = 1024;
// Tracks opened at most per wake, so a run of short or broken tracks
// cannot keep the worker from its commands.
constexpr int max_steps_per_wake = 256;
// Opened tracks remembered for status().
constexpr std::size_t kept_details = 16;

std::uint64_t to_frames(double seconds) {
  return seconds > 0.0 ? static_cast<std::uint64_t>(seconds * output_rate) : 0;
}

std::string name_of(const QueuedTrack& track) {
  return track.title.empty() ? track.id : track.title;
}

Source source_for(Mode mode) {
  switch (mode) {
  case Mode::ORIGINAL:
    return Source::ORIGINAL;
  case Mode::CHRISTMAS_BACKGROUND:
  case Mode::JUKEBOX:
    return Source::MY_MUSIC;
  case Mode::OFF:
    break;
  }
  return Source::OFF;
}

} // namespace

/* The worker */

struct MusicPlayback::Worker {
  // A track the worker opened, for status().
  struct Details {
    std::uint32_t serial = 0;
    std::uint32_t generation = 0;
    std::size_t index = 0;
    QueuedTrack track;
    StreamInfo info;
    std::uint64_t duration = 0; // output frames, 0 when not known
    std::string backend;
  };

  Worker(PcmRing& ring_in, MusicLibrary* library_in)
      : ring(ring_in), library(library_in),
        chunk(chunk_frames * PcmRing::CHANNELS),
        thread([this](const std::stop_token& stop) { run(stop); }) {}

  ~Worker() {
    thread.request_stop();
    // The wait below wakes on the stop request; joining happens in
    // ~jthread, before the members it uses go away.
    if (thread.joinable()) {
      thread.join();
    }
  }
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  // Game thread.
  void post(Command command) {
    {
      const std::lock_guard lock(mutex);
      commands.push(std::move(command));
    }
    wake.notify_one();
  }

  // Any thread: the library has work for the worker.
  void wake_for_library() {
    {
      const std::lock_guard lock(mutex);
      library_wake = true;
    }
    wake.notify_one();
  }

  void notify(Notice::Kind kind, const TrackId& track, std::string text) {
    const std::lock_guard lock(mutex);
    if (notices.size() == MAX_NOTICES) {
      notices.pop_front();
      ++dropped;
    }
    notices.push_back(
        Notice{.kind = kind, .track = track, .text = std::move(text)});
  }

  void run(const std::stop_token& stop) {
    bool library_busy = false;
    while (!stop.stop_requested()) {
      std::vector<Command> taken;
      {
        std::unique_lock lock(mutex);
        if (!library_busy) {
          wake.wait_for(lock, stop, WAKE_INTERVAL,
                        [this] { return !commands.empty() || library_wake; });
        }
        taken = commands.take();
        library_wake = false;
        backend = config_backend;
        limits = config_limits;
      }
      for (Command& c : taken) {
        apply(c);
      }
      fill();
      library_busy = library && library->run_step();
    }
    reader.close();
    ring.producing(0);
  }

  void apply(Command& c) {
    switch (c.kind) {
    case Command::Kind::QUEUE: {
      queue = std::move(c.tracks);
      repeat = c.repeat;
      if (!reader.is_open()) {
        break;
      }
      // The track being decoded plays on, from its place in the new list.
      // The owner's hint is for the track heard; the worker may be a track
      // ahead of that, so it goes by its own.
      std::size_t at = Command::NO_INDEX;
      if (c.keep_index < queue.size() && queue[c.keep_index].id == current.id) {
        at = c.keep_index;
      }
      else {
        const auto it =
            std::find_if(queue.begin(), queue.end(), [&](const QueuedTrack& t) {
              return t.id == current.id;
            });
        if (it != queue.end()) {
          at = static_cast<std::size_t>(it - queue.begin());
        }
      }
      if (at != Command::NO_INDEX) {
        index = at;
        after_current = Command::NO_INDEX;
        const std::lock_guard lock(mutex);
        for (Details& d : details) {
          if (d.serial == serial) {
            d.index = index;
          }
        }
      }
      else {
        // Gone from the list: it ends, and the list goes on at its place.
        after_current = std::min(index, queue.size());
      }
      break;
    }
    case Command::Kind::PLAY:
      reader.close();
      generation = c.generation;
      index = c.index;
      start_frame = c.frame;
      active = index < queue.size();
      failures = 0;
      after_current = Command::NO_INDEX;
      ring.producing(0);
      break;
    case Command::Kind::HALT:
      reader.close();
      generation = c.generation;
      active = false;
      ring.producing(0);
      break;
    }
  }

  void fill() {
    for (int steps = 0; active && steps < max_steps_per_wake; ++steps) {
      if (ring.writable_frames() < chunk_frames) {
        return; // topped up
      }
      if (!reader.is_open() && !open_track()) {
        continue;
      }
      const std::size_t got = reader.read(chunk);
      if (got > 0) {
        ring.write(std::span(chunk).first(got * PcmRing::CHANNELS),
                   PcmRing::Tag{.generation = generation,
                                .track = serial,
                                .frame = frame});
        frame += got;
        failures = 0;
        ring.producing(generation);
        continue;
      }
      if (!reader.at_end()) {
        return; // nothing for now; more comes later
      }
      const bool damaged = reader.damaged();
      if (damaged) {
        notify(Notice::Kind::SKIPPED, current.id,
               name_of(current) + " stopped early: the file is damaged");
      }
      reader.close();
      advance(damaged);
    }
  }

  bool open_track() {
    if (index >= queue.size()) {
      finish("The playlist has ended");
      return false;
    }
    const QueuedTrack track = queue[index];
    std::string why;
    std::unique_ptr<TrackDecoder> decoder;
    if (track.file.empty()) {
      why = "it is not in the music folders";
    }
    else {
      if (backend != Backend::BUILTIN && !vlc_tried) {
        vlc_tried = true;
        std::string vlc_error;
        vlc = load_vlc(&vlc_error);
      }
      decoder = open_decoder(track.file, limits, backend, vlc, &why);
    }
    if (!decoder || !reader.open(std::move(decoder), &why)) {
      notify(Notice::Kind::SKIPPED, track.id,
             "Skipped " + name_of(track) + ": " + why);
      start_frame = 0;
      if (++failures >= queue.size()) {
        finish("None of the tracks could be played");
        return false;
      }
      advance(true);
      return false;
    }
    current = track;
    frame = 0;
    if (start_frame > 0 && reader.seek(start_frame)) {
      frame = start_frame;
    }
    start_frame = 0;
    ++serial;
    {
      const std::lock_guard lock(mutex);
      details.push_back(Details{
          .serial = serial,
          .generation = generation,
          .index = index,
          .track = track,
          .info = reader.info(),
          .duration = reader.duration(),
          .backend = std::string(reader.backend()),
      });
      if (details.size() > kept_details) {
        details.pop_front();
      }
    }
    notify(Notice::Kind::TRACK_STARTED, track.id, "Playing " + name_of(track));
    return true;
  }

  void advance(bool failed) {
    if (after_current != Command::NO_INDEX) {
      index = after_current;
      after_current = Command::NO_INDEX;
    }
    else if (repeat != Repeat::ONE || failed) {
      ++index;
    }
    if (index >= queue.size()) {
      if (repeat != Repeat::OFF && !queue.empty()) {
        index = 0;
      }
      else {
        finish("The playlist has ended");
      }
    }
  }

  void finish(std::string text) {
    active = false;
    ring.producing(0);
    {
      const std::lock_guard lock(mutex);
      ended_generation = generation;
    }
    notify(Notice::Kind::STOPPED, {}, std::move(text));
  }

  PcmRing& ring;
  MusicLibrary* library;

  // Shared with the game thread, under mutex.
  mutable std::mutex mutex;
  std::condition_variable_any wake;
  CommandQueue commands;
  bool library_wake = false;
  std::deque<Notice> notices;
  std::uint32_t dropped = 0;
  std::deque<Details> details;
  std::uint32_t ended_generation = 0;
  Backend config_backend = Backend::AUTO;
  Limits config_limits;

  // Worker thread only.
  Backend backend = Backend::AUTO;
  Limits limits;
  std::vector<QueuedTrack> queue;
  Repeat repeat = Repeat::OFF;
  std::uint32_t generation = 0;
  bool active = false;
  std::size_t index = 0;
  std::size_t after_current = Command::NO_INDEX;
  std::uint64_t start_frame = 0;
  std::uint64_t frame = 0;
  std::uint32_t serial = 0;
  std::size_t failures = 0;
  QueuedTrack current; // the track the reader has open
  TrackReader reader;
  std::vector<std::int16_t> chunk;
  std::shared_ptr<VlcRuntime> vlc;
  bool vlc_tried = false;

  // Last: it starts once everything above exists.
  std::jthread thread;
};

/* MusicPlayback */

MusicPlayback::MusicPlayback(audio::AudioManager& audio, MusicLibrary* library)
    : audio_(audio), library_(library),
      worker_(std::make_unique<Worker>(audio.music_ring(), library)) {
  if (library_) {
    Worker* worker = worker_.get();
    library_->attach_worker([worker] { worker->wake_for_library(); });
  }
}

MusicPlayback::~MusicPlayback() {
  if (library_) {
    library_->attach_worker({});
  }
  worker_.reset();
  // Nothing produces for the ring any more.
  audio_.music_route(Source::ORIGINAL, ++generation_);
}

std::uint32_t MusicPlayback::next_generation(Source source) {
  ++generation_;
  // The audio thread hears of the new generation first, so nothing the
  // worker makes for it is ever taken for stale.
  audio_.music_route(source, generation_);
  return generation_;
}

void MusicPlayback::start(std::size_t index, std::uint64_t frame) {
  if (queue_.empty()) {
    state_ = PlayState::STOPPED;
    return;
  }
  index = std::min(index, queue_.size() - 1);
  started_ = Place{.index = index,
                   .track = queue_[index].id,
                   .position_s = static_cast<double>(frame) / output_rate};
  held_ = started_;
  state_ = PlayState::PLAYING;
  Command c;
  c.kind = Command::Kind::PLAY;
  c.generation = next_generation(Source::MY_MUSIC);
  c.index = index;
  c.frame = frame;
  worker_->post(std::move(c));
}

void MusicPlayback::halt() {
  Command c;
  c.kind = Command::Kind::HALT;
  c.generation = next_generation(source_for(mode_));
  worker_->post(std::move(c));
}

void MusicPlayback::send_queue(std::size_t keep_index) {
  Command c;
  c.kind = Command::Kind::QUEUE;
  c.tracks = queue_;
  c.repeat = repeat_;
  c.keep_index = keep_index;
  worker_->post(std::move(c));
}

void MusicPlayback::sync() {
  if (state_ != PlayState::PLAYING) {
    return;
  }
  bool ended = false;
  {
    const std::lock_guard lock(worker_->mutex);
    ended = worker_->ended_generation == generation_;
  }
  if (ended) {
    state_ = PlayState::STOPPED;
    held_ = Place{};
    if (!queue_.empty()) {
      held_.track = queue_.front().id;
    }
  }
}

PlayState MusicPlayback::play_state() const {
  if (state_ == PlayState::PLAYING) {
    const std::lock_guard lock(worker_->mutex);
    if (worker_->ended_generation == generation_) {
      return PlayState::STOPPED;
    }
  }
  return state_;
}

MusicPlayback::Place MusicPlayback::heard_place() const {
  if (!christmas(mode_) || state_ != PlayState::PLAYING) {
    return held_;
  }
  const PcmRing::Tag heard = audio_.music_ring().heard();
  if (heard.generation == generation_) {
    const std::lock_guard lock(worker_->mutex);
    for (const Worker::Details& d : worker_->details) {
      if (d.serial == heard.track) {
        return Place{.index = d.index,
                     .track = d.track.id,
                     .position_s =
                         static_cast<double>(heard.frame) / output_rate};
      }
    }
  }
  return started_;
}

MusicPlayback::Place MusicPlayback::place() const { return heard_place(); }

void MusicPlayback::set_mode(Mode mode) { switch_mode(mode, true); }

void MusicPlayback::switch_mode(Mode mode, bool chosen) {
  sync();
  const Mode old = mode_;
  if (mode == old) {
    return;
  }
  if (chosen && christmas(mode)) {
    policy_.autostart_north_pole = true;
    policy_.north_pole_mode = mode;
  }
  else if (chosen) {
    outside_mode_.reset(); // the player chose; nothing to go back to
  }
  if (mode == Mode::JUKEBOX) {
    before_jukebox_ = old;
  }
  if (christmas(old) && state_ == PlayState::PLAYING) {
    held_ = heard_place();
  }
  mode_ = mode;
  // Into a Christmas mode the music plays on (or starts), unless paused.
  if (christmas(mode) && state_ != PlayState::PAUSED && !queue_.empty()) {
    start(held_.index, to_frames(held_.position_s));
    return;
  }
  halt();
}

void MusicPlayback::play() {
  sync();
  if (!christmas(mode_) || state_ == PlayState::PLAYING) {
    return;
  }
  start(held_.index, to_frames(held_.position_s));
}

void MusicPlayback::pause() {
  sync();
  if (!christmas(mode_) || state_ != PlayState::PLAYING) {
    return;
  }
  held_ = heard_place();
  state_ = PlayState::PAUSED;
  halt();
}

void MusicPlayback::toggle_pause() {
  if (play_state() == PlayState::PLAYING) {
    pause();
  }
  else {
    play();
  }
}

void MusicPlayback::stop() {
  sync();
  if (!christmas(mode_)) {
    return;
  }
  const Place at = heard_place();
  if (mode_ == Mode::JUKEBOX && policy_.background_after_jukebox &&
      before_jukebox_ == Mode::CHRISTMAS_BACKGROUND) {
    held_ = at;
    mode_ = Mode::CHRISTMAS_BACKGROUND;
    start(at.index, to_frames(at.position_s));
    return;
  }
  held_ = Place{.index = at.index, .track = at.track, .position_s = 0.0};
  state_ = PlayState::STOPPED;
  halt();
}

void MusicPlayback::next() {
  sync();
  if (!christmas(mode_) || queue_.empty()) {
    return;
  }
  std::size_t target = heard_place().index + 1;
  if (target >= queue_.size()) {
    if (repeat_ == Repeat::OFF) {
      held_ = Place{.index = 0, .track = queue_.front().id, .position_s = 0.0};
      if (state_ == PlayState::PLAYING) {
        state_ = PlayState::STOPPED;
        halt();
      }
      return;
    }
    target = 0;
  }
  if (state_ == PlayState::PLAYING) {
    start(target, 0);
  }
  else {
    held_ =
        Place{.index = target, .track = queue_[target].id, .position_s = 0.0};
  }
}

void MusicPlayback::previous() {
  sync();
  if (!christmas(mode_) || queue_.empty()) {
    return;
  }
  const Place at = heard_place();
  std::size_t target = at.index;
  if (at.position_s <= RESTART_AFTER_S) {
    target = at.index > 0             ? at.index - 1
             : repeat_ == Repeat::ALL ? queue_.size() - 1
                                      : 0;
  }
  if (state_ == PlayState::PLAYING) {
    start(target, 0);
  }
  else {
    held_ =
        Place{.index = target, .track = queue_[target].id, .position_s = 0.0};
  }
}

void MusicPlayback::seek(double seconds) {
  sync();
  if (!christmas(mode_) || queue_.empty()) {
    return;
  }
  const Place at = heard_place();
  if (state_ == PlayState::PLAYING) {
    start(at.index, to_frames(seconds));
  }
  else {
    held_ = Place{.index = at.index,
                  .track = at.track,
                  .position_s = std::max(seconds, 0.0)};
  }
}

void MusicPlayback::set_queue(std::vector<QueuedTrack> tracks,
                              std::size_t first, double position_s) {
  sync();
  queue_ = std::move(tracks);
  send_queue(Command::NO_INDEX);
  if (queue_.empty()) {
    held_ = Place{};
    if (state_ == PlayState::PLAYING) {
      state_ = PlayState::STOPPED;
      halt();
    }
    return;
  }
  first = std::min(first, queue_.size() - 1);
  held_ = Place{.index = first,
                .track = queue_[first].id,
                .position_s = std::max(position_s, 0.0)};
  if (christmas(mode_) && state_ != PlayState::PAUSED) {
    start(first, to_frames(position_s));
  }
}

void MusicPlayback::queue_playlist(const Playlist& playlist,
                                   std::size_t first) {
  std::vector<QueuedTrack> tracks;
  tracks.reserve(playlist.tracks.size());
  for (const TrackId& id : playlist.tracks) {
    QueuedTrack t{.id = id, .file = {}, .title = {}};
    if (library_) {
      if (const auto info = library_->find(id)) {
        t.title = info->title;
        t.file = library_->file_of(*info);
      }
    }
    tracks.push_back(std::move(t));
  }
  set_queue(std::move(tracks), first);
}

void MusicPlayback::edit_queue(std::vector<QueuedTrack> tracks) {
  sync();
  const Place at = heard_place();
  queue_ = std::move(tracks);
  const auto found =
      std::find_if(queue_.begin(), queue_.end(),
                   [&](const QueuedTrack& t) { return t.id == at.track; });
  const bool kept = found != queue_.end();
  const std::size_t keep =
      kept ? static_cast<std::size_t>(found - queue_.begin())
           : Command::NO_INDEX;
  send_queue(state_ == PlayState::PLAYING ? keep : Command::NO_INDEX);
  if (kept) {
    held_.index = keep;
    started_.index = keep;
  }
  else if (!queue_.empty()) {
    held_ = Place{.index = std::min(at.index, queue_.size() - 1),
                  .track = {},
                  .position_s = 0.0};
    held_.track = queue_[held_.index].id;
  }
  else {
    held_ = Place{};
  }
}

void MusicPlayback::set_repeat(Repeat repeat) {
  repeat_ = repeat;
  edit_queue(queue_);
}

void MusicPlayback::set_shuffle(bool shuffle) {
  shuffle_ = shuffle;
  if (!shuffle || queue_.size() < 2) {
    return;
  }
  std::vector<QueuedTrack> order = queue_;
  const std::size_t from = std::min(heard_place().index + 1, order.size());
  std::mt19937 rng{std::random_device{}()};
  std::shuffle(order.begin() + static_cast<std::ptrdiff_t>(from), order.end(),
               rng);
  edit_queue(std::move(order));
}

void MusicPlayback::set_backend(Backend backend) {
  const std::lock_guard lock(worker_->mutex);
  worker_->config_backend = backend;
}

void MusicPlayback::set_limits(const Limits& limits) {
  const std::lock_guard lock(worker_->mutex);
  worker_->config_limits = limits;
}

void MusicPlayback::volume(float volume) { audio_.music_volume(volume); }

void MusicPlayback::on_restaurant_enter(int original_choice) {
  in_restaurant_ = true;
  restaurant_choice_ = original_choice;
  if (original_choice > 0) {
    audio_.play_music(static_cast<audio::MusicId>(original_choice - 1));
  }
}

void MusicPlayback::on_restaurant_jukebox(int original_choice) {
  restaurant_choice_ = original_choice;
  if (original_choice == 0) {
    audio_.stop_music();
  }
  else {
    audio_.play_music(static_cast<audio::MusicId>(original_choice - 1));
  }
}

void MusicPlayback::on_restaurant_leave() {
  in_restaurant_ = false;
  restaurant_choice_ = 0;
  audio_.stop_music();
}

bool MusicPlayback::ambience_suppressed() const {
  switch (mode_) {
  case Mode::ORIGINAL:
    return in_restaurant_ && restaurant_choice_ > 0;
  case Mode::JUKEBOX:
    return play_state() == PlayState::PLAYING;
  case Mode::CHRISTMAS_BACKGROUND:
  case Mode::OFF:
    break;
  }
  return false;
}

void MusicPlayback::on_region(bool north_pole) {
  if (in_north_pole_ == north_pole) {
    return;
  }
  in_north_pole_ = north_pole;
  sync();
  if (north_pole) {
    if (policy_.autostart_north_pole && mode_ == Mode::ORIGINAL &&
        christmas(policy_.north_pole_mode)) {
      outside_mode_ = mode_;
      switch_mode(policy_.north_pole_mode, false);
    }
    return;
  }
  const std::optional<Mode> back = outside_mode_;
  outside_mode_.reset();
  if (back && !policy_.keep_everywhere && christmas(mode_)) {
    switch_mode(*back, false);
  }
}

MusicPlayback::Status MusicPlayback::status() const {
  Status s;
  s.mode = mode_;
  s.state = play_state();
  s.generation = generation_;
  s.queue_size = queue_.size();
  s.underruns = audio_.music_ring().underruns();
  s.ambience_suppressed = ambience_suppressed();
  const Place at = heard_place();
  s.queue_index = at.index;
  s.track = at.track;
  s.position_s = at.position_s;
  if (at.index < queue_.size()) {
    s.title = queue_[at.index].title;
  }
  const std::lock_guard lock(worker_->mutex);
  for (auto it = worker_->details.rbegin(); it != worker_->details.rend();
       ++it) {
    if (it->track.id == at.track && !at.track.empty()) {
      s.title = name_of(it->track);
      if (!it->info.title.empty()) {
        s.title = it->info.title;
      }
      s.artist = it->info.artist;
      s.album = it->info.album;
      s.duration_s = static_cast<double>(it->duration) / output_rate;
      s.duration_exact = it->info.frames_exact;
      s.backend = it->backend;
      s.stream = it->info;
      break;
    }
  }
  return s;
}

std::vector<Notice> MusicPlayback::poll_notices() {
  sync();
  const std::lock_guard lock(worker_->mutex);
  std::vector<Notice> out(worker_->notices.begin(), worker_->notices.end());
  worker_->notices.clear();
  return out;
}

std::uint32_t MusicPlayback::dropped_notices() const {
  const std::lock_guard lock(worker_->mutex);
  return worker_->dropped;
}

} // namespace music
