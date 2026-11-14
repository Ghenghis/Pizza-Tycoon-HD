# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""loudness_check.py on generated sounds with known answers (EBU Tech 3341
cases 1, 2 and 3, a true peak between samples) and on whole packs."""

from __future__ import annotations

import hashlib
import json
import math
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import loudness_check as lc  # noqa: E402

RATE = 48000


def sine(level_dbfs: float, seconds: float, hz: float = 1000.0, rate: int = RATE, phase: float = 0.0) -> list[float]:
    a = 10.0 ** (level_dbfs / 20.0)
    return [a * math.sin(2.0 * math.pi * hz * i / rate + phase) for i in range(int(seconds * rate))]


def wav(path: Path, left: list[float], right: list[float] | None = None, rate: int = RATE) -> Path:
    channels = [left] if right is None else [left, right]
    frames = len(left)
    body = struct.pack(
        f"<{frames * len(channels)}h",
        *(max(-32768, min(32767, round(c[i] * 32767))) for i in range(frames) for c in channels),
    )
    fmt = struct.pack("<HHIIHH", 1, len(channels), rate, rate * 2 * len(channels), 2 * len(channels), 16)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        b"RIFF"
        + struct.pack("<I", 36 + len(body))
        + b"WAVE"
        + b"fmt "
        + struct.pack("<I", 16)
        + fmt
        + b"data"
        + struct.pack("<I", len(body))
        + body
    )
    return path


def measured(left: list[float], right: list[float] | None = None, rate: int = RATE) -> float | None:
    return lc.integrated_loudness(rate, [left, left if right is None else right])


def test_ebu_3341_case_1_and_2() -> None:
    tone = sine(-23.0, 20.0)
    assert measured(tone) == pytest.approx(-23.0, abs=0.1)
    tone = sine(-33.0, 20.0)
    assert measured(tone) == pytest.approx(-33.0, abs=0.1)


def test_ebu_3341_case_3_relative_gate() -> None:
    tone = sine(-36.0, 10.0) + sine(-23.0, 60.0) + sine(-36.0, 10.0)
    assert measured(tone) == pytest.approx(-23.0, abs=0.1)


def test_other_rates_and_the_absolute_gate() -> None:
    assert measured(sine(-23.0, 10.0, rate=44100), rate=44100) == pytest.approx(-23.0, abs=0.1)
    assert measured(sine(-23.0, 10.0, rate=22050), rate=22050) == pytest.approx(-23.0, abs=0.15)
    # Silence around a tone is gated out; only the few blocks that straddle
    # its edges count, as BS.1770 has it.
    tone = [0.0] * RATE * 5 + sine(-23.0, 10.0) + [0.0] * RATE * 5
    assert measured(tone) == pytest.approx(-23.0, abs=0.2)
    assert measured([0.0] * RATE) is None
    assert measured(sine(-23.0, 0.3)) is None  # under one block


def test_true_peak_finds_the_peak_between_samples() -> None:
    # A quarter of the rate at 45 degrees: every sample is 0.707 of the peak.
    tone = sine(-6.0206, 1.0, hz=RATE / 4, phase=math.pi / 4)
    sample_peak = max(abs(v) for v in tone)
    assert lc.db(sample_peak) == pytest.approx(-9.03, abs=0.05)
    assert lc.db(lc.true_peak([tone, tone])) == pytest.approx(-6.02, abs=0.3)
    # A low tone: true peak and sample peak agree.
    low = sine(-3.0, 1.0, hz=100)
    assert lc.db(lc.true_peak([low])) == pytest.approx(-3.0, abs=0.05)


def test_mono_plays_on_both_channels(tmp_path: Path) -> None:
    rate, channels = lc.read_wav(wav(tmp_path / "mono.wav", sine(-26.0, 5.0)).read_bytes())
    assert len(channels) == 1
    played = lc.as_played(rate, channels, {}, {})
    assert len(played) == 2
    assert lc.integrated_loudness(rate, played) == pytest.approx(-26.0, abs=0.1)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def manifest(pack: Path, effects: list[dict], ambience: list[dict] | None = None, version: int = 2) -> None:
    doc: dict = {"format": "pthd-sfx-overrides", "version": version, "effects": effects}
    if ambience is not None:
        doc["ambience"] = ambience
    (pack / "manifest.json").write_text(json.dumps(doc), encoding="utf-8")


def test_a_pack_against_its_targets(tmp_path: Path) -> None:
    pack = tmp_path / "pack"
    good_bed = wav(pack / "room.wav", sine(-23.0, 6.0))  # mono, on both channels
    loud_bed = wav(pack / "street.wav", sine(-12.0, 6.0), sine(-12.0, 6.0))
    quiet = wav(pack / "ring.wav", sine(-20.0, 1.0), sine(-20.0, 1.0))
    hot = wav(pack / "siren.wav", sine(-0.2, 1.0, hz=RATE / 4, phase=math.pi / 4))
    click = wav(pack / "click.wav", sine(-10.0, 0.1))
    manifest(
        pack,
        [
            {"event": "restaurant-noise-1", "file": "room.wav", "sha256": sha(good_bed)},
            # gain_db is what the game plays: -12 + -11 = -23, a pass.
            {"event": "restaurant-noise-2", "file": "street.wav", "sha256": sha(loud_bed), "gain_db": -11},
            {"event": "traffic-1", "file": "street.wav", "sha256": sha(loud_bed)},
            {
                "event": "phone-ringing",
                "variants": [{"file": "ring.wav", "sha256": sha(quiet)}, {"file": "siren.wav", "sha256": sha(hot)}],
            },
            {"event": "police-siren", "file": "ring.wav", "sha256": "0" * 64},
        ],
        [
            {
                "with": "restaurant-noise-1",
                "layers": [
                    {
                        "name": "cutlery",
                        "variants": [{"file": "click.wav", "sha256": sha(click)}],
                        "interval_ms": [1000, 2000],
                    }
                ],
            }
        ],
    )
    rows = lc.check_pack(pack)
    by = {(r.event, r.layer, r.variant): r for r in rows}
    assert by[("restaurant-noise-1", "", 1)].ok
    assert by[("restaurant-noise-1", "", 1)].kind == "bed"
    assert by[("restaurant-noise-2", "", 1)].ok
    street = by[("traffic-1", "", 1)]
    assert not street.ok and "LUFS is not -23" in street.problems[0]
    assert by[("phone-ringing", "", 1)].ok  # one-shots have no loudness target
    siren = by[("phone-ringing", "", 2)]
    assert not siren.ok and "true peak" in siren.problems[0]
    assert siren.sample_peak_dbfs is not None and siren.sample_peak_dbfs < -1.0  # only the true peak shows it
    assert "sha256" in by[("police-siren", "", 1)].problems[0]
    layer = by[("restaurant-noise-1", "cutlery", 1)]
    assert layer.kind == "layer" and layer.ok and layer.lufs is None  # 0.1 s: no integrated value

    report = tmp_path / "report.json"
    assert lc.main([str(pack), "--json", str(report)]) == 1
    data = json.loads(report.read_text(encoding="utf-8"))
    assert len(data) == len(rows)
    assert sum(not r["ok"] for r in data) == 3


def test_version_1_ignores_variants_and_a_bad_pack_is_refused(tmp_path: Path) -> None:
    pack = tmp_path / "pack"
    bed = wav(pack / "room.wav", sine(-23.0, 3.0))
    manifest(
        pack,
        [
            {
                "event": "restaurant-noise-1",
                "file": "room.wav",
                "sha256": sha(bed),
                "variants": [{"file": "missing.wav"}],
            }
        ],
        version=1,
    )
    rows = lc.check_pack(pack)
    assert len(rows) == 1 and rows[0].ok
    assert lc.main([str(pack)]) == 0
    manifest(pack, [], version=3)
    assert lc.main([str(pack)]) == 2


def test_without_numpy(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(lc, "np", None)
    tone = sine(-6.0206, 0.1, hz=RATE / 4, phase=math.pi / 4)
    assert lc.db(lc.true_peak([tone])) == pytest.approx(-6.02, abs=0.3)
