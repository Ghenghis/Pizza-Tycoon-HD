# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""hdpack.py warm-list: a session's warm list shipped with the pack."""

from __future__ import annotations

from pathlib import Path

import pytest

import hdpack


def _pack(tmp_path: Path) -> Path:
    pack = tmp_path / "pack"
    for rel in ("gfx/bo.png", "bob/moebel/0116.png", "gfx/k0.png"):
        (pack / rel).parent.mkdir(parents=True, exist_ok=True)
        (pack / rel).write_bytes(b"\x89PNG")
    return pack


def test_the_pack_gets_the_session_lines_it_can_use(tmp_path: Path) -> None:
    pack = _pack(tmp_path)
    session = tmp_path / "hd_warm.txt"
    session.write_text(
        "# Pizza Tycoon HD: pack files used last session, decoded early\n"
        "640 400 gfx/bo.png restaurant\n"
        "56 44 bob/moebel/0116.png\n"
        "640 400 gfx/bo.png cityview\n"  # the same picture again
        "192 22 gfx/missing.png\n"  # not in this pack
        "10 10 ../outside.png\n"
        "0 22 gfx/k0.png\n"
        "192 22 gfx/k0.png menu4 extra\n"
        "\n",
        encoding="utf-8",
    )
    result = hdpack.install_warm_list(pack, session)
    assert result.kept == 2
    assert len(result.dropped) == 4
    text = (pack / "hd_warm.txt").read_text(encoding="utf-8")
    lines = [ln for ln in text.splitlines() if not ln.startswith("#")]
    assert lines == ["640 400 gfx/bo.png restaurant", "56 44 bob/moebel/0116.png"]
    assert text.startswith("# Pizza Tycoon HD: this pack's reference warm list")


def test_the_command_line(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    pack = _pack(tmp_path)
    session = tmp_path / "hd_warm.txt"
    session.write_text("192 22 gfx/k0.png menu4\n", encoding="utf-8")
    assert hdpack.main(["warm-list", str(pack), str(session)]) == 0
    assert "1 pictures, 0 lines dropped" in capsys.readouterr().out
    assert (pack / "hd_warm.txt").read_text(encoding="utf-8").endswith("192 22 gfx/k0.png menu4\n")
    # A missing list or pack is a clear error, not a traceback.
    assert hdpack.main(["warm-list", str(pack), str(tmp_path / "none.txt")]) == 2
    assert hdpack.main(["warm-list", str(tmp_path / "nopack"), str(session)]) == 2
