# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
  Repository scans (tools\ci\scan.py): markers of unfinished work in the
  fork's code (the rules are in scan.py), secrets and private paths, known vulnerabilities in the Python requirements
  (pip-audit), and bandit on the Python tools. gitleaks, pip-audit and
  bandit are used when installed and skipped (listed) when not; with
  -RequireTools or PLEGACY_REQUIRE_TOOLS=1 a missing one fails.
.PARAMETER Only
  Some of: forbidden, secrets, deps, bandit (default: all).
.EXAMPLE
  .\scripts\scan.ps1 -Only forbidden, secrets
#>
param([string[]]$Only = @(), [switch]$RequireTools)
. "$PSScriptRoot\common.ps1"
if ($RequireTools) { $env:PLEGACY_REQUIRE_TOOLS = '1' }
$py = Get-Python
$scanArgs = @((Join-Path $script:Root 'tools\ci\scan.py')) + $Only
if (Test-ToolsRequired) { $scanArgs += '--require-tools' }
Invoke-Checked -File $py -Arguments $scanArgs
