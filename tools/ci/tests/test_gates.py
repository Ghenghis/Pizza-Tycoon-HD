# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""hd_gates.py's own behaviour (pass, skip, fail, evidence), run against fake_game.py."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

import hd_gates

FAKE = str(Path(__file__).with_name("fake_game.py"))


def gates(tmp_path: Path, *extra: str) -> int:
    return hd_gates.main(
        ["--game", FAKE, "--game-path", str(tmp_path), "--artifacts", str(tmp_path / "artifacts"), *extra]
    )


def test_every_gate_passes_and_the_evidence_is_removed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    monkeypatch.delenv("FAKE_FAIL", raising=False)
    pack = tmp_path / "pack"
    pack.mkdir()
    budget = tmp_path / "budget.json"
    budget.write_text(
        json.dumps({"setup": [], "screens": [{"name": "main-menu", "open": ["nav main"]}]}), encoding="utf-8"
    )
    common = ["--pack", str(pack), "--coverage-budget", str(budget), "--coverage-report", str(tmp_path / "rep")]
    assert gates(tmp_path, *common, "--checks", "coverage", "--coverage-update") == 0
    capsys.readouterr()
    assert gates(tmp_path, *common, "--fail-on-skip") == 0
    out = capsys.readouterr().out
    for gate in ("smoke", "equivalence", "fingerprint", "perf", "coverage"):
        assert f"PASS {gate}:" in out
    assert "PASS hd gates: 5 passed, 0 skipped" in out
    assert list((tmp_path / "artifacts").iterdir()) == []


def test_perf_without_a_pack_skips_or_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    log = tmp_path / "skipped.txt"
    monkeypatch.setenv("PLEGACY_SKIP_LOG", str(log))
    assert gates(tmp_path, "--checks", "perf") == 0
    assert "SKIP perf: needs a 4K HD pack" in capsys.readouterr().out
    assert "hd gates: perf: needs a 4K HD pack" in log.read_text(encoding="utf-8")
    assert gates(tmp_path, "--checks", "perf", "--fail-on-skip") == 1
    assert "FAIL perf: skipped, and --fail-on-skip is set" in capsys.readouterr().out


def test_a_failed_gate_keeps_its_evidence(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    # The CPU run gets another picture than the GPU run.
    monkeypatch.setenv("FAKE_COVERAGE", json.dumps({"nav restaurant 0": 0.5}))
    monkeypatch.setenv("FAKE_FAIL", json.dumps([]))
    original = hd_gates.launch_game

    def cpu_differs(*a: object, **kw: object) -> object:
        if a[3] == "eq_cpu":
            monkeypatch.setenv("FAKE_COVERAGE", json.dumps({"nav restaurant 0": 0.1}))
        return original(*a, **kw)  # type: ignore[arg-type]

    monkeypatch.setattr(hd_gates, "launch_game", cpu_differs)
    assert gates(tmp_path, "--checks", "equivalence") == 1
    out = capsys.readouterr().out
    assert "FAIL equivalence: GPU and CPU compositing differ" in out
    [run] = list((tmp_path / "artifacts").iterdir())
    names = {p.name for p in run.iterdir()}
    assert {"eq_gpu.png", "eq_cpu.png", "eq_diff.png", "eq_gpu.log", "eq_cpu.log", "eq_gpu.exec"} <= names


def test_a_missing_game_is_a_clear_failure(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    assert hd_gates.main(["--game", str(tmp_path / "nope"), "--game-path", str(tmp_path)]) == 1
    assert "FAIL hd gates: no game at" in capsys.readouterr().out
