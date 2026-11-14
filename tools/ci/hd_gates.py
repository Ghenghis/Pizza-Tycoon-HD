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
  coverage     how much of each major screen is still drawn at 640x400: the
               share of flat 3x3 texel blocks at hd_scale 3, against the
               budgets in tools/ci/coverage-budget.json (fails when a screen
               gets worse); report in build/coverage (JSON, Markdown, masks)

Needs the original game files. Without --pack, --make-pack first exports the
original art and builds a small baseline pack from it (Pillow and numpy);
without either, perf is skipped (a failure with --fail-on-skip).

Each run keeps its exec scripts, screenshots, game logs and, when the GPU and
CPU pictures differ, an amplified difference picture in a new folder under
--artifacts (default build/hd-gates). The folder is deleted when every gate
passes and kept when one fails. Exit code 1 when a gate fails.

  python tools/ci/hd_gates.py --game build/release/plegacy \\
      --game-path /path/to/PIZZA --make-pack

Record coverage budgets (once per pack and renderer, then after each
improvement), review build/coverage/coverage.md, and commit the budget file:

  python tools/ci/hd_gates.py --game ... --game-path ... --make-pack \\
      --checks coverage --coverage-update
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
from numpy.typing import NDArray
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[2]
HDPACK = ROOT / "tools" / "hdpack" / "hdpack.py"
COVERAGE_BUDGET = ROOT / "tools" / "ci" / "coverage-budget.json"
ENGINE_W, ENGINE_H = 640, 400  # the engine's screen, in pixels

# Child processes never open a console window on Windows.
HIDDEN: dict[str, Any] = {}
if sys.platform == "win32":
    HIDDEN["creationflags"] = subprocess.CREATE_NO_WINDOW


class GateError(Exception):
    """A gate could not run to the end (the game failed, a file is missing)."""


class GateSkippedError(Exception):
    """A gate cannot run with what it was given."""


def game_command(game: str) -> list[str]:
    """The game, or a Python stand-in for it (*.py, run with this Python: the tests use one)."""
    return [sys.executable, game] if game.endswith(".py") else [game]


def run_game(
    args: argparse.Namespace,
    work: Path,
    script: str,
    name: str,
    extra: list[str],
    env_extra: dict[str, str] | None = None,
) -> str:
    return launch_game(args, work, script, name, extra, env_extra).stdout


def launch_game(
    args: argparse.Namespace,
    work: Path,
    script: str,
    name: str,
    extra: list[str],
    env_extra: dict[str, str] | None = None,
    ok_codes: tuple[int, ...] = (0,),
) -> subprocess.CompletedProcess[str]:
    """Runs one hidden --exec session. Exit code 1 means a command failed (the game prints which)."""
    exec_file = work / f"{name}.exec"
    exec_file.write_text(script, encoding="utf-8")
    env = dict(os.environ)
    env.setdefault("SDL_AUDIO_DRIVER", "dummy")
    if args.software:
        env.setdefault("SDL_VIDEO_DRIVER", "offscreen")
        env["SDL_RENDER_DRIVER"] = "software"
    env.update(env_extra or {})
    cmd = [
        *game_command(args.game),
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
    if done.returncode not in ok_codes:
        raise GateError(f"{name}: game exited {done.returncode} (log: {log})\n{done.stderr[-2000:]}")
    return done


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
            # The city map too: its cars, drawn camera and zoom are drawing
            # only as well.
            f"nav cityview @\nbench 120\n"
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


# --------------------------------------------------------------------------
# coverage


def flat_blocks(im: Image.Image, k: int) -> NDArray[np.bool_]:
    """Per engine pixel (a k x k block of texels): True when all its texels are equal."""
    a = np.asarray(im.convert("RGB"))
    h, w = a.shape[0], a.shape[1]
    if (w, h) != (ENGINE_W * k, ENGINE_H * k):
        raise GateError(f"the screenshot is {w}x{h}, not {ENGINE_W * k}x{ENGINE_H * k} (hd_scale {k})")
    blocks = a.reshape(ENGINE_H, k, ENGINE_W, k, 3)
    flat: NDArray[np.bool_] = (blocks == blocks[:, :1, :, :1, :]).all(axis=(1, 3, 4))
    return flat


@dataclass
class ScreenCoverage:
    name: str
    commands: list[str]
    flat: int | None = None
    blocks: int | None = None
    budget: float | None = None
    status: str = ""  # pass, better, worse, unbudgeted, unreachable
    error: str = ""
    mask: NDArray[np.bool_] | None = field(default=None, repr=False)

    @property
    def share(self) -> float | None:
        return None if self.flat is None or not self.blocks else self.flat / self.blocks


def load_coverage_budget(path: Path) -> dict[str, Any]:
    """The budget file, checked: unique lower-case screen names, a list of commands per screen."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateError(f"cannot read the coverage budget {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise GateError(f"{path}: expected a JSON object")
    problems = []
    commands: list[str] = list(data.get("setup", []))
    names: set[str] = set()
    for i, screen in enumerate(data.get("screens", [])):
        name = str(screen.get("name", "")) if isinstance(screen, dict) else ""
        opens = screen.get("open", []) if isinstance(screen, dict) else []
        if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", name):
            problems.append(f"screens[{i}]: name {name!r} is not lower-case-with-dashes")
        elif name in names:
            problems.append(f"screens[{i}]: {name} is listed twice")
        names.add(name)
        if not isinstance(opens, list) or not opens or not all(isinstance(c, str) and c.strip() for c in opens):
            problems.append(f"screens[{i}] ({name}): 'open' must be a non-empty list of commands")
        else:
            commands += opens
    problems += [f"command {c!r} spans lines" for c in commands if "\n" in c or "\r" in c]
    if not names:
        problems.append("no screens listed")
    if not isinstance(data.get("budgets", {}), dict):
        problems.append("'budgets' must be an object")
    if problems:
        raise GateError(f"{path}: " + "; ".join(problems))
    return dict(data)


def coverage_profile(args: argparse.Namespace) -> str:
    """'<pack>@<renderer>': budgets differ per pack and between the GPU and software renderers."""
    if args.coverage_profile:
        return str(args.coverage_profile)
    pack = "baseline" if args.made_pack else (Path(args.pack).name if args.pack else "no-pack")
    return f"{pack}@{'software' if args.software else 'gpu'}"


def measure_coverage(args: argparse.Namespace, work: Path, data: dict[str, Any], k: int) -> list[ScreenCoverage]:
    """Opens every listed screen in one hidden run and measures its screenshot."""
    screens = [ScreenCoverage(s["name"], list(s["open"])) for s in data["screens"]]
    setup = [c.strip() for c in data.get("setup", [])]
    lines = list(setup)
    owner: dict[str, list[ScreenCoverage]] = {}
    shots: dict[str, Path] = {}
    for sc in screens:
        shots[sc.name] = work / f"cov_{sc.name}.png"
        for c in [*sc.commands, f"screenshot {shots[sc.name].as_posix()}"]:
            lines.append(c.strip())
            owner.setdefault(c.strip(), []).append(sc)
    extra = ["--hd-scale", str(k)] + (["--hd-pack", args.pack] if args.pack else [])
    # Exit code 1: some command failed; the game names each one on stderr.
    done = launch_game(args, work, "\n".join(lines) + "\n", "coverage", extra, ok_codes=(0, 1))
    failed = [m.group(1).strip() for m in re.finditer(r"error: command failed: (.*)", done.stderr)]
    bad_setup = [c for c in failed if c in setup]
    if bad_setup:
        raise GateError(f"coverage setup failed: {', '.join(bad_setup)} (log: {work / 'coverage.log'})")
    for c in failed:
        for sc in owner.get(c, []):
            sc.error = sc.error or f"'{c}' failed"
    for sc in screens:
        if sc.error:
            continue
        if not shots[sc.name].exists():
            sc.error = "no screenshot was written"
            continue
        sc.mask = flat_blocks(open_shot(shots[sc.name]), k)
        sc.flat, sc.blocks = int(sc.mask.sum()), int(sc.mask.size)
    return screens


def judge_coverage(screens: list[ScreenCoverage], budgets: dict[str, Any], tolerance: float) -> None:
    for sc in screens:
        value = budgets.get(sc.name)
        sc.budget = float(value) if isinstance(value, (int, float)) else None
        share = sc.share
        if sc.error or share is None:
            sc.status = "unreachable"
        elif sc.budget is None:
            sc.status = "unbudgeted"
        elif share > sc.budget + tolerance:
            sc.status = "worse"
        elif share < sc.budget - tolerance:
            sc.status = "better"
        else:
            sc.status = "pass"


def git_commit() -> str:
    try:
        done = subprocess.run(
            ["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
            **HIDDEN,
        )
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    return str(done.stdout).strip() or "unknown"


def pct(value: float | None) -> str:
    return "-" if value is None else f"{value * 100:.1f}%"


def write_coverage_report(
    report_dir: Path, profile: str, k: int, tolerance: float, screens: list[ScreenCoverage], budget_file: Path
) -> Path:
    """coverage.json, coverage.md and masks/<screen>.png (white = flat) in report_dir."""
    masks = report_dir / "masks"
    masks.mkdir(parents=True, exist_ok=True)
    for old in masks.glob("*.png"):
        old.unlink()
    for sc in screens:
        if sc.mask is not None:
            Image.fromarray(sc.mask.astype(np.uint8) * 255).save(masks / f"{sc.name}.png")
    date, commit = dt.date.today().isoformat(), git_commit()
    measured = [sc.share for sc in screens if sc.share is not None]
    mean = round(sum(measured) / len(measured), 4) if measured else None
    statuses = ("worse", "unreachable", "unbudgeted", "better", "pass")
    summary = {s: sum(1 for sc in screens if sc.status == s) for s in statuses}
    doc = {
        "profile": profile,
        "date": date,
        "commit": commit,
        "hd_scale": k,
        "tolerance": tolerance,
        "budget_file": budget_file.as_posix(),
        "mean_flat_share": mean,
        "summary": summary,
        "screens": [
            {
                "name": sc.name,
                "open": sc.commands,
                "flat_blocks": sc.flat,
                "blocks": sc.blocks,
                "flat_share": None if sc.share is None else round(sc.share, 4),
                "budget": sc.budget,
                "status": sc.status,
                "error": sc.error,
            }
            for sc in screens
        ],
    }
    (report_dir / "coverage.json").write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    rows = sorted(screens, key=lambda sc: 2.0 if sc.share is None else sc.share, reverse=True)
    md = [
        f"# HD screen coverage: {profile}",
        "",
        f"{date}, commit {commit}, hd_scale {k}. Flat: the share of {k}x{k} texel blocks whose",
        "texels are all equal, the look of an upscaled 640x400 pixel (lower is more HD).",
        "Worst first; white in `masks/<screen>.png` marks the flat pixels.",
        "",
        f"Mean flat share: {pct(mean)}. " + ", ".join(f"{n} {s}" for s, n in summary.items() if n) + ".",
        "",
        "| Screen | Opened with | Flat | Budget | Status |",
        "|---|---|---|---|---|",
    ]
    for sc in rows:
        status = sc.status + (f": {sc.error}" if sc.error else "")
        md.append(f"| {sc.name} | `{'; '.join(sc.commands)}` | {pct(sc.share)} | {pct(sc.budget)} | {status} |")
    (report_dir / "coverage.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    return report_dir / "coverage.md"


def update_coverage_budget(path: Path, data: dict[str, Any], profile: str, screens: list[ScreenCoverage]) -> None:
    budgets = dict(data.get("budgets", {}))
    budgets[profile] = {
        "measured": dt.date.today().isoformat(),
        "commit": git_commit(),
        "screens": {sc.name: round(sc.share, 4) for sc in screens if sc.share is not None},
    }
    path.write_text(json.dumps({**data, "budgets": dict(sorted(budgets.items()))}, indent=2) + "\n", encoding="utf-8")


def gate_coverage(args: argparse.Namespace, work: Path) -> str:
    budget_file = Path(args.coverage_budget)
    data = load_coverage_budget(budget_file)
    k = int(data.get("hd_scale", 3))
    tolerance = float(data.get("tolerance", 0.002))
    profile = coverage_profile(args)
    screens = measure_coverage(args, work, data, k)
    budgets = data.get("budgets", {}).get(profile, {}).get("screens", {})
    judge_coverage(screens, budgets, tolerance)
    report = write_coverage_report(Path(args.coverage_report), profile, k, tolerance, screens, budget_file)
    unreachable = [f"{sc.name} ({sc.error})" for sc in screens if sc.status == "unreachable"]
    worse = [f"{sc.name} {pct(sc.share)} > budget {pct(sc.budget)}" for sc in screens if sc.status == "worse"]
    better = [sc.name for sc in screens if sc.status == "better"]
    unbudgeted = [sc.name for sc in screens if sc.status == "unbudgeted"]
    stale = sorted(n for n in budgets if n not in {sc.name for sc in screens})
    measured = [sc.share for sc in screens if sc.share is not None]
    mean = sum(measured) / len(measured) if measured else None
    head = f"{len(screens)} screens, profile {profile}, mean flat {pct(mean)}; report {report}"
    if args.coverage_update:
        update_coverage_budget(budget_file, data, profile, screens)
        if unreachable:
            raise GateError(
                f"budget for {profile} written to {budget_file}, but {len(unreachable)} screen(s) could not be "
                f"opened: {'; '.join(unreachable)}. Fix their 'open' commands or take them off the list"
            )
        return f"{head}\n     budget for {profile} written to {budget_file}: review the report, then commit it"
    problems = []
    if worse:
        problems.append(f"{len(worse)} screen(s) got worse: {'; '.join(worse)}")
    if unreachable:
        problems.append(f"{len(unreachable)} screen(s) could not be opened: {'; '.join(unreachable)}")
    if problems:
        raise GateError(f"{head}\n     " + "\n     ".join(problems))
    if unbudgeted:
        raise GateSkippedError(
            f"{len(unbudgeted)} of {len(screens)} screens have no budget for {profile} ({head}). Record them: "
            "hd_gates.py --checks coverage --coverage-update (scripts: -UpdateCoverage or "
            f"PLEGACY_COVERAGE_UPDATE=1), review the report, commit {budget_file.name}"
        )
    notes = []
    if better:
        notes.append(f"improved: {', '.join(better)}; lower their budgets with --coverage-update")
    if stale:
        notes.append(f"budget entries for screens no longer listed: {', '.join(stale)}")
    return head + "".join(f"\n     {n}" for n in notes)


GATES: dict[str, Callable[[argparse.Namespace, Path], str]] = {
    "smoke": gate_smoke,
    "equivalence": gate_equivalence,
    "fingerprint": gate_fingerprint,
    "perf": gate_perf,
    "coverage": gate_coverage,
}


def make_pack(args: argparse.Namespace, scratch: Path) -> str:
    """Exports the original art and builds a baseline pack from part of it."""
    export, pack = scratch / "export", scratch / "pack"
    env = dict(os.environ, SDL_VIDEO_DRIVER="offscreen", SDL_AUDIO_DRIVER="dummy")
    steps = [
        [
            *game_command(args.game),
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
    p.add_argument("--coverage-budget", default=str(COVERAGE_BUDGET), help="screens and budgets (JSON)")
    p.add_argument("--coverage-report", default=str(ROOT / "build" / "coverage"), help="where the report goes")
    p.add_argument("--coverage-profile", default="", help="budget profile (default: <pack>@<gpu|software>)")
    p.add_argument("--coverage-update", action="store_true", help="record the measured shares as the budget")
    args = p.parse_args(argv)
    args.made_pack = False
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
    Path(args.artifacts).mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=f"run-{stamp}-", dir=args.artifacts))  # a new folder every run
    failed: list[str] = []
    skipped: list[str] = []
    with tempfile.TemporaryDirectory(prefix="hd-gates-pack-") as scratch:
        if not args.pack and args.make_pack:
            try:
                args.pack = make_pack(args, Path(scratch))
                args.made_pack = True
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
