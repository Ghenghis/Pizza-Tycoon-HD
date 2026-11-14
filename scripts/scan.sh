#!/bin/sh
# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Repository scans (tools/ci/scan.py; the twin of scan.ps1): forbidden
# patterns in the fork's code, secrets and private paths, known
# vulnerabilities in the Python requirements (pip-audit), and bandit on the
# Python tools. gitleaks, pip-audit and bandit are used when installed and
# skipped (listed) when not; PLEGACY_REQUIRE_TOOLS=1 makes a missing one fail.
#
#   scripts/scan.sh [forbidden] [secrets] [deps] [bandit]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/scripts/common.sh"
if tools_required; then set -- "$@" --require-tools; fi
"$PYTHON" "$ROOT/tools/ci/scan.py" "$@"
