# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end gates that run the real game (hidden, scripted):

  equivalence  GPU compositing draws the same picture as CPU compositing
  fingerprint  smooth motion changes only the drawing: the full game state
               after 2,000 ticks is byte-for-byte the same with it on and off
  perf         a fully redrawn 4K restaurant frame stays under --max-p99 ms
               (99th percentile) and opening it under --max-first ms; only
               enforced on a real GPU, reported on the software renderer

Needs the original game files. Exit code 1 when a gate fails.

  python tools/ci/hd_gates.py --game build/release/plegacy \\
      --game-path /path/to/PIZZA --pack /path/to/hd-pack
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageChops


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
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    if args.software:
        env.setdefault("SDL_VIDEODRIVER", "offscreen")
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
    done = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=900, check=False)
    if done.returncode != 0:
        raise RuntimeError(f"{name}: game exited {done.returncode}\n{done.stderr[-2000:]}")
    return done.stdout


def gate_equivalence(args: argparse.Namespace, work: Path) -> bool:
    shots = {}
    for mode, env in (("gpu", {"PLEGACY_CPU_COMPOSE": ""}), ("cpu", {"PLEGACY_CPU_COMPOSE": "1"})):
        shot = work / f"eq_{mode}.png"
        script = (
            "restaurant qs 0 0\nnav restaurant 0\ngame tick 200\n" f"screenshot {shot.as_posix()}\n"
        )
        extra = ["--hd-scale", "3"] + (["--hd-pack", args.pack] if args.pack else [])
        run_game(args, work, script, f"eq_{mode}", extra, env)
        shots[mode] = Image.open(shot).convert("RGB")
    diff = ImageChops.difference(shots["gpu"], shots["cpu"])
    worst = max(hi for _, hi in diff.getextrema())
    ok = worst <= 4
    print(
        f"{'ok ' if ok else 'FAIL'} equivalence: GPU vs CPU compositing, largest "
        f"difference {worst}/255 (limit 4)"
    )
    return ok


def gate_fingerprint(args: argparse.Namespace, work: Path) -> bool:
    prints = {}
    for value in ("true", "false"):
        out = work / f"fp_{value}.txt"
        script = (
            f"game options set smooth_motion {value}\nrestaurant qs 0 0\n"
            f"nav restaurant 0\nbench 120\ngame tick 2000\n"
            f"dump fingerprint {out.as_posix()}\n"
        )
        run_game(args, work, script, f"fp_{value}", [])
        prints[value] = out.read_bytes()
    ok = prints["true"] == prints["false"] and len(prints["true"]) > 1000
    print(
        f"{'ok ' if ok else 'FAIL'} fingerprint: game state with smooth motion on and off "
        f"is {'identical' if ok else 'DIFFERENT'} ({len(prints['true'])} bytes)"
    )
    return ok


def gate_perf(args: argparse.Namespace, work: Path) -> bool:
    if not args.pack:
        print("skip perf: needs --pack (a 4K HD pack)")
        return True
    script = f"restaurant qs 0 0\nnav restaurant 0\ngame tick 300\nbench {args.bench_frames}\n"
    extra = ["--hd-scale", "6", "--hd-pack", args.pack]
    # Twice: the first run writes the warm list, the second is a normal session.
    run_game(args, work, script, "perf", extra)
    out = run_game(args, work, script, "perf", extra)
    line = next((ln for ln in out.splitlines() if "frames at" in ln), "")
    p99 = re.search(r"p99 ([\d.]+)", line)
    first = re.search(r"first frame ([\d.]+) ms", line)
    if not (p99 and first):
        print(f"FAIL perf: no bench result in output:\n{out[-1000:]}")
        return False
    p99_ms, first_ms = float(p99.group(1)), float(first.group(1))
    software = "software" in line
    ok = software or (p99_ms <= args.max_p99 and first_ms <= args.max_first)
    note = " (software renderer: reported only)" if software else ""
    print(
        f"{'ok ' if ok else 'FAIL'} perf: p99 {p99_ms:.2f} ms (limit {args.max_p99}), "
        f"first frame {first_ms:.2f} ms (limit {args.max_first}){note}\n     {line.strip()}"
    )
    return ok


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--game", required=True, help="the game executable")
    p.add_argument("--game-path", required=True, help="folder with PT.EXE")
    p.add_argument("--pack", default="", help="HD pack folder (perf and equivalence)")
    p.add_argument("--checks", default="equivalence,fingerprint,perf")
    p.add_argument(
        "--software", action="store_true", help="use SDL's software renderer (headless machines)"
    )
    p.add_argument("--bench-frames", type=int, default=300)
    p.add_argument("--max-p99", type=float, default=8.0)
    p.add_argument("--max-first", type=float, default=16.0)
    args = p.parse_args(argv)
    gates = {"equivalence": gate_equivalence, "fingerprint": gate_fingerprint, "perf": gate_perf}
    ok = True
    with tempfile.TemporaryDirectory(prefix="hd-gates-") as tmp:
        for name in args.checks.split(","):
            ok &= gates[name.strip()](args, Path(tmp))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
