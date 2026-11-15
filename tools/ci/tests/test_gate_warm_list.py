# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""A pack the gates build ships the perf_warm session's warm list."""

from __future__ import annotations

import argparse
from pathlib import Path

import hd_gates

FAKE = str(Path(__file__).with_name("fake_game.py"))


def _args(tmp_path: Path, pack: Path, made: bool) -> argparse.Namespace:
    return argparse.Namespace(
        game=FAKE,
        game_path=str(tmp_path),
        pack=str(pack),
        made_pack=made,
        software=False,
        bench_frames=10,
        max_p99=8.0,
        max_first=16.0,
    )


def _pack(tmp_path: Path) -> Path:
    pack = tmp_path / "pack"
    for rel in ("gfx/bo.png", "bob/moebel/0116.png"):
        (pack / rel).parent.mkdir(parents=True, exist_ok=True)
        (pack / rel).write_bytes(b"\x89PNG")
    return pack


def test_a_built_pack_ships_the_warm_list(tmp_path: Path) -> None:
    pack = _pack(tmp_path)
    work = tmp_path / "work"
    work.mkdir()
    assert "p99" in hd_gates.gate_perf(_args(tmp_path, pack, True), work)
    lines = [
        ln for ln in (pack / "hd_warm.txt").read_text(encoding="utf-8").splitlines() if not ln.startswith("#")
    ]
    assert lines == ["640 400 gfx/bo.png restaurant", "56 44 bob/moebel/0116.png restaurant"]


def test_a_given_pack_is_left_alone(tmp_path: Path) -> None:
    pack = _pack(tmp_path)
    work = tmp_path / "work"
    work.mkdir()
    hd_gates.gate_perf(_args(tmp_path, pack, False), work)
    assert not (pack / "hd_warm.txt").exists()
