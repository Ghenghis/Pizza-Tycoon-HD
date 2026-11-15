# Game recording (R34): session API, video readback, encoder process

2026-11-14. This is the design for recording the game's own picture and sound, after
`RECORDING-AND-MUSIC.md` (the owner's proposal).

**Built (the first slice):**
- the audio tap;
- the session, with the Classic picture path (`src/engine/record/`);
- the owned FFmpeg process;
- the `record` console command.

**Planned:** the HD readback, the hardware encoders, the UI and the long acceptance runs.

## The first slice, as built

- **Commands.**
  - `record start [seconds] [folder]`, `record stop` and `record status` work in the game.
  - `record clip <seconds> [folder] [music]` works in batch mode (`--exec`). It records the
    current screen in real time, with the simulation and the sound running.
  - Output goes in a new `rec-<date>-<time>` folder, by default under `recordings/` beside the
    saves.
- **Picture.** Classic (HD off) only, for now; `record start` refuses the HD path.
  - The software path's composed frame is already in memory as 640x400 RGBA. It is several
    indexed layers, each with its own palette, put together, so no single indexed frame exists.
  - When a frame is due, the game thread copies it into one of four slots allocated at the
    start. No GPU readback and no waiting.
  - A worker converts the frame to rgb24 and writes it to FFmpeg's stdin.
  - Output frame k shows the game frame taken nearest to k/fps, within half a period. When the
    game shows nothing new, the last frame is held.
- **Sound.** The recorder's audio thread drains `MixTap`.
  - Each frame is placed by its sample position; a gap is written as silence in its place.
  - Audio from before the start is not used.
- **Time 0.** Time 0 is when FFmpeg has taken the first frame, for both streams, so FFmpeg's
  start-up never costs frames.
  - Raw inputs run with `-probesize 32 -analyzeduration 0`. With probing on, FFmpeg waits for
    about a second of audio before it reads more picture, and the start stalls.
- **The end.**
  - The stop point is at least what either stream has begun, so both streams end at exactly the
    same length: N frames and N x 44100 / fps samples.
  - Neither stream is ever written past the duration limit.
- **Output.**
  - `-f segment -segment_format matroska -reset_timestamps 0`, libx264 (veryfast, CRF 18,
    yuv420p) with a keyframe forced at each segment start, plus AAC at 192 kb/s.
  - `recording.json` holds the configuration, the FFmpeg path and version, counts (frames,
    held, dropped, samples, gap samples), the stop reason, the exit code, and each segment's
    times, size and SHA-256.
- **Encoders.**
  - `Capabilities::probe()` lists the encoders FFmpeg has, NVENC, AMF and QSV included.
  - Only software encoders that open a trial encode here are offered: libx264, libopenh264 and
    mpeg4. The hardware encoders are listed, never offered, until a later round initialises
    them.
- **Process.** `record::Subprocess` starts FFmpeg with arguments as a list, never through a shell.
  - Windows: `CREATE_NO_WINDOW`, a Job object with kill-on-close, an explicit handle list, the
    audio on a named pipe, and quoting by the `CommandLineToArgvW` rules (tested).
  - Linux: `fork` + `execv`, not `posix_spawn`, because `PR_SET_PDEATHSIG` must be set in the
    child. The child has its own process group, which a kill takes whole. It ends with the
    thread that started it: the game thread, which calls `RecordingSession::start()`.
  - Other POSIX: `posix_spawn` with its own process group.
  - stderr is kept in a 64 KiB tail. A child that ends early, or stops reading, stops the
    session with `encoder-error`, and the game goes on.

**Measured in the cloud container** (Linux, FFmpeg 6.1.1, libx264). 5 s of a scripted
synthetic game, through the real session, `MixTap` and FFmpeg, then decoded with
ffprobe/ffmpeg:
- 150 frames;
- video 5.000 s, audio 5.039 s (AAC padding);
- start times equal once the 1024-sample AAC priming is allowed for;
- audio present (RMS 1393 for a 1414 tone);
- no audio from before the start;
- a white flash and a 1 kHz burst, both at 2.0 s, within 12–24 ms of each other.

The same checks are in `tests/RecordingTests.cpp` (`[record][ffmpeg]`, skipped without FFmpeg).
A game run (`record clip` in `--exec`) has not been run here.

## What is recorded

- The game's composed image and its own final mix: nothing from the desktop, other windows, the
  system loopback or a microphone.
- Recording is off by default. It is started and stopped explicitly, with a visible indicator,
  the elapsed time and an estimate of the time and storage left.
- Default limits: 60 minutes, a byte quota and a free-space reserve. The session stops at the
  first of these it reaches, or on an encoder error, keeps what it can and says why.

## The audio tap (built)

`AudioManager::fill_block()` writes every block it hands the device into `AudioManager::mix_tap()`:

- The ring holds the **final mix**, after the music and effect volumes, the master volume, the
  mute and the ducking. A recording therefore sounds as the player hears the game. It is silent
  when the game is muted, for example in the background with "Mute in Background" on.
- **Positions by sample.** Every frame the mixer makes has a number, counted from 0 since the
  `AudioManager` was made, whether or not the tap is on (`MixTap::position()`). Each block in the
  ring is stamped with the number of its first frame. Audio time is `position / 44100`, never the
  wall clock or the frame rate.
- **Bounded and allocation-free.** The ring is single-producer, single-consumer: 512 blocks of
  256 frames (2.97 s, 512 KiB), allocated when the `AudioManager` is made. The callback only
  copies and publishes, with no lock, wait or allocation. Off (the default), it only counts frames.
- **Never out of place.** When the ring is full, the newest audio is dropped and counted
  (`dropped_frames()`). `read()` returns runs of consecutive positions and ends a run at a gap.
  The reader therefore knows exactly which frames were lost, fills them with silence and keeps
  sync.
- One reader: the recorder's audio thread, which calls `read()` and passes each run to
  `submit_audio()` below. Nothing reads the tap yet.

Tests: `tests/MixTapTests.cpp` covers positions while off and on, partial reads, a full ring and
its gap, a writer and reader running at once, and the bytes `fill_block()` gave the device.

## Session API (as built; the frame type is 32-bit RGBA for now)

```cpp
namespace recording {

struct Config {
  std::filesystem::path folder;          // output; checked writable before start
  std::chrono::seconds max_duration{3600};
  std::uint64_t quota_bytes;             // hard cap on bytes written
  std::uint64_t reserve_bytes;           // free space always left on the drive
  Size size;                             // output; a supported one only
  int fps = 60;                          // constant output rate
  Codec codec;                           // one the capabilities list
  int video_kbps; int audio_kbps = 192;
  bool include_cursor = true;            // the indicator is never hidden from the player
  MinimizePolicy minimized = MinimizePolicy::HOLD_LAST_FRAME; // or PAUSE
};

struct Capabilities {                    // probed before a session can start
  std::filesystem::path ffmpeg;          // the official build found, and its version
  std::vector<Codec> codecs;             // e.g. h264_nvenc, hevc_nvenc, libx264 (never AV1 on Ampere)
  ReadbackPath readback;                 // see "Video readback"
  std::vector<Size> sizes; std::vector<int> fps;
};

enum class StopReason { USER, DURATION, QUOTA, DISK_SPACE, ENCODER_ERROR, GAME_EXIT };

class RecordingSession {
public:
  // Checks the folder, free space, quota and profile, starts the encoder and
  // its threads, and returns null with the reason when any check fails, before
  // anything is recorded.
  static std::unique_ptr<RecordingSession> start(const Config&, const Capabilities&,
                                                 std::string* why);
  // Render thread, once a presented frame: hands over a frame for timestamp t
  // (the recording clock). Never blocks. False when the queue is full: the
  // frame is dropped and counted, and the writer holds the last one.
  bool submit_video(VideoFrame frame, Timestamp t);
  // The recorder's audio thread (from MixTap::read()): a run of 44100 Hz
  // stereo s16, starting at sample position first_sample. A position gap is
  // written as silence. Never blocks the mix; the callback never calls this.
  void submit_audio(std::span<const std::int16_t> pcm, std::uint64_t first_sample);
  // Any thread: finishes the current segment and the manifest. Idempotent.
  void stop(StopReason reason);
  [[nodiscard]] Status status() const;
};

struct Status {
  State state;               // STARTING, RECORDING, STOPPING, STOPPED, FAILED
  StopReason stop_reason;
  std::chrono::milliseconds elapsed;
  std::uint64_t bytes_written; std::uint64_t free_bytes;
  std::size_t segments;      // finished, each with its hash, duration and counts
  std::uint64_t frames_in, frames_dropped, frames_held;  // held: duplicated to keep the rate
  std::uint64_t audio_frames, audio_gap_frames;           // gap: tap drops, written as silence
  std::size_t video_queue, audio_queue;                   // occupancy, for the indicator
  double av_drift_ms;        // video clock against audio sample position
  std::string error;         // the encoder's last message, when it failed
};

} // namespace recording
```

**Clocks.**
- One monotonic recording clock starts at `start()`.
- Video frames are stamped on that clock. Audio is placed by sample position, with the first tap
  position taken at `start()` as zero.
- The writer emits video at a constant rate. It holds (duplicates) the last frame when the game
  shows nothing new, and never advances the simulation to make frames.
- Drift is the difference between the frames' clock and the audio position. Above a set bound the
  writer drops or holds frames, never audio.

**Threads.**
- The render thread copies a frame into a bounded pool (3 to 4 frames, allocated at start).
- The audio thread drains the tap.
- One writer thread feeds the encoder.
- Every queue is bounded in bytes and in time. Under sustained pressure the policy drops or holds
  video with its timestamps, or stops cleanly. It never blocks the game and never drops audio
  silently.

## Video readback (planned)

No per-frame synchronous screenshot: `SDL_RenderReadPixels` (as `Renderer::with_current_frame()`
uses for Ctrl+S) waits for the GPU and stalls the frame. The plan by render path:

1. **Classic / software path.** The composed frame is already in memory: a 640x400 8-bit
   backbuffer plus its palette.
   - The render thread copies 256 KiB plus 768 bytes into the frame pool.
   - The writer expands to BGRA and scales to the output size.
   - No GPU readback at all.
2. **HD on SDL's `gpu` render driver** (`PostFx::is_gpu_driver()`).
   - The composed `back_tex_` is an `SDL_GPUTexture` (`SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER`) on
     the renderer's device (`SDL_PROP_RENDERER_GPU_DEVICE_POINTER`).
   - After `SDL_FlushRenderer`, a copy pass downloads it with `SDL_DownloadFromGPUTexture` into
     one of 3 `SDL_GPUTransferBuffer`s. That buffer is submitted with
     `SDL_SubmitGPUCommandBufferAndAcquireFence`.
   - A buffer is mapped only once `SDL_QueryGPUFence` says it is done, 1 or 2 frames later,
     carrying its timestamp from when it was taken. The game never waits.
   - If no buffer is free, the frame is dropped and counted.
   - Scaling to the output size happens on the GPU, before the download (a blit into a capture
     texture), so only output-size bytes cross the bus.
3. **HD on another driver** (Direct3D 11/12, OpenGL, Vulkan through the SDL_Renderer API).
   - SDL_Renderer offers no asynchronous readback.
   - The only option is `SDL_RenderReadPixels` on a small capture target at the recording rate.
     It is a qualified **fallback profile** that must be measured (frame tails, input latency)
     before it is offered.
   - Otherwise recording asks for the `gpu` driver. It never starts silently in a mode that would
     stall the game.
4. **Minimised or occluded.** The game's own frame is used, never the desktop. By policy the
   session holds the last frame or pauses, with timestamps kept consistent either way.

## Encoder process (planned)

- **Which FFmpeg.**
  - The official FFmpeg build at a configured path, or on the PATH.
  - Before it is offered, it is probed with `-hide_banner -version` and `-hide_banner -encoders`,
    which give the version and the encoders it really has. Probe results are cached with the
    executable's size, modification time and hash.
  - No encoder is assumed: NVENC H.264/HEVC are listed on the RTX 3090 only when the probe shows
    them, and AV1 is never listed on Ampere.
  - The game does not ship FFmpeg; which build and licence to use is the owner's decision.
- **Hidden.** No console window ever appears.
  - Windows: `CreateProcessW` with `CREATE_NO_WINDOW` and `STARTF_USESTDHANDLES`.
  - POSIX: `posix_spawn`.
  - Never through a shell.
- **Structured arguments.** `std::vector<std::string>` throughout. On Windows they are quoted by
  the `CommandLineToArgvW` rules in one function, and that function is tested against tricky
  paths (spaces, quotes, trailing backslashes, Unicode). Paths are passed as arguments, never
  spliced into a string.
- **Owned lifetime.**
  - The process belongs to the session object.
  - Windows: it runs in a Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so it cannot
    outlive the game, even after a crash.
  - Linux: `PR_SET_PDEATHSIG`; macOS: its own process group, signalled on exit.
- **Inputs.** Raw video on stdin: `-f rawvideo -pix_fmt bgra -s WxH -r FPS -i pipe:0`. Raw audio
  on a second pipe: `-f s16le -ar 44100 -ac 2 -i <pipe>`.
  - Windows: a named pipe `\\.\pipe\pthd-rec-<random>` created by the game.
  - POSIX: an inherited descriptor, `pipe:3`.
  - Writes to the pipes happen only on the writer thread, with a bounded buffer.
- **Output.** Short, independently playable Matroska segments:
  - `-f segment -segment_format matroska -segment_time 60 -reset_timestamps 0 -segment_list <csv>`,
    with keyframes forced at segment boundaries.
  - The game writes its own recording manifest: segments, hashes, durations, frame and audio
    counts, stop reason and configuration.
  - Remux to one MP4 or MKV is optional and happens only after a clean stop.
- **Stop and errors.**
  - `stop()` closes both pipes. FFmpeg finalises the open segment, and the session waits for it
    with a timeout. On timeout the job is terminated and the last segment is marked partial.
  - The exit status is checked.
  - stderr is read on its own thread into a bounded tail (64 KiB) for the status and the log.
  - A broken pipe, a non-zero exit, a full disk or a lost destination stops the session with
    `ENCODER_ERROR` or `DISK_SPACE`. The game stays responsive and the finished segments stay
    discoverable.

## Acceptance (from the proposal, still open)

- **Short slice.** One qualified profile; decode the output and check the images, the audio
  content, monotonic timestamps and a clean stop.
- **Interruptions.** Kill tests, a full disk and a lost destination. Decode every retained
  segment.
- **Hour run.** 60 minutes with scene changes, music and effects. Measure A/V drift, frame tails,
  audio underruns and memory. Keep the receipts.
- **Platforms.** AMD and Android are qualified separately.
