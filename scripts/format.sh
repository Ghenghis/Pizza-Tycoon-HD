#!/bin/sh
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Formats the fork's C++ files (clang-format) and the Python tools (black,
# ruff --fix); the twin of format.ps1.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
cd "$ROOT"
BASE=upstream-pizzalegacy-1.0.0-beta.2
git rev-parse -q --verify "$BASE" >/dev/null 2>&1 || BASE=$(git rev-list --max-parents=0 HEAD 2>/dev/null | tail -1)
if [ -n "$BASE" ]; then
  (git diff --name-only "$BASE" -- src tests; git ls-files --others --exclude-standard -- src tests) |
    grep -E '\.(cpp|h|inl)$' | sort -u | while read -r f; do if [ -f "$f" ]; then echo "$f"; fi; done |
    xargs -r clang-format -i
fi
for d in tools/hdpack tools/ci; do
  (cd "$d" && run_pytool black -q . && run_pytool ruff check --fix -q .)
done
run_pytool black -q --config tools/ci/pyproject.toml tools/gen_game_location_screen
run_pytool ruff check --fix -q --config tools/ci/pyproject.toml tools/gen_game_location_screen
ok "formatted"
