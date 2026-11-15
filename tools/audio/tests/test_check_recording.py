# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""check_recording.py on synthetic recordings: the manifest checks with no
FFmpeg at all, the stream checks with canned ffprobe answers, and one real
round trip when FFmpeg is installed."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_recording as cr  # noqa: E402


def make(folder: Path, frames: int = 60, fps: int = 30, segments: int = 2, **override: Any) -> dict[str, Any]:
    """A recording.json and segment files of made-up bytes."""
    folder.mkdir(parents=True, exist_ok=True)
    segs = []
    step = frames / fps / segments
    for i in range(segments):
        f = folder / f"rec-{i:03d}.mkv"
        f.write_bytes(bytes([i]) * (1000 + i))
        segs.append(
            {
                "file": f.name,
                "start_s": i * step,
                "end_s": (i + 1) * step,
                "bytes": f.stat().st_size,
                "sha256": hashlib.sha256(f.read_bytes()).hexdigest(),
            }
        )
    m: dict[str, Any] = {
        "format": "pthd-recording",
        "version": 1,
        "complete": True,
        "exit_code": 0,
        "stop_reason": "user",
        "duration_s": frames / fps,
        "video": {"width": 640, "height": 400, "fps": fps, "frames": frames},
        "audio": {"samples": frames * 44100 // fps},
        "segments": segs,
    }
    m.update(override)
    (folder / "recording.json").write_text(json.dumps(m), encoding="utf-8")
    return m


def failed(results: list[cr.Result]) -> list[str]:
    return [r.name for r in results if not r.ok]


def test_a_sound_manifest_passes(tmp_path: Path) -> None:
    m = make(tmp_path)
    assert failed(cr.check_manifest(tmp_path, m)) == []


def test_manifest_problems_are_found(tmp_path: Path) -> None:
    m = make(tmp_path)
    (tmp_path / "rec-001.mkv").write_bytes(b"changed")
    m["audio"]["samples"] += 1
    m["segments"][1]["start_s"] += 0.5
    names = failed(cr.check_manifest(tmp_path, m))
    assert "rec-001.mkv size" in names
    assert "rec-001.mkv sha256" in names
    assert "rec-001.mkv follows on" in names
    assert "A/V length" in names
    (tmp_path / "rec-000.mkv").unlink()
    assert "rec-000.mkv present" in failed(cr.check_manifest(tmp_path, m))


def test_an_incomplete_recording_fails(tmp_path: Path) -> None:
    m = make(tmp_path, complete=False, exit_code=1)
    assert "complete" in failed(cr.check_manifest(tmp_path, m))


def test_no_recording_is_exit_2(tmp_path: Path) -> None:
    assert cr.main([str(tmp_path)]) == 2
    (tmp_path / "recording.json").write_text('{"format": "other"}', encoding="utf-8")
    assert cr.main([str(tmp_path)]) == 2


def canned(
    m: dict[str, Any], video_frames: list[int], aac_frames: list[int], v_start: float, a_start: float
) -> cr.Runner:
    """An ffprobe that answers from these numbers."""

    def run(args: list[str]) -> str:
        name = Path(args[-1]).name
        i = [s["file"] for s in m["segments"]].index(name)
        return json.dumps(
            {
                "streams": [
                    {
                        "codec_type": "video",
                        "width": 640,
                        "height": 400,
                        "nb_read_frames": str(video_frames[i]),
                        "start_time": str(v_start),
                    },
                    {"codec_type": "audio", "nb_read_frames": str(aac_frames[i]), "start_time": str(a_start)},
                ]
            }
        )

    return run


def test_stream_checks_with_canned_ffprobe(tmp_path: Path) -> None:
    m = make(tmp_path, frames=60)  # 88200 samples: 88200 + 1024 need 88 AAC frames
    good = canned(m, [30, 30], [44, 44], 0.067, 0.067 - 1024 / 44100)
    assert failed(cr.check_streams(tmp_path, m, good, "ffprobe")) == []

    short = canned(m, [30, 29], [44, 44], 0.067, 0.067 - 1024 / 44100)
    assert "frame count" in failed(cr.check_streams(tmp_path, m, short, "ffprobe"))
    no_priming = canned(m, [30, 30], [43, 43], 0.067, 0.067 - 1024 / 44100)
    assert "sample count" in failed(cr.check_streams(tmp_path, m, no_priming, "ffprobe"))
    late = canned(m, [30, 30], [44, 44], 0.067, 0.067)
    assert "A/V start" in failed(cr.check_streams(tmp_path, m, late, "ffprobe"))


@pytest.mark.skipif(not (shutil.which("ffmpeg") and shutil.which("ffprobe")), reason="needs ffmpeg and ffprobe")
def test_a_real_recording_round_trip(tmp_path: Path) -> None:
    # As the recorder makes them: raw picture and sound to AAC/H.264 (or
    # MPEG-4) Matroska segments, timestamps kept.
    encoders = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"], capture_output=True, text=True).stdout
    video = ["-c:v", "libx264", "-preset", "veryfast"] if " libx264 " in encoders else ["-c:v", "mpeg4"]
    out = tmp_path / "rec-%03d.mkv"
    subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-nostdin",
            "-f",
            "lavfi",
            "-i",
            "color=c=gray:s=640x400:r=30:d=2",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=220:sample_rate=44100:duration=2",
            "-map",
            "0:v",
            "-map",
            "1:a",
            *video,
            "-pix_fmt",
            "yuv420p",
            "-force_key_frames",
            "expr:gte(t,n_forced*1)",
            "-c:a",
            "aac",
            "-ac",
            "2",
            "-f",
            "segment",
            "-segment_time",
            "1",
            "-segment_format",
            "matroska",
            "-reset_timestamps",
            "0",
            "-segment_list",
            str(tmp_path / "list.csv"),
            "-segment_list_type",
            "csv",
            str(out),
        ],
        check=True,
    )
    segs = []
    for line in (tmp_path / "list.csv").read_text().splitlines():
        name, start, end = line.split(",")
        f = tmp_path / name
        segs.append(
            {
                "file": name,
                "start_s": float(start),
                "end_s": float(end),
                "bytes": f.stat().st_size,
                "sha256": hashlib.sha256(f.read_bytes()).hexdigest(),
            }
        )
    m = {
        "format": "pthd-recording",
        "version": 1,
        "complete": True,
        "exit_code": 0,
        "stop_reason": "user",
        "duration_s": 2.0,
        "video": {"width": 640, "height": 400, "fps": 30, "frames": 60},
        "audio": {"samples": 88200},
        "segments": segs,
    }
    (tmp_path / "recording.json").write_text(json.dumps(m), encoding="utf-8")
    results = cr.check(tmp_path)
    by = {r.name: r for r in results}
    assert by["frame count"].ok, by["frame count"].detail
    assert by["sample count"].ok, by["sample count"].detail
    assert cr.main([str(tmp_path)]) in (0, 1)  # runs end to end
