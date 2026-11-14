# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end gates that run the real game (hidden, scripted):

  smoke        the restaurant and the European map render at hd_scale 3 at
               1920x1200 with real content
  equivalence  GPU compositing draws the same picture as CPU compositing
  fingerprint  smooth motion changes only the drawing: the full game state
               after 2,000 ticks is byte-for-byte the same with it on and off
  perf         a fully redrawn 4K restaurant frame stays under --max-p99 ms
               (99th percentile) and opening it under --max-first ms; only
               enforced on a real GPU, reported on the software renderer

Needs the original game files. Without --pack, --make-pack first exports the
original art and builds a small baseline pack from it (Pillow and numpy);
without either, perf is skipped (a failure with --fail-on-skip).

Each run keeps its exec scripts, screenshots, game logs and, when the GPU and
CPU pictures differ, an amplified difference picture in a new folder under
--artifacts (default build/hd-gates). The folder is deleted when every gate
passes and kept when one fails. Exit code 1 when a gate fails.

  python tools/ci/hd_gates.py --game build/release/plegacy \\
      --game-path /path/to/PIZZA --make-pack
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import Any

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[2]
HDPACK = ROOT / "tools" / "hdpack" / "hdpack.py"

# Child processes never open a console window on Windows.
HIDDEN: dict[str, Any] = {}
if sys.platform == "win32":
    HIDDEN["creationflags"] = subprocess.CREATE_NO_WINDOW


class GateError(Exception):
    """A gate could not run to the end (the game failed, a file is missing)."""


class GateSkippedError(Exception):
    """A gate cannot run with what it was given."""


def run_game(
    args: argparse.Namespace,
    work: Path,
    script: str,
    name: str,
    extra: list[str],
    env_extra: dict[str, str] | None = None,
) -> str:
    exec_file = work / f"{name}.exec"
    exec_file.write_text(script, encoding="utf-8")
    env = dict(os.environ)
    env.setdefault("SDL_AUDIO_DRIVER", "dummy")
    if args.software:
        env.setdefault("SDL_VIDEO_DRIVER", "offscreen")
        env["SDL_RENDER_DRIVER"] = "software"
    env.update(env_extra or {})
    cmd = [
        args.game,
        "--config",
        str(work / f"{name}.cfg"),
        "--game-path",
        args.game_path,
        "--seed",
        "7",
        *extra,
        "--exec",
        str(exec_file),
    ]
    log = work / f"{name}.log"
    try:
        done: subprocess.CompletedProcess[str] = subprocess.run(
            cmd, env=env, capture_output=True, text=True, timeout=900, check=False, **HIDDEN
        )
    except subprocess.TimeoutExpired as exc:
        log.write_text(f"$ {' '.join(cmd)}\ntimed out after {exc.timeout} s\n", encoding="utf-8")
        raise GateError(f"{name}: the game did not finish within {exc.timeout} s (log: {log})") from exc
    except OSError as exc:
        raise GateError(f"{name}: cannot start {args.game}: {exc}") from exc
    log.write_text(
        f"$ {' '.join(cmd)}\nexit {done.returncode}\n--- stdout\n{done.stdout}\n--- stderr\n{done.stderr}",
        encoding="utf-8",
    )
    if done.returncode != 0:
        raise GateError(f"{name}: game exited {done.returncode} (log: {log})\n{done.stderr[-2000:]}")
    return done.stdout


def open_shot(path: Path) -> Image.Image:
    if not path.exists():
        raise GateError(f"the game wrote no screenshot at {path} (see the .log next to it)")
    with Image.open(path) as im:
        return im.convert("RGB")


def gate_smoke(args: argparse.Namespace, work: Path) -> str:
    shots = {name: work / f"smoke_{name}.png" for name in ("restaurant", "euromap")}
    script = (
        f"nav restaurant @\nscreenshot {shots['restaurant'].as_posix()}\n"
        f"nav euromap\nscreenshot {shots['euromap'].as_posix()}\n"
    )
    extra = ["--hd-scale", "3"] + (["--hd-pack", args.pack] if args.pack else [])
    run_game(args, work, script, "smoke", extra)
    problems = []
    for name, path in shots.items():
        im = open_shot(path)
        colours = len(im.resize((64, 40)).getcolors(64 * 40) or [])
        if im.size != (1920, 1200):
            problems.append(f"{name} is {im.size[0]}x{im.size[1]}, not 1920x1200")
        elif colours <= 50:
            problems.append(f"{name} looks empty ({colours} colours in a 64x40 thumbnail)")
    if problems:
        raise GateError("; ".join(problems))
    return "restaurant and European map render at 1920x1200" + (" with the pack" if args.pack else "")


def gate_equivalence(args: argparse.Namespace, work: Path) -> str:
    shots = {}
    for mode, env in (("gpu", {"PLEGACY_CPU_COMPOSE": ""}), ("cpu", {"PLEGACY_CPU_COMPOSE": "1"})):
        shot = work / f"eq_{mode}.png"
        script = (
            "game options set lighting false\n"  # lighting is GPU-only by design
            "restaurant qs 0 0\nnav restaurant 0\ngame tick 200\n"
            f"screenshot {shot.as_posix()}\n"
        )
        extra = ["--hd-scale", "3"] + (["--hd-pack", args.pack] if args.pack else [])
        run_game(args, work, script, f"eq_{mode}", extra, env)
        shots[mode] = open_shot(shot)
    if shots["gpu"].size != shots["cpu"].size:
        raise GateError(f"GPU picture is {shots['gpu'].size}, CPU picture {shots['cpu'].size}")
    diff = ImageChops.difference(shots["gpu"], shots["cpu"])
    # (low, high) per band for an RGB picture
    worst = max(int(band[1]) for band in diff.getextrema() if isinstance(band, tuple))
    if worst > 4:
        diff_file = work / "eq_diff.png"
        diff.point(lambda v: min(255, v * 32)).save(diff_file)
        raise GateError(
            f"GPU and CPU compositing differ by up to {worst}/255 (limit 4); "
            f"difference x32 in {diff_file}, pictures eq_gpu.png and eq_cpu.png"
        )
    return f"GPU vs CPU compositing, largest difference {worst}/255 (limit 4)"


def gate_fingerprint(args: argparse.Namespace, work: Path) -> str:
    prints = {}
    for value in ("true", "false"):
        out = work / f"fp_{value}.txt"
        script = (
            f"game options set smooth_motion {value}\nrestaurant qs 0 0\n"
            f"nav restaurant 0\nbench 120\ngame tick 2000\n"
            f"dump fingerprint {out.as_posix()}\n"
        )
        run_game(args, work, script, f"fp_{value}", [])
        if not out.exists():
            raise GateError(f"the game wrote no fingerprint at {out}")
        prints[value] = out.read_bytes()
    size = len(prints["true"])
    if size <= 1000:
        raise GateError(f"the fingerprint is only {size} bytes: the game state was not dumped")
    if prints["true"] != prints["false"]:
        raise GateError(f"game state differs with smooth motion on and off (fp_true.txt vs fp_false.txt, {size} bytes)")
    return f"game state with smooth motion on and off is identical ({size} bytes)"


def gate_perf(args: argparse.Namespace, work: Path) -> str:
    if not args.pack:
        raise GateSkippedError("needs a 4K HD pack: pass --pack or --make-pack")
    script = f"restaurant qs 0 0\nnav restaurant 0\ngame tick 300\nbench {args.bench_frames}\n"
    extra = ["--hd-scale", "6", "--hd-pack", args.pack]
    # Twice: the first run writes the warm list, the second is a normal session.
    run_game(args, work, script, "perf_warm", extra)
    out = run_game(args, work, script, "perf", extra)
    line = next((ln for ln in out.splitlines() if "frames at" in ln), "")
    p99 = re.search(r"p99 ([\d.]+)", line)
    first = re.search(r"first frame ([\d.]+) ms", line)
    if not (p99 and first):
        raise GateError(f"no bench result in the game's output (log: {work / 'perf.log'})")
    p99_ms, first_ms = float(p99.group(1)), float(first.group(1))
    result = f"p99 {p99_ms:.2f} ms (limit {args.max_p99}), first frame {first_ms:.2f} ms (limit {args.max_first})"
    if "software" in line:
        return f"{result}; software renderer: reported only\n     {line.strip()}"
    if p99_ms > args.max_p99 or first_ms > args.max_first:
        raise GateError(f"{result}\n     {line.strip()}")
    return f"{result}\n     {line.strip()}"


GATES: dict[str, Callable[[argparse.Namespace, Path], str]] = {
    "smoke": gate_smoke,
    "equivalence": gate_equivalence,
    "fingerprint": gate_fingerprint,
    "perf": gate_perf,
}


def make_pack(args: argparse.Namespace, scratch: Path) -> str:
    """Exports the original art and builds a baseline pack from part of it."""
    export, pack = scratch / "export", scratch / "pack"
    env = dict(os.environ, SDL_VIDEO_DRIVER="offscreen", SDL_AUDIO_DRIVER="dummy")
    steps = [
        [
            args.game,
            "--config",
            str(scratch / "export.cfg"),
            "--game-path",
            args.game_path,
            "--export-assets",
            str(export),
        ],
        [sys.executable, str(HDPACK), "upscale", str(export), str(pack), "--scale", "6"]
        + ["--only", "gfx/*", "--only", "bob/moebel/*"],
        [sys.executable, str(HDPACK), "validate", str(export), str(pack)],
    ]
    for cmd in steps:
        done = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=1800, check=False, **HIDDEN)
        if done.returncode != 0:
            raise GateError(f"making the pack failed: {' '.join(cmd)} exited {done.returncode}\n{done.stderr[-2000:]}")
    return str(pack)


def note_skip(text: str) -> None:
    log = os.environ.get("PLEGACY_SKIP_LOG")
    if log:
        with open(log, "a", encoding="utf-8") as f:
            f.write(f"hd gates: {text}\n")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--game", required=True, help="the game executable")
    p.add_argument("--game-path", required=True, help="folder with PT.EXE")
    p.add_argument("--pack", default="", help="HD pack folder (smoke, equivalence, perf)")
    p.add_argument("--make-pack", action="store_true", help="without --pack: build a baseline pack first")
    p.add_argument("--checks", default=",".join(GATES))
    p.add_argument("--software", action="store_true", help="use SDL's software renderer (headless machines)")
    p.add_argument("--bench-frames", type=int, default=300)
    p.add_argument("--max-p99", type=float, default=8.0)
    p.add_argument("--max-first", type=float, default=16.0)
    p.add_argument("--artifacts", default=str(ROOT / "build" / "hd-gates"), help="where each run's evidence goes")
    p.add_argument("--keep", action="store_true", help="keep the run's folder even when every gate passes")
    p.add_argument("--fail-on-skip", action="store_true", help="a skipped gate fails")
    args = p.parse_args(argv)
    names = [n.strip() for n in args.checks.split(",") if n.strip()]
    unknown = [n for n in names if n not in GATES]
    if unknown:
        p.error(f"unknown check(s) {', '.join(unknown)}; choose from {', '.join(GATES)}")
    if not Path(args.game).is_file():
        print(
            f"FAIL hd gates: no game at {args.game}\n     fix: build it first (scripts/build.ps1 or scripts/build.sh)"
        )
        return 1

    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    work = Path(args.artifacts) / f"run-{stamp}-{os.getpid()}"
    work.mkdir(parents=True)
    failed: list[str] = []
    skipped: list[str] = []
    with tempfile.TemporaryDirectory(prefix="hd-gates-pack-") as scratch:
        if not args.pack and args.make_pack:
            try:
                args.pack = make_pack(args, Path(scratch))
                print("PASS pack: exported the original art and built a baseline pack (gfx, furniture)")
            except (GateError, OSError, subprocess.SubprocessError) as exc:
                print(f"FAIL pack: {exc}")
                failed.append("pack")
        for name in [] if failed else names:  # no gates without the pack they were promised
            try:
                print(f"PASS {name}: {GATES[name](args, work)}", flush=True)
            except GateSkippedError as exc:
                if args.fail_on_skip:
                    print(f"FAIL {name}: skipped, and --fail-on-skip is set: {exc}")
                    failed.append(name)
                else:
                    print(f"SKIP {name}: {exc}")
                    skipped.append(name)
                    note_skip(f"{name}: {exc}")
            except (GateError, OSError) as exc:
                print(f"FAIL {name}: {exc}", flush=True)
                failed.append(name)
    if skipped:
        print(f"     skipped: {', '.join(skipped)}")
    if failed:
        print(f"FAIL hd gates: {', '.join(failed)}\n     evidence kept in {work}")
        return 1
    if args.keep:
        print(f"     evidence in {work}")
    else:
        shutil.rmtree(work, ignore_errors=True)
    print(f"PASS hd gates: {len(names) - len(skipped)} passed, {len(skipped)} skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
