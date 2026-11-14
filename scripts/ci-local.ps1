# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
  Local CI on your own PC (no hosted runners), the same steps as
  scripts/ci-local.sh: doctor, dependencies, build with zero warnings, lint,
  scans, tests and smoke run, HD gates, then a snapshot package. Stops at
  the first failure. Ends with the list of checks that were skipped.
  Every process runs in this console or hidden: no window opens.
.PARAMETER GamePath
  Folder with PT.EXE (default: $env:PLEGACY_TEST_GAME_PATH). With it, the
  game files are expected: a check that needs them and is skipped fails.
.PARAMETER RequireGame
  Expect the game files even without -GamePath (PLEGACY_REQUIRE_GAME=1):
  then the run fails, rather than skipping the game checks.
.PARAMETER RequireTools
  gitleaks, pip-audit, bandit and shellcheck must be installed
  (PLEGACY_REQUIRE_TOOLS=1).
#>
param(
    [string]$GamePath = $env:PLEGACY_TEST_GAME_PATH,
    [switch]$RequireGame,
    [switch]$RequireTools
)
. "$PSScriptRoot\common.ps1"
if ($GamePath) { $env:PLEGACY_TEST_GAME_PATH = $GamePath }
if ($RequireGame) { $env:PLEGACY_REQUIRE_GAME = '1' }
if ($RequireTools) { $env:PLEGACY_REQUIRE_TOOLS = '1' }

$skipLog = Join-Path $script:Root 'build\ci-local-skipped.txt'
New-Item -ItemType Directory -Force -Path (Split-Path $skipLog) | Out-Null
Set-Content -Path $skipLog -Value '' -Encoding utf8
$env:PLEGACY_SKIP_LOG = $skipLog

function Write-SkipSummary {
    $skipped = @(Get-Content -Path $skipLog -Encoding utf8 | Where-Object { $_.Trim() })
    if ($skipped.Count -eq 0) { Write-Note 'skipped: nothing'; return }
    Write-Host "Skipped ($($skipped.Count)):" -ForegroundColor Yellow
    $skipped | ForEach-Object { Write-Note $_ }
}

try {
    Write-Host '== doctor' -ForegroundColor Cyan
    & "$PSScriptRoot\doctor.ps1"
    if ($LASTEXITCODE -ne 0) { throw "doctor failed (exit code $LASTEXITCODE): fix the REQUIRED checks above" }
    Write-Host '== dependencies' -ForegroundColor Cyan
    & "$PSScriptRoot\fetch-deps.ps1"
    Write-Host '== build' -ForegroundColor Cyan
    & "$PSScriptRoot\build.ps1"
    Write-Host '== lint' -ForegroundColor Cyan
    & "$PSScriptRoot\lint.ps1"
    Write-Host '== scans' -ForegroundColor Cyan
    & "$PSScriptRoot\scan.ps1"
    Write-Host '== tests' -ForegroundColor Cyan
    & "$PSScriptRoot\test.ps1"
    Write-Host '== HD gates' -ForegroundColor Cyan
    & "$PSScriptRoot\hd-gates.ps1"
    Write-Host '== package (snapshot)' -ForegroundColor Cyan
    & "$PSScriptRoot\release.ps1" -Snapshot
} catch {
    Write-SkipSummary
    Write-Bad "local CI: $($_.Exception.Message)"
    throw
}
Write-SkipSummary
Write-Ok 'local CI passed'
