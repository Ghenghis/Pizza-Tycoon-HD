#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
"""Static scans of the repository (run by scripts/scan.ps1 and scripts/scan.sh).

  forbidden  markers of unfinished work in the fork's code (src, tests,
             tools, scripts, cmake): to-do/fix-me notes, stubs,
             placeholders, unimplemented paths, "coming soon", empty catch
             blocks. Allowed hits: tools/ci/forbidden-allowlist.txt
  secrets    keys, tokens and private machine paths. gitleaks scans the git
             history when it is installed; a built-in scan of the working
             tree always runs. Allowed hits: tools/ci/secrets-allowlist.txt
  deps       known vulnerabilities in the pack tool's pinned Python
             requirements (pip-audit, when installed)
  bandit     security lint of the Python tools (bandit, when installed)

Allowlist lines are "rule | path | text | expires | reason": path is a glob
(* matches any characters, slashes too), text must appear in the hit's line
(* = any line), expires is YYYY-MM-DD. An expired entry no longer counts.

Prints one PASS, FAIL or SKIP line per scan, then a summary of what was
skipped. Exit code 1 when a scan fails. With --require-tools (or
PLEGACY_REQUIRE_TOOLS=1) a scan skipped for want of a tool fails too.

  python tools/ci/scan.py                     # every scan
  python tools/ci/scan.py forbidden secrets   # some of them
"""

from __future__ import annotations

import argparse
import datetime as dt
import fnmatch
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
FORBIDDEN_ALLOWLIST = ROOT / "tools" / "ci" / "forbidden-allowlist.txt"
SECRETS_ALLOWLIST = ROOT / "tools" / "ci" / "secrets-allowlist.txt"
BANDIT_ALLOWLIST = ROOT / "tools" / "ci" / "bandit-allowlist.txt"
GITLEAKS_CONFIG = ROOT / "tools" / "ci" / "gitleaks.toml"
PY_REQUIREMENTS = ROOT / "tools" / "hdpack" / "requirements-dev.txt"

# Child processes never open a console window on Windows.
HIDDEN: dict[str, Any] = {}
if sys.platform == "win32":
    HIDDEN["creationflags"] = subprocess.CREATE_NO_WINDOW


# --------------------------------------------------------------------------
# Results


@dataclass
class Outcome:
    status: str  # PASS, FAIL or SKIP
    name: str
    detail: str
    lines: list[str] = field(default_factory=list)
    fix: str = ""
    missing_tool: bool = False


def report(o: Outcome) -> None:
    print(f"{o.status} {o.name}: {o.detail}", flush=True)
    for line in o.lines:
        print(f"     {line}")
    if o.fix:
        print(f"     fix: {o.fix}")
    if o.status == "SKIP":
        log = os.environ.get("PLEGACY_SKIP_LOG")
        if log:
            with open(log, "a", encoding="utf-8") as f:
                f.write(f"scan {o.name}: {o.detail}" + (f" (fix: {o.fix})" if o.fix else "") + "\n")


def run(cmd: list[str], timeout: int = 1800) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        check=False,
        **HIDDEN,
    )


def find_tool(module: str, exe: str) -> list[str] | None:
    """The tool as `python -m module` when importable, else from PATH."""
    if importlib.util.find_spec(module) is not None:
        return [sys.executable, "-m", module]
    path = shutil.which(exe)
    return [path] if path else None


# --------------------------------------------------------------------------
# Files


# Never scanned: build output, fetched dependencies, caches, packs.
SKIP_DIRS = {
    ".git",
    "build",
    "deps",
    "release",
    "export",
    "hd",
    "shots",
    "__pycache__",
    ".mypy_cache",
    ".pytest_cache",
    ".ruff_cache",
    "node_modules",
}


def repo_files(*tops: str) -> list[str]:
    """Tracked and new (not ignored) files, repo-relative with forward slashes."""
    done: subprocess.CompletedProcess[str] | None
    try:
        done = run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *tops], 120)
    except (OSError, subprocess.SubprocessError):
        done = None  # no git: walk the folders below
    if done is not None and done.returncode == 0:
        names = sorted({n for n in done.stdout.split("\0") if n})
        return [n for n in names if (ROOT / n).is_file()]
    # Not a git checkout: walk the folders instead.
    out: list[str] = []
    starts = [ROOT / t for t in tops] if tops else [ROOT]
    for start in starts:
        if start.is_file():
            out.append(start.relative_to(ROOT).as_posix())
            continue
        for dirpath, dirnames, filenames in os.walk(start):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and not d.startswith("pack")]
            for name in filenames:
                out.append((Path(dirpath) / name).relative_to(ROOT).as_posix())
    return sorted(set(out))


def read_text(rel: str, limit: int = 4 << 20) -> str | None:
    """The file as text, or None for binary and very large files."""
    path = ROOT / rel
    try:
        if path.stat().st_size > limit:
            return None
        data = path.read_bytes()
    except OSError:
        return None
    if b"\0" in data[:8192]:
        return None
    return data.decode("utf-8", errors="replace")


# --------------------------------------------------------------------------
# Allowlists


@dataclass
class Allow:
    lineno: int
    rule: str
    path: str
    text: str
    expires: dt.date
    reason: str
    used: int = 0

    def matches(self, rule: str, path: str, line: str) -> bool:
        return (
            self.rule in ("*", rule)
            and fnmatch.fnmatchcase(path, self.path)
            and (self.text == "*" or self.text in line)
        )


def load_allowlist(path: Path) -> tuple[list[Allow], list[str]]:
    entries: list[Allow] = []
    errors: list[str] = []
    if not path.exists():
        return entries, errors
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split(" | ")]
        if len(parts) != 5 or not all(parts):
            errors.append(f"{path.name}:{n}: expected 'rule | path | text | expires | reason'")
            continue
        try:
            expires = dt.date.fromisoformat(parts[3])
        except ValueError:
            errors.append(f"{path.name}:{n}: expiry '{parts[3]}' is not YYYY-MM-DD")
            continue
        entries.append(Allow(n, parts[0], parts[1], parts[2], expires, parts[4]))
    return entries, errors


@dataclass
class Hit:
    rule: str
    path: str
    lineno: int
    line: str
    hide: bool = False  # never print the line (it may hold a secret)

    def show(self) -> str:
        text = "(value hidden)" if self.hide else self.line.strip()[:160]
        return f"{self.path}:{self.lineno}: [{self.rule}] {text}"


def apply_allowlist(name: str, hits: list[Hit], path: Path, today: dt.date, fix: str) -> Outcome:
    entries, errors = load_allowlist(path)
    bad: list[str] = []
    expired: list[str] = []
    allowed = 0
    for hit in hits:
        entry = next((e for e in entries if e.matches(hit.rule, hit.path, hit.line)), None)
        if entry is None:
            bad.append(hit.show())
        elif entry.expires < today:
            entry.used += 1
            expired.append(f"{hit.show()}  (allowlist line {entry.lineno} expired {entry.expires})")
        else:
            entry.used += 1
            allowed += 1
    stale = [
        f"{path.name}:{e.lineno} matches nothing any more: delete it ({e.rule} | {e.path})"
        for e in entries
        if e.used == 0
    ]
    rel = path.relative_to(ROOT).as_posix()
    if errors or bad or expired:
        lines = errors + bad + expired + [f"note: {s}" for s in stale]
        what = []
        if errors:
            what.append(f"{len(errors)} allowlist error(s)")
        if bad:
            what.append(f"{len(bad)} hit(s) not in {rel}")
        if expired:
            what.append(f"{len(expired)} hit(s) whose allowlist entry expired")
        return Outcome("FAIL", name, ", ".join(what), lines, fix)
    lines = [f"note: {s}" for s in stale]
    return Outcome("PASS", name, f"no new hits ({allowed} allowed by {rel})", lines)


# --------------------------------------------------------------------------
# forbidden


C_LIKE = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".in"}
HASH_LIKE = {".py", ".sh", ".bash", ".ps1", ".psm1", ".cmake"}
FORBIDDEN_TOPS = ("src", "tests", "tools", "scripts", "cmake", "CMakeLists.txt")

# Each rule: name, pattern, where ("comment" = only in comments, "any" =
# comments, strings and code). Words are matched whole.
FORBIDDEN_RULES: list[tuple[str, re.Pattern[str], str]] = [
    ("marker", re.compile(r"\b(?:todo|fixme|xxx|hack)\b", re.I), "comment"),
    ("marker", re.compile(r"\b(?:TODO|FIXME)\b"), "any"),
    ("stub", re.compile(r"\bstub(?:s|bed)?\b", re.I), "comment"),
    ("stub", re.compile(r"\bSTUB\b"), "any"),
    ("placeholder", re.compile(r"\bplaceholders?\b", re.I), "comment"),
    ("placeholder", re.compile(r"\bPLACEHOLDER\b"), "any"),
    (
        "not-implemented",
        re.compile(
            r"\bNOT_IMPLEMENTED\b|\bnot\s+(?:yet\s+)?implemented\b|\bunimplemented\b|\bNotImplementedError\b", re.I
        ),
        "any",
    ),
    ("coming-soon", re.compile(r"\bcoming\s+soon\b|\bmock\s+data\b", re.I), "any"),
]

EMPTY_CATCH = {
    "c": re.compile(r"\bcatch\s*\([^)]*\)\s*\{\s*\}"),
    "py": re.compile(r"^[ \t]*except\b[^\n]*:[ \t]*(?:#[^\n]*)?(?:\n[ \t]*)?\bpass\b[ \t]*(?:#[^\n]*)?$", re.M),
    "ps1": re.compile(r"\bcatch\b\s*(?:\[[^\]]+\]\s*)*\{\s*\}", re.I),
}


def language(rel: str, text: str) -> str | None:
    name = rel.rsplit("/", 1)[-1]
    suffix = Path(name).suffix.lower()
    if name == "CMakeLists.txt" or suffix == ".cmake":
        return "cmake"
    if suffix in C_LIKE:
        return "c"
    if suffix in HASH_LIKE:
        return suffix.lstrip(".").replace("bash", "sh").replace("psm1", "ps1")
    if not suffix and text.startswith("#!"):
        first = text.split("\n", 1)[0]
        if "python" in first:
            return "py"
        if "sh" in first:
            return "sh"
    return None


def split_comments(text: str, lang: str) -> Iterator[tuple[int, str, str]]:
    """Yields (line number, code part, comment part) for each line."""
    in_block = False  # inside /* */ (C) or <# #> (PowerShell)
    for n, line in enumerate(text.split("\n"), 1):
        code: list[str] = []
        comment: list[str] = []
        quote = ""
        i = 0
        while i < len(line):
            ch = line[i]
            if in_block:
                end = "*/" if lang == "c" else "#>"
                j = line.find(end, i)
                if j < 0:
                    comment.append(line[i:])
                    break
                comment.append(line[i:j])
                i = j + 2
                in_block = False
                continue
            if quote:
                code.append(ch)
                if ch == "\\" and lang in ("c", "py"):
                    code.append(line[i + 1 : i + 2])
                    i += 2
                    continue
                if ch == quote:
                    quote = ""
                i += 1
                continue
            if ch in "\"'":
                quote = ch
                code.append(ch)
                i += 1
                continue
            if lang == "c" and line.startswith("//", i):
                comment.append(line[i + 2 :])
                break
            if lang == "c" and line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            if lang == "ps1" and line.startswith("<#", i):
                in_block = True
                i += 2
                continue
            if lang != "c" and ch == "#" and (i == 0 or line[i - 1] in " \t;({"):
                comment.append(line[i + 1 :])
                break
            code.append(ch)
            i += 1
        yield n, "".join(code), " ".join(comment)


def scan_forbidden_text(rel: str, text: str) -> list[Hit]:
    lang = language(rel, text)
    if lang is None:
        return []
    lines = text.split("\n")
    hits: list[Hit] = []
    for n, code, comment in split_comments(text, lang):
        seen: set[str] = set()
        for rule, pattern, where in FORBIDDEN_RULES:
            if rule in seen:
                continue
            haystack = comment if where == "comment" else code + " " + comment
            if pattern.search(haystack):
                seen.add(rule)
                hits.append(Hit(rule, rel, n, lines[n - 1]))
    catch = EMPTY_CATCH.get(lang)
    if catch:
        for m in catch.finditer(text):
            n = text.count("\n", 0, m.start()) + 1
            hits.append(Hit("empty-catch", rel, n, lines[n - 1]))
    return hits


def scan_forbidden(today: dt.date) -> Outcome:
    hits: list[Hit] = []
    files = repo_files(*FORBIDDEN_TOPS)
    for rel in files:
        text = read_text(rel)
        if text is not None:
            hits.extend(scan_forbidden_text(rel, text))
    fix = (
        "finish the code (or turn the note into an issue link), or add "
        "'rule | path | text | expires | reason' to tools/ci/forbidden-allowlist.txt"
    )
    outcome = apply_allowlist("forbidden", hits, FORBIDDEN_ALLOWLIST, today, fix)
    outcome.detail = f"{len(files)} files, " + outcome.detail
    return outcome


# --------------------------------------------------------------------------
# secrets

_NOT_A_USER = r"(?!(?:user|username|you|me|name|example|runner|USER|USERNAME|Public|Default|Shared)[\\/])"
SECRET_RULES: list[tuple[str, re.Pattern[str]]] = [
    ("private-key", re.compile(r"-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY(?: BLOCK)?-----")),
    ("aws-key", re.compile(r"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b")),
    ("github-token", re.compile(r"\bgh[pousr]_[A-Za-z0-9]{36,}\b|\bgithub_pat_[A-Za-z0-9_]{22,}\b")),
    ("huggingface-token", re.compile(r"\bhf_[A-Za-z0-9]{30,}\b")),
    ("openai-key", re.compile(r"\bsk-(?:proj-|ant-)?[A-Za-z0-9_-]{32,}\b")),
    ("slack-token", re.compile(r"\bxox[abposr]-[A-Za-z0-9-]{10,}\b")),
    ("google-api-key", re.compile(r"\bAIza[0-9A-Za-z_-]{35}\b")),
    ("stripe-key", re.compile(r"\b[rs]k_live_[0-9A-Za-z]{20,}\b")),
    ("jwt", re.compile(r"\beyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}")),
    (
        "assigned-secret",
        re.compile(
            r"(?i)\b(?:api[_-]?key|secret(?:[_-]?key)?|access[_-]?token|auth[_-]?token|client[_-]?secret|password|passwd)"
            r"\b[\"']?\s*[:=]\s*[\"'](?![<$%{])[A-Za-z0-9+/_=.-]{16,}[\"']"
        ),
    ),
    (
        "private-path",
        re.compile(
            r"(?<![\w.~$%-])(?:/home/" + _NOT_A_USER + r"[a-z_][a-z0-9_.-]*/"
            r"|/Users/" + _NOT_A_USER + r"[A-Za-z][\w.-]*/"
            r"|/root(?=/)/"
            r"|[A-Za-z]:\\+Users\\+" + _NOT_A_USER + r"[^\\\s\"'<>%]+\\)"
        ),
    ),
]


def scan_secrets_builtin(today: dt.date) -> Outcome:
    hits: list[Hit] = []
    files = repo_files()
    for rel in files:
        text = read_text(rel)
        if text is None:
            continue
        for n, line in enumerate(text.split("\n"), 1):
            for rule, pattern in SECRET_RULES:
                if pattern.search(line):
                    # Never print a secret: show where it is, not what it is.
                    hits.append(Hit(rule, rel, n, line, hide=rule != "private-path"))
    fix = (
        "remove the secret and revoke it (it is in git history if it was committed); "
        "replace machine paths with <placeholders>; or, for a false positive, add "
        "'rule | path | text | expires | reason' to tools/ci/secrets-allowlist.txt"
    )
    outcome = apply_allowlist("secrets (built-in)", hits, SECRETS_ALLOWLIST, today, fix)
    outcome.detail = f"{len(files)} files, " + outcome.detail
    return outcome


def scan_gitleaks() -> Outcome:
    exe = shutil.which("gitleaks")
    if not exe:
        return Outcome(
            "SKIP",
            "secrets (gitleaks)",
            "gitleaks is not installed, so git history is not scanned (the built-in scan covers the working tree)",
            fix="winget install --id Gitleaks.Gitleaks -e  (Linux: https://github.com/gitleaks/gitleaks/releases)",
            missing_tool=True,
        )
    cmd = [exe, "detect", "--source", str(ROOT), "--redact", "--no-banner", "--exit-code", "1"]
    if GITLEAKS_CONFIG.exists():
        cmd += ["--config", str(GITLEAKS_CONFIG)]
    if not (ROOT / ".git").exists():
        cmd.append("--no-git")
    done = run(cmd)
    tail = (done.stdout + done.stderr).strip().splitlines()[-30:]
    if done.returncode == 0:
        return Outcome("PASS", "secrets (gitleaks)", "no leaks in git history or the working tree")
    return Outcome(
        "FAIL",
        "secrets (gitleaks)",
        f"gitleaks exited {done.returncode}",
        tail,
        "remove and revoke the secret; for a false positive add an allowlist to tools/ci/gitleaks.toml",
    )


# --------------------------------------------------------------------------
# deps and bandit


def scan_deps() -> Outcome:
    tool = find_tool("pip_audit", "pip-audit")
    if tool is None:
        return Outcome(
            "SKIP",
            "deps",
            "pip-audit is not installed",
            fix="python -m pip install pip-audit",
            missing_tool=True,
        )
    rel = PY_REQUIREMENTS.relative_to(ROOT).as_posix()
    done = run([*tool, "-r", str(PY_REQUIREMENTS), "--progress-spinner", "off", "--format", "json", "--strict"])
    note = "note: torch and transformers (depthmaps.py) are pinned nowhere, so they are not audited"
    try:
        data = json.loads(done.stdout)
    except json.JSONDecodeError:
        return Outcome(
            "FAIL",
            "deps",
            f"pip-audit exited {done.returncode} without a report for {rel}",
            (done.stdout + done.stderr).strip().splitlines()[-20:],
            "check the network (pip-audit installs the requirements in a temporary environment "
            "and asks PyPI and OSV), then run it again",
        )
    lines: list[str] = []
    found = 0
    for dep in data.get("dependencies", []):
        vulns = dep.get("vulns") or []
        if vulns:
            found += len(vulns)
            fixed = sorted({v for vuln in vulns for v in vuln.get("fix_versions", [])})
            ids = ", ".join(vuln.get("id", "?") for vuln in vulns[:4]) + (", ..." if len(vulns) > 4 else "")
            lines.append(
                f"{dep.get('name')} {dep.get('version')}: {len(vulns)} advisories ({ids}); "
                f"fixed in {', '.join(fixed) or 'no release yet'}"
            )
        elif dep.get("skip_reason"):
            lines.append(f"{dep.get('name')}: not audited: {dep['skip_reason']}")
    if found == 0 and done.returncode == 0:
        return Outcome("PASS", "deps", f"no known vulnerabilities in {rel} and what it pulls in", [note])
    return Outcome(
        "FAIL",
        "deps",
        f"{found} known vulnerabilities in {rel} and what it pulls in",
        [*lines, note],
        "pin the fixed release in tools/hdpack/requirements*.txt (and pyproject.toml), "
        "run the pack tool's tests, then run this again",
    )


BANDIT_TARGETS = ["tools/hdpack", "tools/ci", "tools/gen_game_location_screen"]


def scan_bandit(today: dt.date) -> Outcome:
    tool = find_tool("bandit", "bandit")
    if tool is None:
        return Outcome(
            "SKIP",
            "bandit",
            "bandit is not installed",
            fix="python -m pip install bandit",
            missing_tool=True,
        )
    targets = [t for t in BANDIT_TARGETS if (ROOT / t).exists()]
    cmd = [*tool, "-q", "-f", "json", "-r", *targets, "-x", "tools/hdpack/tests"]
    cmd += ["--severity-level", "medium", "--confidence-level", "medium"]
    done = run(cmd)
    try:
        data = json.loads(done.stdout)
    except json.JSONDecodeError:
        return Outcome(
            "FAIL",
            "bandit",
            f"bandit exited {done.returncode} without a report",
            (done.stdout + done.stderr).strip().splitlines()[-20:],
            "run the command by hand to see why: " + " ".join(cmd[len(tool) :]),
        )
    problems = [f"{e.get('filename')}: {e.get('reason')}" for e in data.get("errors", [])]
    hits: list[Hit] = []
    for r in data.get("results", []):
        path = Path(r["filename"])
        rel = (path.relative_to(ROOT) if path.is_absolute() else path).as_posix()
        n = int(r["line_number"])
        text = read_text(rel) or ""
        source = text.split("\n")
        line = source[n - 1] if 0 < n <= len(source) else ""
        hits.append(Hit(r["test_id"], rel, n, f"{line.strip()}  <- {r['issue_text']}"))
    fix = (
        "fix the finding, or, once reviewed, add 'test id | path | text | expires | reason' "
        "to tools/ci/bandit-allowlist.txt"
    )
    outcome = apply_allowlist("bandit", hits, BANDIT_ALLOWLIST, today, fix)
    if problems:
        outcome.status = "FAIL"
        outcome.lines = problems + outcome.lines
        outcome.detail = f"bandit could not read {len(problems)} file(s); " + outcome.detail
    outcome.detail = f"medium and high findings in {', '.join(targets)}: " + outcome.detail
    return outcome


# --------------------------------------------------------------------------


SCANS = ("forbidden", "secrets", "deps", "bandit")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("scans", nargs="*", metavar="scan", help=f"{', '.join(SCANS)} (default: all)")
    p.add_argument("--require-tools", action="store_true", help="a scan skipped for a missing tool fails")
    p.add_argument("--today", type=dt.date.fromisoformat, default=dt.date.today(), help=argparse.SUPPRESS)
    args = p.parse_args(argv)
    unknown = [s for s in args.scans if s not in SCANS]
    if unknown:
        p.error(f"unknown scan(s) {', '.join(unknown)}; choose from {', '.join(SCANS)}")
    strict = args.require_tools or os.environ.get("PLEGACY_REQUIRE_TOOLS") == "1"
    outcomes: list[Outcome] = []
    for name in args.scans or SCANS:
        if name == "forbidden":
            outcomes.append(scan_forbidden(args.today))
        elif name == "secrets":
            outcomes.append(scan_gitleaks())
            outcomes.append(scan_secrets_builtin(args.today))
        elif name == "deps":
            outcomes.append(scan_deps())
        elif name == "bandit":
            outcomes.append(scan_bandit(args.today))
    for o in outcomes:
        if strict and o.status == "SKIP" and o.missing_tool:
            o.status = "FAIL"
            o.detail += " (--require-tools)"
        report(o)
    skipped = [o for o in outcomes if o.status == "SKIP"]
    failed = [o for o in outcomes if o.status == "FAIL"]
    if skipped:
        print(f"     skipped: {', '.join(o.name for o in skipped)}")
    if failed:
        print(f"FAIL scan: {', '.join(o.name for o in failed)}")
        return 1
    print(f"PASS scan: {len(outcomes) - len(skipped)} passed, {len(skipped)} skipped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
