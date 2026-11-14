# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""The release gate, on synthetic releases; fake_game.py plays the packaged exe."""

from __future__ import annotations

import hashlib
import sys
import tempfile
import zipfile
from pathlib import Path

import pytest

import check_release

FAKE_GAME = Path(__file__).with_name("fake_game.py").read_bytes()
GOOD = {
    "Pizza Legacy.exe": FAKE_GAME,
    "SDL3.dll": b"dll",
    "data/furniture.json": b"{}",
    "data/fonts/hd_fonts.json": b"{}",
    "data/fonts/FiraSans-Medium.ttf": b"ttf",
    "data/fonts/OFL-firasans.txt": b"OFL",
    "data/game_location.vga": b"vga",
    "hd/gfx/titel1.png": b"png",
    "tools/hdpack/hdpack.py": b"# tool",
    "tools/hdpack/workflows/upscale_only_api.json": b"{}",
    "LICENSE": b"GPL",
    "LICENSES/GPL-3.0-or-later.txt": b"GPL",
    "README.md": b"#",
    "SECURITY.md": b"#",
    "CREDITS.md": b"#",
    "CHANGELOG.md": b"#",
    "HD_REMASTER.md": b"#",
}


def release(
    tmp_path: Path, files: dict[str, bytes] | None = None, top: str = "", backslashes: bool = False
) -> list[str]:
    """Writes a zip, the bare exe and their .sha256 files; returns the check's arguments."""
    files = GOOD if files is None else files
    out = tmp_path / "release"
    out.mkdir(exist_ok=True)
    zip_path, exe = out / "PizzaTycoonHD_v9.9.9_win64.zip", out / "PizzaTycoonHD_v9.9.9_win64.exe"
    with zipfile.ZipFile(zip_path, "w") as zf:
        for name, data in files.items():
            entry = (f"{top}/" if top else "") + name
            zf.writestr(entry.replace("/", "\\") if backslashes else entry, data)
    exe.write_bytes(files.get("Pizza Legacy.exe", b"exe"))
    for f in (zip_path, exe):
        f.with_name(f.name + ".sha256").write_text(f"{hashlib.sha256(f.read_bytes()).hexdigest()}  {f.name}\n")
    return [str(zip_path), "--exe", str(exe), "--version-expected", "9.9.9"]


@pytest.fixture(autouse=True)
def own_temp(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """The check's temporary folders go here, so the tests can see they are removed."""
    temp = tmp_path / "temp"
    temp.mkdir()
    monkeypatch.setattr(tempfile, "tempdir", str(temp))
    return temp


def run(args: list[str], capsys: pytest.CaptureFixture[str]) -> tuple[int, str]:
    code = check_release.main(args)
    return code, capsys.readouterr().out


@pytest.mark.parametrize("top, backslashes", [("", False), ("PizzaTycoonHD_v9.9.9_win64", False), ("", True)])
def test_a_good_release_passes(
    tmp_path: Path, own_temp: Path, capsys: pytest.CaptureFixture[str], top: str, backslashes: bool
) -> None:
    game = tmp_path / "game"
    game.mkdir()
    (game / "PT.EXE").write_bytes(b"")
    code, out = run(
        release(tmp_path, top=top, backslashes=backslashes)
        + ["--launcher", sys.executable, "--game-path", str(game), "--require-game", "--require-run"],
        capsys,
    )
    assert code == 0, out
    assert "PASS checksums: 2 SHA-256 line(s) in 2 file(s) match" in out
    assert "PASS contents: 17 files" in out
    assert "PASS version: Version: Pizza Tycoon HD 9.9.9" in out
    assert "PASS smoke: quick start, restaurant, 5 game seconds and a screenshot" in out
    assert list(own_temp.iterdir()) == []  # its unpacked copy is gone


def test_without_game_files_the_smoke_run_is_skipped_or_required(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    log = tmp_path / "skipped.txt"
    monkeypatch.setenv("PLEGACY_SKIP_LOG", str(log))
    args = release(tmp_path) + ["--launcher", sys.executable]
    code, out = run(args, capsys)
    assert code == 0
    assert "SKIP smoke: no game folder with PT.EXE" in out
    assert "release check: smoke: no game folder" in log.read_text(encoding="utf-8")
    code, out = run([*args, "--require-game"], capsys)
    assert code == 1
    assert "FAIL release check: smoke: no game folder with PT.EXE (--game-path), and it is required here" in out


def test_the_wrong_version_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    args = release(tmp_path) + ["--launcher", sys.executable]
    args[args.index("--version-expected") + 1] = "1.1.0"
    code, out = run(args, capsys)
    assert code == 1
    assert "the release is 1.1.0: package the right build" in out


def test_a_changed_file_fails_its_checksum(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    args = release(tmp_path)
    with open(args[0], "ab") as f:
        f.write(b"tampered")
    code, out = run(args, capsys)
    assert code == 1
    assert "the file changed after it was packaged" in out


def test_a_missing_checksum_file_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    args = release(tmp_path)
    Path(args[0] + ".sha256").unlink()
    code, out = run(args, capsys)
    assert code == 1
    assert "no checksum file PizzaTycoonHD_v9.9.9_win64.zip.sha256" in out


@pytest.mark.parametrize(
    "extra, message",
    [
        ({"data/PALETTE.E": b""}, "must never ship (original game files): data/PALETTE.E"),
        ({"data/back1.vga": b""}, "must never ship (original game files): data/back1.vga"),
        ({"export/gfx/back1.png": b""}, "does not allow: export/gfx/back1.png"),
        ({"notes.txt": b""}, "does not allow: notes.txt"),
    ],
)
def test_files_that_must_not_ship_fail(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], extra: dict[str, bytes], message: str
) -> None:
    code, out = run(release(tmp_path, {**GOOD, **extra}), capsys)
    assert code == 1
    assert message in out


def test_missing_notices_fail(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    files = {k: v for k, v in GOOD.items() if k not in ("LICENSE", "data/fonts/hd_fonts.json")}
    code, out = run(release(tmp_path, files), capsys)
    assert code == 1
    assert "required file(s) missing: data/fonts/hd_fonts.json, LICENSE" in out


def test_an_extra_requirement_is_checked(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    files = {k: v for k, v in GOOD.items() if k != "SDL3.dll"}
    code, out = run(release(tmp_path, files) + ["--require", "SDL3.dll"], capsys)
    assert code == 1
    assert "required file(s) missing: SDL3.dll" in out


def test_an_unsafe_zip_is_not_unpacked(tmp_path: Path, own_temp: Path, capsys: pytest.CaptureFixture[str]) -> None:
    code, out = run(release(tmp_path, {**GOOD, "../evil.dll": b"x"}), capsys)
    assert code == 1
    assert "unsafe path in the zip: '../evil.dll'" in out
    assert not (tmp_path / "evil.dll").exists()
    assert list(own_temp.iterdir()) == []


def test_the_bare_exe_must_be_the_zips(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    args = release(tmp_path)
    exe = Path(args[args.index("--exe") + 1])
    exe.write_bytes(b"another build")
    exe.with_name(exe.name + ".sha256").write_text(f"{hashlib.sha256(b'another build').hexdigest()}  {exe.name}\n")
    code, out = run(args, capsys)
    assert code == 1
    assert "is not the same file as Pizza Legacy.exe in the zip" in out


def test_a_machine_that_cannot_start_the_game_skips_or_fails(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(check_release, "can_run_windows_exe", lambda: False)
    code, out = run(release(tmp_path), capsys)
    assert code == 0
    assert "SKIP version: this machine cannot start a Windows program" in out
    code, out = run(release(tmp_path) + ["--require-run"], capsys)
    assert code == 1
    assert "cannot start a Windows program, and it is required here" in out
