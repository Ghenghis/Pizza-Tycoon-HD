#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks a game recording (docs/studio/notes/recording.md) against its own
recording.json.

  python tools/audio/check_recording.py <recording folder>
  python tools/audio/check_recording.py <folder> --sync-at 2.0   # a flash and a burst at 2.0 s

From the manifest alone:
  * it is a complete pthd-recording, version 1;
  * every segment is there, with its size and SHA-256;
  * the segments follow each other without a gap;
  * the audio is exactly as long as the picture (frames x 44100 / fps).

With ffprobe (and ffmpeg for --sync-at), on the files:
  * each segment has one video and one audio stream, of the manifest's size;
  * the decoded frames add up to the manifest's frame count;
  * the AAC frames hold the manifest's samples, plus the encoder's 1024
    priming samples and at most one frame of padding;
  * the two streams start together, allowing for the AAC priming;
  * with --sync-at: the first bright frame and the first loud audio after
    that time are within 1.5 frames of each other.

Exit status: 0 when everything passes, 1 when a check fails, 2 when the
folder has no usable recording.json. FFmpeg runs without a window. Standard
library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

FORMAT = "pthd-recording"
AUDIO_RATE = 44100
AAC_FRAME = 1024  # samples per AAC frame, and the encoder's priming
START_TOLERANCE_S = 0.002
SEGMENT_TOLERANCE_S = 0.002

# A command line in, its standard output out (and a failure raised).
Runner = Callable[[list[str]], str]


class RecordingError(Exception):
    """The folder has no usable recording.json."""


@dataclass
class Result:
    name: str
    ok: bool
    detail: str


def run_tool(args: list[str]) -> str:
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)  # no console window on Windows
    done = subprocess.run(args, capture_output=True, check=False, creationflags=flags)
    if done.returncode != 0:
        raise RuntimeError(f"{args[0]} failed: " + done.stderr.decode(errors="replace").strip()[:300])
    return done.stdout.decode(errors="replace")


def load_manifest(folder: Path) -> dict[str, Any]:
    try:
        doc = json.loads((folder / "recording.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise RecordingError(f"{folder}: no readable recording.json ({e})") from e
    if not isinstance(doc, dict) or doc.get("format") != FORMAT or doc.get("version") != 1:
        raise RecordingError(f"{folder}: recording.json is not a {FORMAT} version 1")
    return doc


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def check_manifest(folder: Path, m: dict[str, Any]) -> list[Result]:
    out = [
        Result("complete", m.get("complete") is True and m.get("exit_code") == 0, f"stop reason {m.get('stop_reason')}")
    ]
    segments = m.get("segments") or []
    out.append(Result("segments", len(segments) > 0, f"{len(segments)} listed"))
    end = None
    for s in segments:
        path = folder / str(s.get("file", ""))
        if not path.is_file():
            out.append(Result(f"{s.get('file')} present", False, "missing"))
            continue
        size = path.stat().st_size
        out.append(Result(f"{path.name} size", size == s.get("bytes"), f"{size} bytes, manifest {s.get('bytes')}"))
        digest = sha256(path)
        out.append(Result(f"{path.name} sha256", digest == s.get("sha256"), digest[:16] + "..."))
        if end is not None:
            gap = abs(float(s.get("start_s", 0)) - end)
            out.append(
                Result(f"{path.name} follows on", gap <= SEGMENT_TOLERANCE_S, f"{gap * 1000:.1f} ms from the last")
            )
        end = float(s.get("end_s", 0))
    video = m.get("video") or {}
    audio = m.get("audio") or {}
    fps = int(video.get("fps") or 0)
    frames = int(video.get("frames") or 0)
    samples = int(audio.get("samples") or 0)
    expected = (frames * AUDIO_RATE + fps // 2) // fps if fps else -1
    out.append(Result("A/V length", samples == expected, f"{frames} frames, {samples} samples (expected {expected})"))
    if fps:
        duration = frames / fps
        out.append(
            Result(
                "duration",
                abs(duration - float(m.get("duration_s", -1))) < 1e-6,
                f"{duration:.3f} s, manifest {m.get('duration_s')}",
            )
        )
    return out


def probe_json(run: Runner, ffprobe: str, path: Path, *extra: str) -> dict[str, Any]:
    text = run([ffprobe, "-v", "error", *extra, "-of", "json", str(path)])
    result: dict[str, Any] = json.loads(text)
    return result


def check_streams(folder: Path, m: dict[str, Any], run: Runner, ffprobe: str) -> list[Result]:
    out: list[Result] = []
    video = m["video"]
    frames = 0
    aac_frames = 0
    segments = [folder / str(s["file"]) for s in m["segments"]]
    for path in segments:
        info = probe_json(run, ffprobe, path, "-count_frames", "-show_streams")
        streams = info.get("streams", [])
        kinds = sorted(str(s.get("codec_type")) for s in streams)
        out.append(Result(f"{path.name} streams", kinds == ["audio", "video"], str(kinds)))
        for s in streams:
            if s.get("codec_type") == "video":
                frames += int(s.get("nb_read_frames", 0))
                size = (s.get("width"), s.get("height"))
                out.append(
                    Result(
                        f"{path.name} picture",
                        size == (video["width"], video["height"]),
                        f"{size[0]}x{size[1]}",
                    )
                )
            elif s.get("codec_type") == "audio":
                aac_frames += int(s.get("nb_read_frames", 0))
    out.append(Result("frame count", frames == video["frames"], f"{frames} decoded, manifest {video['frames']}"))
    samples = int(m["audio"]["samples"])
    held = aac_frames * AAC_FRAME
    low = samples + AAC_FRAME
    out.append(
        Result(
            "sample count",
            low <= held < low + AAC_FRAME,
            f"{aac_frames} AAC frames hold {held} samples; {samples} recorded + {AAC_FRAME} priming",
        )
    )
    if segments:
        first = probe_json(run, ffprobe, segments[0], "-show_streams")
        starts = {str(s.get("codec_type")): float(s.get("start_time", "nan")) for s in first.get("streams", [])}
        priming = AAC_FRAME / AUDIO_RATE
        if "video" in starts and "audio" in starts:
            off = starts["video"] - (starts["audio"] + priming)
            out.append(
                Result(
                    "A/V start",
                    abs(off) <= START_TOLERANCE_S,
                    f"video {starts['video']:.4f} s, audio {starts['audio']:.4f} s + {priming:.4f} s AAC priming",
                )
            )
    return out


def check_sync(folder: Path, m: dict[str, Any], run: Runner, ffmpeg: str, at_s: float) -> Result:
    """The first bright frame and the first loud audio after at_s, in the
    recording's own timeline (each segment keeps its timestamps)."""
    flash: float | None = None
    burst: float | None = None
    shift = None
    for s in m["segments"]:
        path = str(folder / s["file"])
        base = [ffmpeg, "-v", "error", "-nostdin", "-copyts", "-i", path]
        lum = run(
            base
            + ["-map", "0:v", "-vf", "signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=-", "-f", "null", "-"]
        )
        t = None
        for line in lum.splitlines():
            if "pts_time:" in line:
                t = float(line.split("pts_time:")[1])
                if shift is None:
                    shift = t  # the container's start offset
            elif (
                "YAVG" in line
                and t is not None
                and flash is None
                and float(line.split("=")[1]) > 200
                and shift is not None
                and t - shift >= at_s - 0.5
            ):
                flash = t
        loud = run(
            base
            + [
                "-map",
                "0:a",
                "-af",
                "asetnsamples=n=64,astats=metadata=1:reset=1,ametadata=print:key=lavfi.astats.Overall.Peak_level:file=-",
                "-f",
                "null",
                "-",
            ]
        )
        t = None
        for line in loud.splitlines():
            if "pts_time:" in line:
                t = float(line.split("pts_time:")[1])
            elif (
                "Peak_level" in line
                and t is not None
                and burst is None
                and float(line.split("=")[1]) > -10.0
                and shift is not None
                and t - shift >= at_s - 0.5
            ):
                burst = t
    if flash is None or burst is None:
        return Result("A/V sync", False, f"flash {flash}, burst {burst}")
    fps = m["video"]["fps"]
    return Result(
        "A/V sync",
        abs(flash - burst) <= 1.5 / fps,
        f"flash at {flash:.3f} s, burst at {burst:.3f} s, {1000 * (burst - flash):+.1f} ms",
    )


def check(folder: Path, run: Runner | None = None, sync_at: float | None = None) -> list[Result]:
    m = load_manifest(folder)
    results = check_manifest(folder, m)
    ffprobe = shutil.which("ffprobe")
    ffmpeg = shutil.which("ffmpeg")
    if run is None:
        if not ffprobe:
            return results + [Result("ffprobe", False, "ffprobe is not on the PATH")]
        run = run_tool
    try:
        results += check_streams(folder, m, run, ffprobe or "ffprobe")
        if sync_at is not None:
            results.append(check_sync(folder, m, run, ffmpeg or "ffmpeg", sync_at))
    except (RuntimeError, ValueError, KeyError) as e:
        results.append(Result("decode", False, str(e)))
    return results


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("folder", type=Path, help="a recording's folder, with recording.json")
    ap.add_argument("--sync-at", type=float, help="seconds of a bright frame and a loud burst to align")
    args = ap.parse_args(argv)
    try:
        results = check(args.folder, sync_at=args.sync_at)
    except RecordingError as e:
        print(e, file=sys.stderr)
        return 2
    for r in results:
        print(("PASS " if r.ok else "FAIL ") + f"{r.name}: {r.detail}")
    failed = [r for r in results if not r.ok]
    print(f"{len(results)} checks, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
