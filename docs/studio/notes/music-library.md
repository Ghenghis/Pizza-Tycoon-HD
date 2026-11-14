# Music library (R35): design

Design only, 2026-11-14. No code, builds, imports or listening tests ran for it. Written against
`main` at `39c56ee` plus this round's audio work (the `audio::Mixer`, master volume, device
handling in `AudioManager::handle_event`). It covers the "My music" half of R35
(`RECORDING-AND-MUSIC.md`, "User-facing music workflow"); recording is a separate design.

## 1. What the player gets

- One music switch with three settings: **Original** (the 1994 FM songs, as today), **My music**
  (the player's own files) and **Off**. Effects, music and master volume stay separate sliders.
- My music plays named playlists of files the player picked. Add, remove, reorder, repeat,
  shuffle, next and previous. An optional short crossfade, off by default. A **Restore original**
  action is always one step away.
- A missing, corrupt or unsupported track is skipped, with a message naming it. It never stops
  the game. The playlist itself stays as it was; nothing is removed behind the player's back.
- Optional event playlists, such as a Christmas playlist for the calendar's Christmas event. The
  player turns each one on; nothing switches on by region, locale or anything else about them.
- Files from Google Drive are read as metadata first. Only files the player picks are imported,
  and their remote identity is kept. Nothing is swept, uploaded, shared or sent anywhere.

## 2. Where it attaches today

- `audio::Mixer::render(out, music)` already takes the music as one int16 stereo block per
  callback block, with its own ramped gain (`music_volume`, the music switch, now also
  `master_volume` and the focus mute). `AudioManager::fill_block()` decides what that block is.
  Today it is the FM player (`render_music`). My music becomes a second source for the same
  block. The Mixer itself does not change.
- The threading model at the top of `AudioManager.h` stays the rule. The callback never waits,
  never allocates, and never touches files or decoders.
- Device loss and change are already handled (`AudioManager::handle_event`). The music source
  does not depend on the device, so a reopened device continues where playback was.
- Preferences live in the config file (`GameOptions`, `ConfigManager`). Game saves have a
  cosmetic `presentation` block, kept out of the fingerprint (location contract §6.5). Writes that
  must survive a crash go through `util::AtomicFile`.

## 3. Threads and data flow

```
game thread                    music worker (one thread)              audio thread (SDL)
-----------                    -------------------------              ------------------
MusicLibrary   --commands-->   file I/O, decoder init,                AudioManager::fill_block
PlaylistStore  (bounded        decode, SDL_AudioStream resample,      reads the PCM ring
MusicPlayback   queue)         crossfade, fills the PCM ring   --->   (lock-free, no waits)
EventMusicPolicy <--notices--  (skips, errors, track changes)         -> Mixer::render(out, music)
```

- **Commands** (game to worker): a bounded queue of 64 entries. The game thread never blocks on
  it: when it is full, the newest command of a kind replaces the queued one (two "next" presses
  become one "next twice"; a newer playlist replaces an older one).
- **PCM ring** (worker to callback): a single-producer, single-consumer ring of int16 stereo
  frames at the stream's 44100 Hz, sized at 2^15 frames (0.74 s, 128 KiB) and allocated once.
  The read and write indices are atomics, with acquire and release ordering. The callback takes
  what it needs for a block. A shortfall plays as silence and adds to an atomic underrun counter,
  which is shown in the debug overlay. The callback never signals the worker. The worker wakes
  on a command or every 20 ms, and tops the ring up to its high-water mark.
- **Notices** (worker to game): a bounded queue of messages ("Skipped <title>: file is damaged"),
  which the game thread polls once a frame and shows. When it is full, the oldest are dropped
  and a counter says how many.
- **Imports** run on the same worker, after playback commands, in chunks: 1 MiB of hashing or
  copying at a time. A large import cannot delay the next block of music.

## 4. Interfaces

These are proposed APIs, in the codebase's style. R35's names map one to one:
`MusicLibrary.importSelected` is `import_selected`, `PlaylistStore.save` is `save`,
`MusicPlayback.setSource/queue/volume` are `set_source`, `queue` and `volume`, and
`EventMusicPolicy.resolve` is `resolve`. All of them are called from the game thread.

```cpp
namespace music {

// Stable keys in the studio's namespace:slug form (ADR W02-02):
//   trk:<32 hex>  the first 128 bits of the SHA-256 of the file's bytes
//   pl:<16 hex>   random when the playlist is made, then fixed
using TrackId = std::string;
using PlaylistId = std::string;

enum class Format { WAV, FLAC, MP3, OGG_VORBIS };

struct RemoteSource {      // where an imported file came from; metadata only
  std::string provider;    // "gdrive"
  std::string file_id;     // the provider's id
  std::string revision_id; // empty when the provider has none
  std::string checksum;    // e.g. Drive's md5Checksum, when given
  std::string name, mime_type;
  std::uint64_t size = 0;
};

struct TrackInfo {
  TrackId id;
  std::string title, artist, album; // display only: from tags or the file name, valid UTF-8,
                                    // 200 bytes at most
  std::string sha256;               // the whole file, hex
  std::uint64_t bytes = 0;
  Format format = Format::WAV;
  std::uint32_t sample_rate = 0;    // as decoded
  std::uint8_t channels = 0;
  std::uint64_t frames = 0;         // duration = frames / sample_rate
  float gain_db = 0.0f;             // loudness correction, 0 unless the player turns it on (§5)
  std::optional<RemoteSource> remote;
  bool missing = false;             // the local copy is gone; skipped, never deleted
};

class MusicLibrary {
public:
  struct Selection {
    std::filesystem::path file;         // a local file the player picked
    std::optional<RemoteSource> remote; // filled when it came from Drive
  };
  // Copies the picked files into the private library on the worker. Returns at once;
  // the outcome of each file arrives through poll_imports().
  ImportJob import_selected(std::vector<Selection> files);
  std::vector<ImportResult> poll_imports(); // imported, already there (same id), or refused
                                            // with a reason
  std::optional<TrackInfo> find(const TrackId& id) const;
  std::vector<TrackInfo> tracks() const;
  // Deletes the private copy. Playlists keep the id, shown as missing until the player
  // removes it there.
  bool remove(const TrackId& id);
  struct Usage { std::uint64_t bytes, quota_bytes; std::size_t tracks; };
  Usage usage() const;
};

enum class Repeat { OFF, ALL, ONE };

struct Playlist {
  PlaylistId id;
  std::string name;              // UTF-8, 100 bytes at most
  std::vector<TrackId> tracks;   // order as the player arranged it; ids may be missing
  bool shuffle = false;
  Repeat repeat = Repeat::ALL;
  std::uint16_t crossfade_ms = 0; // 0 to 5000
  std::uint32_t revision = 0;    // +1 on every save; a stale save is refused, not merged
};

class PlaylistStore {
public:
  std::vector<Playlist> list() const;
  std::optional<Playlist> get(const PlaylistId& id) const;
  // Creates or replaces one playlist, written with util::AtomicFile. Refused (with the reason)
  // when the revision is stale or the name is empty.
  SaveResult save(const Playlist& playlist);
  bool remove(const PlaylistId& id);
  PlaylistId create(std::string name); // a fresh pl:<hex>
};

enum class Source { ORIGINAL, MY_MUSIC, OFF };

class MusicPlayback {
public:
  void set_source(Source source);              // ramps out, switches, ramps in
  void queue(const PlaylistId& playlist, std::size_t first = 0); // replaces the queue
  void next();
  void previous();                             // restarts the track when past 3 s
  void volume(float v);                        // the existing Mixer::music_volume
  struct Status {
    Source source;
    TrackId track;
    double position_s;
    std::size_t queue_index;
    std::uint32_t underruns;
  };
  Status status() const;                       // a snapshot, without waiting on the worker
  std::vector<Notice> poll_notices();          // skips and errors, to show the player
};

struct EventMusicRule {    // the player opted in to this
  std::string event_id;    // a calendar event id (W02-18), e.g. "pthd:christmas"
  PlaylistId playlist;
};

enum class AfterEvent { RESUME, RESTART }; // back to the normal queue where it was, or from the top

class EventMusicPolicy {
public:
  // Pure: what should play, from the active calendar events (the W02-18 resolution), the
  // player's rules and their normal playlist. With events off, or no rule matching, the
  // normal playlist. Two matching rules: the first in the player's order.
  struct Resolution { PlaylistId playlist; std::string because_of_event; };
  static Resolution resolve(std::span<const std::string> active_events,
                            std::span<const EventMusicRule> rules,
                            const PlaylistId& normal, bool events_enabled);
};

} // namespace music
```

When an event playlist takes over, `MusicPlayback` remembers the normal queue: playlist, track
and position. When the event ends it goes back according to `AfterEvent`, a setting that defaults
to `RESUME`. The real date is used only if the player turned that on, as W02-18 says. The manual
preview switches back when it is closed.

## 5. From a file to the Mixer

1. The worker opens the file with `SDL_IOFromFile` (UTF-8 paths; SDL converts to wide paths on
   Windows) and checks the format's header before any decoder runs. Same rule as `gfx::PngLimits`.
2. The decoder produces PCM at the file's rate and channel count.
3. An `SDL_AudioStream` created on the worker converts that to the callback's format: int16
   stereo at 44100 Hz. It resamples, downmixes surround to stereo and copies mono to both sides.
   SDL 3.4's headers (the SDL wiki is generated from them) document `SDL_CreateAudioStream`,
   `SDL_PutAudioStreamData` and `SDL_GetAudioStreamData` as safe to call from any thread. This
   stream is never bound to a device, so no callback locking applies.
4. Crossfade and track changes are done on the worker. It opens the next track ahead of time
   and, when crossfading, mixes A's tail with B's head using equal-power ramps. The callback
   only ever sees one ring.
5. In `fill_block`, the source decides the music block: the FM player (Original), the ring
   (My music) or silence (Off). A switch ramps through the Mixer's existing music gain.
   - The FM player is not run while My music plays, so Original continues where it stopped.
   - While My music plays, the worker keeps decoding, so a short device change does not cut
     the song.
   - While no device is open the callback is not called, the ring stays full, the worker idles,
     and the position holds.
6. **Headroom.** The FM boost (`MUSIC_BOOST`) applies only to Original. Mastered tracks sit close
   to full scale, so My music starts with a fixed -3 dB trim under the Mixer's 0.8 headroom.
   Loud tracks may then saturate together with effects; the mix saturates rather than wraps.
7. **Loudness.** Off by default. When the player turns on "Even out loudness", each track's
   `gain_db` is applied. That value comes from its ReplayGain tag, or from an integrated loudness
   (EBU R128) measured once at import. It is only ever an attenuation (at most 0 dB), so
   headroom is kept. It is stored apart from the file and never written into it, so turning the
   option off restores the original level exactly.

## 6. Storage, ids and privacy

- **Folder.** Everything lives under a new `platform_paths::music_dir()`, inside the per-user
  folder (`base_dir()`, SDL's preferences path), so it is local to the device:
  - `library.json`: the track index;
  - `playlists.json`;
  - `tracks/<first 2 hex>/<track id>.<ext>`: the private copies.

  Both JSON files are written with `util::AtomicFile`.
- **Bounded.** Default limits, each adjustable:
  - a 4 GiB library quota;
  - 512 MiB per file and 2 hours per track;
  - 10,000 tracks;
  - a 1 GiB free-space reserve. An import that would cross it is refused before copying starts.
- **Admitted copies.** An import copies the file in, hashing as it copies, then checks the hash
  and size against the source. That keeps playback offline, and unaffected when the original is
  moved or renamed.
- **Stable ids.** `trk:` plus 128 bits of the file's SHA-256 is the same on every device and
  survives renames. Importing the same file twice gives the same track. If two different files
  ever share the short id (full hashes differ), the second gets a `-2` suffix. Playlist ids are
  random once and then fixed. Both fit the event id pattern
  `^[a-z][a-z0-9-]{1,31}:[a-z0-9][a-z0-9-]{1,63}$`.
- **Paths stay on the device.**
  - `library.json` keeps the copy's path inside `music/`, plus the original location for display
    and re-import.
  - Saves and the config file hold only ids.
  - A save's `presentation` block may hold `{"music": {"source": "my", "playlist": "pl:..."}}`.
    It is cosmetic and absent by default, so a default Classic save stays byte-identical
    (contract §6.5).
  - On a device without that playlist, the game uses the player's normal choice and says so.
- **Drive.** The game does not connect to Drive. A connector outside the game lists metadata
  (id, name, MIME type, size, revision, checksum), lets the player pick, and downloads only the
  picked files. It hands each one over as a `Selection` with its `RemoteSource`, and the game
  checks size and checksum on import. Nothing is uploaded, shared or sent.
- **Missing copies.** When a copy is gone (the folder was cleared), the next library scan marks
  the track missing. It is skipped with a notice, and its playlists are left alone.
- **Evidence.** Test reports and published evidence name tracks by id only, never by private
  path or content.

## 7. Decoders: decision

Checked on the official sources on 2026-11-14:

| Candidate | Version (date) | Licence | Source checked |
| --- | --- | --- | --- |
| dr_flac | 0.13.4 (2026-10-31) | public domain (Unlicense) or MIT-0 | `github.com/mackron/dr_libs`, tag `flac-0.13.4`, header |
| dr_mp3 | 0.7.4 (2026-10-31) | public domain (Unlicense) or MIT-0 | same repo, tag `mp3-0.7.4` |
| dr_wav | 0.14.6 (2026-10-31) | public domain (Unlicense) or MIT-0 | same repo, tag `wav-0.14.6` |
| stb_vorbis | 1.24 (2026-11-12) | MIT or public domain (Unlicense) | `github.com/nothings/stb`, `stb_vorbis.c` and README |
| SDL3_mixer | 3.2.4 | zlib | `github.com/libsdl-org/SDL_mixer`, tag `release-3.2.4` |

The dr_libs master branch already says v0.7.5, v0.13.5 and v0.14.7, marked "UNRELEASED" or
"TBD". The README says to take the tags for released versions, so the tags above are the ones
to vendor.

**Decision: vendor dr_flac, dr_mp3, dr_wav and stb_vorbis at the versions above, and convert
with SDL's own `SDL_AudioStream`.**

- **Licences.** Permissive and compatible with GPL-3.0-or-later. Take the public-domain
  alternative of each; keep the notices in `LICENSES/`.
- **SDL-compatible.** They are plain C decoders with no audio output of their own. SDL3_mixer
  3.2.4 uses the same three by default: its build options `SDLMIXER_FLAC_DRFLAC`,
  `SDLMIXER_MP3_DRMP3` and `SDLMIXER_VORBIS_STB` are on, with `decoder_drflac.c`,
  `decoder_drmp3.c` and `decoder_stb_vorbis.c`. So the choice matches SDL's own.
- **No new library build.** Four files, compiled in one translation unit with their
  `*_IMPLEMENTATION` defines. The same code serves Windows (MSVC and MinGW), Linux and later
  Android. Resampling uses SDL, which the game already links.
- **Streaming with bounded memory.**
  - dr_flac, dr_mp3 and dr_wav decode through read/seek callbacks over an `SDL_IOStream`.
  - stb_vorbis decodes from a bounded in-memory copy. Ogg Vorbis files are small; it gets a
    64 MiB limit of its own.

**Alternative kept in reserve: SDL3_mixer 3.2.4's decode-only API.** `MIX_CreateAudioDecoder_IO`
and `MIX_DecodeAudio` decode to a requested format and are documented as safe to call from any
thread. They need `MIX_Init()` and SDL 3.4.0, which the game already has. They are not chosen now
for three reasons:

- they bring a whole second mixer;
- they bring optional backends that would have to be configured away, several of them LGPL
  (mpg123, FluidSynth, libxmp, game-music-emu);
- they add a library to every platform build, for what four headers do.

SDL3_mixer becomes the better choice if Opus (its `SDLMIXER_OPUS` uses libopus) or tracker
formats are wanted.

**Not supported in the first version**, refused at import with "format not supported":

- AAC, M4A, ALAC and WMA: there is no permissive single-file decoder; platform codecs can come
  later.
- Opus: later, through libopus and opusfile, or SDL3_mixer.
- MIDI files: a possible later "My music" path through the existing OPL player.

**Hardening.** The decoders parse files the game did not make, so:

- they run only on the worker;
- header, size and duration limits are checked before a decoder starts (§6);
- a file is refused with more than 8 channels, or a rate outside 8 to 384 kHz;
- tags (ID3v2, Vorbis comments, FLAC metadata) are read by our own bounded reader, not trusted
  from the decoder;
- a fuzz target per decoder adapter runs in the Clang/libFuzzer build;
- the vendored files' SHA-256 values are recorded in `third_party/` beside them.

stb_vorbis 1.23 (2026-11-08) and 1.24 (2026-11-12) are fix releases, which is one more reason to
pin and fuzz.

## 8. Failure handling

| Case | What happens |
| --- | --- |
| Other sample rate or channel count | `SDL_AudioStream` converts; mono goes to both sides, surround is downmixed |
| Very short file (under one block) | Plays normally; a zero-length file is refused at import ("no audio") |
| Broken or hostile tags | Ignored; the title comes from the file name |
| Unicode file names | Opened through SDL's UTF-8 IO; names shown as UTF-8, cut at 200 bytes |
| Corrupt file at import | Refused with the decoder's reason; nothing is added |
| Corrupt data mid-track | The track ends there, a notice names it, playback moves to the next one |
| Copy missing | Marked missing, skipped with a notice, playlist unchanged |
| Every track in the queue fails | After one full pass, playback stops with one notice (no busy loop); Restore original is offered |
| Device lost or changed | `AudioManager::handle_event` reopens the stream; worker and ring are untouched, the song continues |
| Slow disk or stalled worker | Underruns play as silence and are counted; nothing blocks the game |
| Quota or free-space reserve reached | Import refused before copying, with the numbers |

## 9. Settings and menu

- **Config file.**
  - `music_source` (`original`, `my`, `off`) replaces the `music` switch. An old `music = false`
    is read as `off`.
  - `music_playlist` (a playlist id), `music_after_event` (`resume` or `restart`),
    `music_events` (on or off) and `music_loudness` (`off` or `even`).
  - Event rules and playlists live in `playlists.json`, not in the config file.
- **Audio screen.**
  - "Music: Original / My music / Off";
  - "Playlist >", which opens the playlist screen (pick, add files, reorder, repeat, shuffle,
    crossfade);
  - "Restore original";
  - the three volume sliders and Mute in Background as now.
- **Debug console.** `music_status` prints the track id, position, queue and underruns.

## 10. Tests and acceptance

- **Unit tests.**
  - The PCM ring under ThreadSanitizer (producer and consumer at different block sizes, wrap,
    underrun).
  - Source switching in `fill_block` on the dummy audio driver, as `tests/AudioDeviceTests.cpp`
    does.
  - Each decoder adapter on small fixtures made for the tests (generated tones, never third-party
    audio).
  - Truncated, garbage and oversized-header files for every format.
- **Library tests.**
  - Quota and reserve.
  - The same file twice gives the same id.
  - A file renamed between picking and copying.
  - A Unicode name (`Café ♪ 01.flac`).
  - A copy deleted after import.
  - An atomic write interrupted half way.
- **Playlist store.** Round trip, a stale revision refused, unknown ids kept and shown as missing.
- **Event policy.** Pure-function tables: off, no match, one match, two matches, event end with
  `RESUME` and with `RESTART`.
- **Acceptance (R35 priority 1, offline).**
  - Original to My music to Off and back, with the position kept each time.
  - A missing file recovered by a skip.
  - A device unplugged and replugged during My music.

## 11. Order of work

1. PCM ring and the source switch in `fill_block`, fed by a generated tone. This proves the
   real-time path before any decoder exists.
2. The worker, dr_wav and `SDL_AudioStream` conversion.
3. Library import (hash, copy, limits, metadata) and the playlist store on `util::AtomicFile`.
4. The FLAC, MP3 and Vorbis adapters with their fuzz targets.
5. Menu, config keys and the save's `presentation` entry.
6. Shuffle, repeat, crossfade and the event policy.
7. Drive metadata intake, in the connector outside the game.

## 11a. As built, 2026-11-14 (steps 1 to 3, revised)

The owner's R35 update (`NORTH-POLE-MUSIC-LIBRARY.md`) changed the plan before step 3; the code
follows it where it differs from the sections above:

- **One owner, four modes.** `music::MusicPlayback` owns the music: Original, Christmas
  background, Jukebox, Off. The two Christmas modes are one backend. Every change of what plays
  takes a new playback generation; `AudioManager::music_route(source, generation)` switches at
  the next block, cuts the old source (its last sample fades to zero in 64 frames, no crossfade)
  and the new one fades in over the next block. Effects are never touched.
- **Generation-tagged ring.** `music::PcmRing` holds 256-frame blocks tagged with generation,
  track and frame. The callback plays only the generation the route asks for and drops older
  blocks unplayed, so late decoder output never leaks. Pause and stop halt the producer under a
  new generation, so their silence is not counted as underruns.
- **Folders, not copies.** The songs stay where they are and are only read. A root is the game's
  `Xmas` folder or a chosen folder (a chosen one takes precedence; combining is the player's
  choice). Each subfolder is a collection; a lone album folder gets a breadcrumb. Ids are the
  SHA-256 of the bytes (`trk:<32 hex>`), hashed once and cached by size and time in
  `music-library.json`; equal bytes are one track with several sources. Scans are bounded,
  cancellable steps on the music worker and never follow links or junctions.
- **Decoders.** dr_mp3 0.7.4 and dr_wav 0.14.6 behind `music::TrackDecoder`, after our own
  bounded header and tag readers (WAV RIFF/INFO; MP3 ID3v2.2 to 2.4, ID3v1, Xing/Info/VBRI). An
  installed libVLC 3 is a second adapter, loaded at runtime, its audio routed through our ring
  by `libvlc_audio_set_callbacks`; `Backend::AUTO` prefers it when it loads.
- **Settings.** `music-settings.json` (mode, folders, collection names and hiding, playlists with
  revisions, queue place, repeat, shuffle, collapsed, North Pole policy, backend) is a
  `music::RevisionedDocument`: candidate first, read back and checked, then three working copies
  kept, each with the SHA-256 of its data. The music volume stays in the game's config.
- **Restaurants.** `on_restaurant_enter`, `on_restaurant_jukebox`, `on_restaurant_leave` and
  `ambience_suppressed` replace the views' direct FM calls, so a restaurant never stops or
  replaces the Christmas music; the saved `jukebox_music` and its appeal stay as they are.

## 12. Unrelated projects

Nothing here comes from the Barro's Pizza project or the Pizza Connection 3 tools: no code,
schemas, formats or names (owner's decision, 2026-11-14; location contract §17).
