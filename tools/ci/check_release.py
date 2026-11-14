#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Release gate: checks a packaged Windows release the way a player gets it.

  1. checksums  every line of <zip>.sha256 (and <exe>.sha256) matches its file
  2. contents   the zip is safe to unpack (no absolute or ../ paths) and its
                files match tools/ci/release-manifest.txt: required files there
                (game, data/, LICENSE, notices), nothing else, no original game
                files
  3. unpack     into a temporary folder this run owns; the bare exe, when
                given, is the same file as the one in the zip
  4. version    the unpacked game, run hidden, answers --version (with
                --version-expected: "Pizza Tycoon HD <version>")
  5. smoke      with --game-path: a hidden --exec run (quick start, open the
                restaurant, 5 game seconds, screenshot) of the unpacked game,
                using only the zip's data/ and hd/ folders

Stops at the first failure. The temporary folder is always removed. A step
that cannot run here (no game files; a Windows exe on a machine that can't
start one, unless --launcher names a way, such as wine) is skipped and
listed; --require-game and --require-run make those skips failures. Exit
code 1 when a check fails.

  python tools/ci/check_release.py release/PizzaTycoonHD_v1.1.0_win64.zip \\
      --exe release/PizzaTycoonHD_v1.1.0_win64.exe --version-expected 1.1.0
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tools" / "ci" / "release-manifest.txt"
GAME_EXE = "Pizza Legacy.exe"
SMOKE_TICKS = 150  # 5 seconds at the game's 30 ticks a second

# Child processes never open a console window on Windows.
HIDDEN: dict[str, Any] = {}
if sys.platform == "win32":
    HIDDEN["creationflags"] = subprocess.CREATE_NO_WINDOW


class CheckError(Exception):
    """A check failed; the message says what and how to fix it."""


class CheckSkippedError(Exception):
    """A check cannot run on this machine or without game files."""


def ok(text: str) -> None:
    print(f"PASS {text}", flush=True)


def skip(text: str, fix: str) -> None:
    print(f"SKIP {text}\n     fix: {fix}", flush=True)
    log = os.environ.get("PLEGACY_SKIP_LOG")
    if log:
        with open(log, "a", encoding="utf-8") as f:
            f.write(f"release check: {text}\n")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# --------------------------------------------------------------------------
# 1. checksums


def check_checksums(files: list[Path]) -> str:
    checked = 0
    for f in files:
        listing = f.with_name(f.name + ".sha256")
        if not listing.is_file():
            raise CheckError(f"no checksum file {listing.name} next to {f.name}")
        lines = [ln for ln in listing.read_text(encoding="ascii", errors="replace").splitlines() if ln.strip()]
        if not lines:
            raise CheckError(f"{listing.name} is empty")
        named = set()
        for n, line in enumerate(lines, 1):
            m = re.fullmatch(r"([0-9a-fA-F]{64}) [ *](.+)", line.strip())
            if not m:
                raise CheckError(f"{listing.name}:{n}: expected '<sha256>  <file name>', got {line.strip()!r}")
            name = m.group(2).strip()
            if Path(name).name != name:
                raise CheckError(f"{listing.name}:{n}: {name!r} is not a plain file name")
            target = listing.with_name(name)
            if not target.is_file():
                raise CheckError(f"{listing.name}:{n}: {name} is not there")
            actual = sha256(target)
            if actual != m.group(1).lower():
                raise CheckError(
                    f"{listing.name}:{n}: {name} has SHA-256 {actual}, the file says {m.group(1).lower()}: "
                    "the file changed after it was packaged; package it again"
                )
            named.add(name)
            checked += 1
        if f.name not in named:
            raise CheckError(f"{listing.name} has no line for {f.name}")
    return f"checksums: {checked} SHA-256 line(s) in {len(files)} file(s) match"


# --------------------------------------------------------------------------
# 2. contents


@dataclass
class Manifest:
    require: list[str] = field(default_factory=list)
    allow: list[str] = field(default_factory=list)
    deny: list[str] = field(default_factory=list)


def load_manifest(path: Path) -> Manifest:
    m = Manifest()
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        kind, _, pattern = line.partition(" ")
        if kind not in ("require", "allow", "deny") or not pattern.strip():
            raise CheckError(f"{path.name}:{n}: expected 'require|allow|deny <pattern>', got {line!r}")
        getattr(m, kind).append(pattern.strip())
    if not m.allow or not m.require:
        raise CheckError(f"{path.name}: needs require and allow lines")
    return m


def glob_match(pattern: str, path: str) -> bool:
    """* within one folder, ** across folders, ? one character; case-insensitive (Windows paths)."""
    rx = ""
    i = 0
    while i < len(pattern):
        if pattern.startswith("**", i):
            rx, i = rx + ".*", i + 2
        elif pattern[i] == "*":
            rx, i = rx + "[^/]*", i + 1
        elif pattern[i] == "?":
            rx, i = rx + "[^/]", i + 1
        else:
            rx, i = rx + re.escape(pattern[i]), i + 1
    return re.fullmatch(rx, path, flags=re.IGNORECASE) is not None


def zip_files(zf: zipfile.ZipFile) -> dict[str, zipfile.ZipInfo]:
    """The zip's files by path (forward slashes, a single top folder removed); refuses unsafe names."""
    entries: dict[str, zipfile.ZipInfo] = {}
    for info in zf.infolist():
        # Windows PowerShell 5.1's Compress-Archive writes backslashes.
        name = info.filename.replace("\\", "/")
        parts = PurePosixPath(name).parts
        if name.startswith("/") or re.match(r"^[A-Za-z]:", name) or ".." in parts:
            raise CheckError(f"unsafe path in the zip: {info.filename!r}")
        if not name.endswith("/"):
            entries[name] = info
    if not entries:
        raise CheckError("the zip has no files")
    tops = {n.split("/", 1)[0] for n in entries}
    if len(tops) == 1 and all("/" in n for n in entries):
        top = tops.pop() + "/"
        entries = {n[len(top) :]: info for n, info in entries.items()}
    return entries


def check_contents(names: list[str], manifest: Manifest) -> str:
    exact_allows = {p.lower() for p in manifest.allow if not re.search(r"[*?]", p)}
    denied = [n for n in names if n.lower() not in exact_allows and any(glob_match(p, n) for p in manifest.deny)]
    if denied:
        raise CheckError(
            f"{len(denied)} file(s) that must never ship (original game files): {', '.join(denied[:20])}; "
            "remove them from the build's data folder and package again"
        )
    stray = [n for n in names if not any(glob_match(p, n) for p in manifest.allow)]
    if stray:
        raise CheckError(
            f"{len(stray)} file(s) the release manifest does not allow: {', '.join(stray[:20])}; "
            "stop packaging them, or add an allow line to tools/ci/release-manifest.txt"
        )
    missing = [p for p in manifest.require if not any(glob_match(p, n) for n in names)]
    if missing:
        raise CheckError(
            f"required file(s) missing: {', '.join(missing)}; the packaging script must copy them "
            "(scripts/release.ps1, scripts/build-windows-cross.sh)"
        )
    return f"contents: {len(names)} files, all allowed, none of the original game's, every required one there"


def unpack(zf: zipfile.ZipFile, entries: dict[str, zipfile.ZipInfo], dest: Path) -> None:
    for name, info in entries.items():
        target = dest / name
        if dest.resolve() not in target.resolve().parents:
            raise CheckError(f"unsafe path in the zip: {info.filename!r}")
        target.parent.mkdir(parents=True, exist_ok=True)
        with zf.open(info) as src, target.open("wb") as out:
            shutil.copyfileobj(src, out)


# --------------------------------------------------------------------------
# 4. and 5. running the unpacked game


def can_run_windows_exe() -> bool:
    if sys.platform == "win32":
        return True
    # WSL starts Windows programs when interop is on.
    return (
        Path("/proc/sys/fs/binfmt_misc/WSLInterop").exists()
        or Path("/proc/sys/fs/binfmt_misc/WSLInterop-late").exists()
    )


def run_hidden(cmd: list[str], cwd: Path, timeout: int) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ, SDL_AUDIO_DRIVER="dummy")
    try:
        done: subprocess.CompletedProcess[str] = subprocess.run(
            cmd,
            cwd=cwd,
            env=env,
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout,
            check=False,
            **HIDDEN,
        )
    except subprocess.TimeoutExpired as exc:
        raise CheckError(f"{Path(cmd[0]).name} {' '.join(cmd[1:3])}... did not finish within {timeout} s") from exc
    except OSError as exc:
        raise CheckError(f"cannot start {cmd[0]}: {exc}") from exc
    return done


def check_version(launch: list[str], exe: Path, expected: str, timeout: int) -> str:
    done = run_hidden([*launch, str(exe), "--version"], exe.parent, timeout)
    out = (done.stdout + done.stderr).strip()
    if done.returncode != 0:
        raise CheckError(f"the unpacked game's --version exited {done.returncode}: {out[-500:]}")
    first = next((ln for ln in out.splitlines() if ln.startswith("Version:")), "")
    if not first:
        raise CheckError(f"the unpacked game's --version printed no 'Version:' line: {out[-500:]}")
    if expected and f"Pizza Tycoon HD {expected}" not in first:
        raise CheckError(f"the unpacked game says {first!r}, the release is {expected}: package the right build")
    return f"version: {first}"


def check_smoke(launch: list[str], exe: Path, game_path: Path, work: Path, timeout: int) -> str:
    shot = work / "smoke.png"
    script = work / "smoke.exec"
    script.write_text(
        f"restaurant qs 0 0\nnav restaurant @\ngame tick {SMOKE_TICKS}\nscreenshot {shot.as_posix()}\n",
        encoding="utf-8",
    )
    cmd = [
        *launch,
        str(exe),
        "--config",
        str(work / "settings.conf"),
        "--game-path",
        str(game_path),
        "--hd-scale",
        "2",
        "--exec",
        str(script),
    ]
    started = time.monotonic()
    done = run_hidden(cmd, exe.parent, timeout)
    took = time.monotonic() - started
    if done.returncode != 0:
        raise CheckError(f"the --exec smoke run exited {done.returncode}: {(done.stdout + done.stderr)[-1500:]}")
    if not shot.is_file() or shot.stat().st_size < 1000:
        raise CheckError("the --exec smoke run wrote no screenshot")
    return f"smoke: quick start, restaurant, {SMOKE_TICKS // 30} game seconds and a screenshot in {took:.1f} s"


# --------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("zip", help="the release zip")
    p.add_argument("--exe", default="", help="the bare exe released next to the zip")
    p.add_argument("--version-expected", default="", help="the version the game must report")
    p.add_argument("--require", action="append", default=[], help="another required file (e.g. SDL3.dll)")
    p.add_argument("--manifest", default=str(MANIFEST))
    p.add_argument("--game-path", default="", help="folder with PT.EXE, for the --exec smoke run")
    p.add_argument("--require-game", action="store_true", help="a smoke run skipped for want of game files fails")
    p.add_argument("--require-run", action="store_true", help="a game that cannot be started here fails")
    p.add_argument("--timeout", type=int, default=120, help="seconds per game run")
    p.add_argument("--launcher", default="", help="run the game through this program (e.g. wine)")
    args = p.parse_args(argv)

    zip_path = Path(args.zip)
    if not zip_path.is_file():
        print(f"FAIL release check: no zip at {zip_path}\n     fix: run scripts/release.ps1 first")
        return 1
    skipped: list[str] = []
    try:
        ok(check_checksums([zip_path] + ([Path(args.exe)] if args.exe else [])))
        manifest = load_manifest(Path(args.manifest))
        manifest.require += args.require
        with tempfile.TemporaryDirectory(prefix="plegacy-release-check-") as tmp, zipfile.ZipFile(zip_path) as zf:
            entries = zip_files(zf)
            ok(check_contents(sorted(entries), manifest))
            unpacked = Path(tmp) / "game"
            unpack(zf, entries, unpacked)
            exe = next((unpacked / n for n in entries if n.lower() == GAME_EXE.lower()), unpacked / GAME_EXE)
            if args.exe:
                if sha256(Path(args.exe)) != sha256(exe):
                    raise CheckError(f"{Path(args.exe).name} is not the same file as {GAME_EXE} in the zip")
                ok(f"unpack: {len(entries)} files; {Path(args.exe).name} is the zip's {GAME_EXE}")
            else:
                ok(f"unpack: {len(entries)} files")
            launch = [args.launcher] if args.launcher else []
            runnable = bool(args.launcher) or can_run_windows_exe()
            for step in ("version", "smoke"):
                try:
                    if not runnable:
                        raise CheckSkippedError(f"{step}: this machine cannot start a Windows program")
                    if step == "version":
                        ok(check_version(launch, exe, args.version_expected, args.timeout))
                        continue
                    game = Path(args.game_path) if args.game_path else None
                    if game is None or not ((game / "PT.EXE").is_file() or (game / "pt.exe").is_file()):
                        raise CheckSkippedError("smoke: no game folder with PT.EXE (--game-path)")
                    work = Path(tmp) / "smoke"
                    work.mkdir()
                    ok(check_smoke(launch, exe, game, work, args.timeout))
                except CheckSkippedError as exc:
                    needs_game = "game folder" in str(exc)
                    if (needs_game and args.require_game) or (not needs_game and args.require_run):
                        raise CheckError(f"{exc}, and it is required here") from exc
                    skip(
                        str(exc),
                        (
                            "set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE"
                            if needs_game
                            else "run the check on Windows (or WSL with interop)"
                        ),
                    )
                    skipped.append(step)
    except (CheckError, zipfile.BadZipFile, OSError) as exc:
        print(f"FAIL release check: {exc}", flush=True)
        return 1
    print(f"PASS release check: {zip_path.name}" + (f" ({', '.join(skipped)} skipped)" if skipped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
