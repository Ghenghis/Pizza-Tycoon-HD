#!/bin/sh
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Static checks, each required (the twin of lint.ps1):
#   the fork's sh scripts (scripts/*.sh): ShellCheck
#   ruff, black --check and mypy (strict) on every Python tool:
#     tools/hdpack/*.py, tools/ci/*.py, tools/gen_game_location_screen
#   PSScriptAnalyzer on scripts/*.ps1, when pwsh is installed (lint.ps1 on
#     Windows always runs it)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
cd "$ROOT"

echo "== sh scripts (shellcheck)"
command -v shellcheck >/dev/null 2>&1 ||
  die "shellcheck is not installed" "sudo apt-get install shellcheck (or: $PYTHON -m pip install shellcheck-py)"
shellcheck -x scripts/*.sh
ok "shellcheck: $(echo scripts/*.sh | wc -w) scripts"

echo "== Python tools (ruff, black, mypy --strict)"
(cd tools/hdpack && run_pytool ruff check -q . && run_pytool black --check -q . && run_pytool mypy ./*.py)
ok "tools/hdpack"
(cd tools/ci && run_pytool ruff check -q . && run_pytool black --check -q . && run_pytool mypy ./*.py)
ok "tools/ci"
CFG=tools/ci/pyproject.toml
GEN=tools/gen_game_location_screen
run_pytool ruff check -q --config "$CFG" "$GEN"
run_pytool black --check -q --config "$CFG" "$GEN"
run_pytool mypy --config-file "$CFG" "$GEN"
ok "$GEN"

echo "== PowerShell scripts (PSScriptAnalyzer)"
if command -v pwsh >/dev/null 2>&1; then
  pwsh -NoProfile -NonInteractive -File "$ROOT/scripts/lint.ps1" -PowerShellOnly
else
  skip_check "PSScriptAnalyzer" "pwsh is not installed (scripts\\lint.ps1 checks them on Windows)" \
    "https://learn.microsoft.com/powershell/scripting/install/installing-powershell-on-linux" tool
fi
ok "lint"
