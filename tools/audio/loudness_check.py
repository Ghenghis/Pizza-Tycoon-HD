#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks an effects pack's loudness against its targets.

  python tools/audio/loudness_check.py <pack folder>
  python tools/audio/loudness_check.py <pack folder> --json report.json
  python tools/audio/loudness_check.py <pack folder> --bed-target -23 --tolerance 2 --true-peak -1

Reads <pack>/manifest.json (pthd-sfx-overrides, version 1 or 2; see
docs/studio/notes/sfx-overrides.md) and measures every sound it names as the
game plays it: after its trims and gain_db, mono played on both channels.

  * integrated loudness, ITU-R BS.1770-4: K-weighting, 400 ms blocks every
    100 ms, the -70 LUFS absolute and -10 LU relative gates;
  * true peak, 4x oversampled (a 48-tap windowed-sinc interpolator);
  * sample peak.

Targets: an ambience bed (an event of ambience priority, the loop the game
plays under a scene) must be at the bed target (-23 LUFS integrated) within
the tolerance (2 LU); every sound must stay at or under the true-peak limit
(-1 dBTP), at the top of its gain_jitter_db. One-shots and ambience layers report their loudness without a
target. A sound shorter than one 400 ms block has no integrated loudness.

Exit status: 0 when every sound passes, 1 when one fails, 2 when the pack
cannot be read. WAV is read here; any other format is decoded by ffmpeg when
it is on the PATH (run without a window). The standard library is enough;
numpy, when installed, makes the true peak faster. Nothing is played or
written but the report.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import shutil
import struct
import subprocess
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path
from types import ModuleType
from typing import Any

np: ModuleType | None
try:
    import numpy as np
except ImportError:  # the pure-Python path is used instead
    np = None

FORMAT = "pthd-sfx-overrides"
VERSIONS = (1, 2)
# As audio::default_priority() (src/engine/audio/EffectPack.cpp).
AMBIENCE_EVENTS = {
    "restaurant-noise-1",
    "restaurant-noise-2",
    "restaurant-noise-3",
    "traffic-1",
    "traffic-2",
    "kitchen-busy",
    "kitchen-idle",
}
OVERSAMPLE = 4
TAPS_PER_PHASE = 12


class PackError(Exception):
    """The manifest cannot be used at all."""


# ---------------------------------------------------------------- decoding


def read_wav(data: bytes) -> tuple[int, list[list[float]]]:
    """(rate, channels as -1..1 floats) of a PCM or float WAV."""
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV file")
    fmt = None
    body = None
    at = 12
    while at + 8 <= len(data):
        cid = data[at : at + 4]
        size = struct.unpack_from("<I", data, at + 4)[0]
        chunk = data[at + 8 : at + 8 + size]
        if cid == b"fmt ":
            fmt = chunk
        elif cid == b"data":
            body = chunk
        at += 8 + size + (size & 1)
    if fmt is None or body is None or len(fmt) < 16:
        raise ValueError("a WAV file without fmt or data")
    tag, channels, rate = struct.unpack_from("<HHI", fmt, 0)
    bits = struct.unpack_from("<H", fmt, 14)[0]
    if tag == 0xFFFE and len(fmt) >= 26:
        tag = struct.unpack_from("<H", fmt, 24)[0]
    if channels < 1 or rate < 1000:
        raise ValueError("a WAV file with no channels or rate")
    width = bits // 8
    frames = len(body) // (width * channels)
    values: list[float]
    if tag == 3 and bits in (32, 64):
        values = list(struct.unpack_from(f"<{frames * channels}{'f' if bits == 32 else 'd'}", body))
    elif tag == 1 and bits == 8:
        values = [(b - 128) / 128.0 for b in body[: frames * channels]]
    elif tag == 1 and bits == 16:
        values = [v / 32768.0 for v in struct.unpack_from(f"<{frames * channels}h", body)]
    elif tag == 1 and bits == 32:
        values = [v / 2147483648.0 for v in struct.unpack_from(f"<{frames * channels}i", body)]
    elif tag == 1 and bits == 24:
        values = []
        for i in range(frames * channels):
            b = body[i * 3 : i * 3 + 3]
            v = b[0] | (b[1] << 8) | (b[2] << 16)
            values.append((v - (1 << 24) if v & 0x800000 else v) / 8388608.0)
    else:
        raise ValueError(f"a WAV format this check does not read (tag {tag}, {bits} bits)")
    return rate, [values[c::channels] for c in range(channels)]


def decode_ffmpeg(path: Path) -> tuple[int, list[list[float]]]:
    """Any other file, through ffmpeg, as 44100 Hz stereo float."""
    exe = shutil.which("ffmpeg")
    if not exe:
        raise ValueError("not a WAV file, and ffmpeg is not on the PATH to decode it")
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)  # no console window on Windows
    run = subprocess.run(
        [exe, "-v", "error", "-nostdin", "-i", str(path), "-f", "f32le", "-ac", "2", "-ar", "44100", "-"],
        capture_output=True,
        check=False,
        creationflags=flags,
    )
    if run.returncode != 0:
        raise ValueError("ffmpeg cannot decode it: " + run.stderr.decode(errors="replace").strip()[:200])
    raw = run.stdout[: len(run.stdout) // 8 * 8]
    values = struct.unpack(f"<{len(raw) // 4}f", raw)
    return 44100, [list(values[0::2]), list(values[1::2])]


def as_played(rate: int, channels: list[list[float]], spec: dict[str, Any], entry: dict[str, Any]) -> list[list[float]]:
    """The sound as the game plays it: stereo, trimmed, with its gain."""

    def number(key: str, fallback: float = 0.0) -> float:
        for source in (spec, entry):
            v = source.get(key)
            if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v):
                return float(v)
        return fallback

    if len(channels) == 1:
        channels = [channels[0], channels[0]]
    channels = channels[:2]
    frames = len(channels[0])
    head = int(max(number("trim_start_ms"), 0.0) * rate / 1000.0)
    tail = int(max(number("trim_end_ms"), 0.0) * rate / 1000.0)
    if head + tail >= frames:
        raise ValueError("its trims leave no sound")
    gain = 10.0 ** (min(max(number("gain_db"), -60.0), 12.0) / 20.0)
    return [[min(max(v * gain, -1.0), 1.0) for v in c[head : frames - tail]] for c in channels]


# ---------------------------------------------------------------- measuring


def k_weighting(rate: int) -> list[tuple[list[float], list[float]]]:
    """The two BS.1770 biquads (b, a) at any rate, from the analog design."""
    f0, gain_db, q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    k = math.tan(math.pi * f0 / rate)
    vh = 10.0 ** (gain_db / 20.0)
    vb = vh**0.4996667741545416
    a0 = 1.0 + k / q + k * k
    shelf = (
        [(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0],
        [1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0],
    )
    f0, q = 38.13547087602444, 0.5003270373238773
    k = math.tan(math.pi * f0 / rate)
    a0 = 1.0 + k / q + k * k
    high = ([1.0, -2.0, 1.0], [1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0])
    return [shelf, high]


def biquad(x: list[float], b: list[float], a: list[float]) -> list[float]:
    b0, b1, b2 = b
    _, a1, a2 = a
    y = [0.0] * len(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        out = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, out
        y[i] = out
    return y


def integrated_loudness(rate: int, channels: list[list[float]]) -> float | None:
    """LUFS, gated; None for a sound shorter than one block or all silence."""
    block = int(round(0.4 * rate))
    hop = int(round(0.1 * rate))
    frames = len(channels[0])
    if frames < block:
        return None
    # Running sums of the K-weighted squares, per channel.
    sums = []
    for c in channels:
        y = c
        for b, a in k_weighting(rate):
            y = biquad(y, b, a)
        acc = [0.0]
        total = 0.0
        for v in y:
            total += v * v
            acc.append(total)
        sums.append(acc)
    powers = []
    for start in range(0, frames - block + 1, hop):
        powers.append(sum((s[start + block] - s[start]) / block for s in sums))

    def lufs(p: float) -> float:
        return -0.691 + 10.0 * math.log10(p) if p > 0.0 else -math.inf

    gated = [p for p in powers if lufs(p) > -70.0]
    if not gated:
        return None
    relative = lufs(sum(gated) / len(gated)) - 10.0
    gated = [p for p in gated if lufs(p) > relative]
    return lufs(sum(gated) / len(gated)) if gated else None


def interpolator() -> list[float]:
    """48 taps: a Hann-windowed sinc low-pass at the original Nyquist."""
    n = OVERSAMPLE * TAPS_PER_PHASE
    centre = (n - 1) / 2.0
    taps = []
    for i in range(n):
        t = (i - centre) / OVERSAMPLE
        sinc = 1.0 if t == 0 else math.sin(math.pi * t) / (math.pi * t)
        window = 0.5 - 0.5 * math.cos(2.0 * math.pi * (i + 0.5) / n)
        taps.append(sinc * window)
    # Each phase passes a constant at unity.
    for p in range(OVERSAMPLE):
        phase = taps[p::OVERSAMPLE]
        s = sum(phase)
        for k in range(len(phase)):
            taps[p + k * OVERSAMPLE] = phase[k] / s
    return taps


def true_peak(channels: list[list[float]]) -> float:
    """The highest absolute value, 4x oversampled; at least the sample peak."""
    taps = interpolator()
    peak = max((abs(v) for c in channels for v in c), default=0.0)
    for c in channels:
        if np is not None:
            up = np.zeros(len(c) * OVERSAMPLE)
            up[::OVERSAMPLE] = c
            peak = max(peak, float(np.max(np.abs(np.convolve(up, np.array(taps))), initial=0.0)))
            continue
        phases = [taps[p::OVERSAMPLE] for p in range(OVERSAMPLE)]
        n = len(c)
        for i in range(n):
            for ph in phases:
                v = 0.0
                for k, h in enumerate(ph):
                    j = i - k
                    if j < 0:
                        break
                    v += h * c[j]
                if v > peak or -v > peak:
                    peak = abs(v)
    return peak


def db(v: float) -> float:
    return 20.0 * math.log10(v) if v > 0.0 else -math.inf


# ---------------------------------------------------------------- the pack


@dataclass
class Row:
    kind: str  # bed, one-shot, layer
    event: str
    layer: str
    variant: int  # from 1
    file: str
    lufs: float | None = None
    true_peak_dbtp: float | None = None
    # At the top of the sound's gain_jitter_db: what the limit is checked on.
    true_peak_max_dbtp: float | None = None
    sample_peak_dbfs: float | None = None
    problems: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.problems


def sounds_of(entry: dict[str, Any], v2: bool) -> list[dict[str, Any]]:
    variants = entry.get("variants") if v2 else None
    if isinstance(variants, list):
        return [v for v in variants if isinstance(v, dict)]
    return [entry]


def measure(
    pack: Path, spec: dict[str, Any], entry: dict[str, Any], row: Row, target: float, tolerance: float, tp_limit: float
) -> None:
    name = spec.get("file")
    if not isinstance(name, str) or not name or Path(name).is_absolute() or ".." in Path(name).parts:
        row.problems.append("its file must be a path inside the pack")
        return
    path = pack / name
    try:
        data = path.read_bytes()
    except OSError:
        row.problems.append("its file is missing")
        return
    if hashlib.sha256(data).hexdigest() != spec.get("sha256"):
        row.problems.append("its file does not match its sha256 (the game refuses it)")
    try:
        rate, channels = read_wav(data) if data[:4] == b"RIFF" else decode_ffmpeg(path)
        played = as_played(rate, channels, spec, entry)
    except ValueError as e:
        row.problems.append(str(e))
        return
    row.lufs = integrated_loudness(rate, played)
    row.true_peak_dbtp = db(true_peak(played))
    jitter = 0.0
    for source in (spec, entry):
        v = source.get("gain_jitter_db")
        if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v):
            jitter = min(max(float(v), 0.0), 6.0)
            break
    row.true_peak_max_dbtp = row.true_peak_dbtp + jitter
    row.sample_peak_dbfs = db(max((abs(v) for c in played for v in c), default=0.0))
    if row.true_peak_max_dbtp > tp_limit + 1e-9:
        at_top = " at the top of its gain_jitter_db" if jitter else ""
        row.problems.append(f"true peak {row.true_peak_max_dbtp:.1f} dBTP{at_top} is over {tp_limit:.1f}")
    if row.kind == "bed":
        if row.lufs is None:
            row.problems.append("too short or too quiet to measure as a bed")
        elif abs(row.lufs - target) > tolerance:
            row.problems.append(f"{row.lufs:.1f} LUFS is not {target:.0f} +/- {tolerance:g} LU")


def check_pack(pack: Path, target: float = -23.0, tolerance: float = 2.0, tp_limit: float = -1.0) -> list[Row]:
    try:
        doc = json.loads((pack / "manifest.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise PackError(f"its manifest.json cannot be read ({e})") from e
    if not isinstance(doc, dict) or doc.get("format") != FORMAT or doc.get("version") not in VERSIONS:
        raise PackError("its manifest.json is not an effects pack of a version this check reads")
    v2 = doc["version"] >= 2
    rows: list[Row] = []
    for entry in doc.get("effects") or []:
        if not isinstance(entry, dict):
            continue
        event = str(entry.get("event", ""))
        priority = entry.get("priority") if v2 else None
        bed = priority == "ambience" if isinstance(priority, str) else event in AMBIENCE_EVENTS
        for i, spec in enumerate(sounds_of(entry, v2), start=1):
            row = Row("bed" if bed else "one-shot", event, "", i, str(spec.get("file", "")))
            measure(pack, spec, entry, row, target, tolerance, tp_limit)
            rows.append(row)
    for scene in (doc.get("ambience") or []) if v2 else []:
        if not isinstance(scene, dict):
            continue
        for layer in scene.get("layers") or []:
            if not isinstance(layer, dict):
                continue
            for i, spec in enumerate(sounds_of(layer, True), start=1):
                row = Row("layer", str(scene.get("with", "")), str(layer.get("name", "")), i, str(spec.get("file", "")))
                measure(pack, spec, layer, row, target, tolerance, tp_limit)
                rows.append(row)
    return rows


def fmt(v: float | None, unit: str) -> str:
    if v is None:
        return "-"
    return f"{v:.1f} {unit}" if math.isfinite(v) else f"-inf {unit}"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("pack", type=Path, help="the pack folder, with manifest.json")
    ap.add_argument("--bed-target", type=float, default=-23.0, help="LUFS integrated for ambience beds")
    ap.add_argument("--tolerance", type=float, default=2.0, help="LU either side of the bed target")
    ap.add_argument("--true-peak", type=float, default=-1.0, help="dBTP no sound may exceed")
    ap.add_argument("--json", type=Path, help="also write the report here")
    args = ap.parse_args(argv)
    try:
        rows = check_pack(args.pack, args.bed_target, args.tolerance, args.true_peak)
    except PackError as e:
        print(f"{args.pack}: {e}", file=sys.stderr)
        return 2
    for r in rows:
        where = r.event + (f" / {r.layer}" if r.layer else "") + (f" #{r.variant}" if r.variant > 1 or r.layer else "")
        verdict = "ok" if r.ok else "FAIL: " + "; ".join(r.problems)
        print(
            f"{r.kind:8} {where:40} {fmt(r.lufs, 'LUFS'):>11} {fmt(r.true_peak_dbtp, 'dBTP'):>11} "
            f"{fmt(r.sample_peak_dbfs, 'dBFS'):>11}  {verdict}"
        )
    failed = sum(not r.ok for r in rows)
    print(
        f"{len(rows)} sounds, {failed} failing (beds {args.bed_target:g} LUFS +/- {args.tolerance:g}, "
        f"true peak <= {args.true_peak:g} dBTP)"
    )
    if args.json:

        def clean(v: object) -> object:
            return None if isinstance(v, float) and not math.isfinite(v) else v

        report = [{k: clean(v) for k, v in asdict(r).items()} | {"ok": r.ok} for r in rows]
        args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
