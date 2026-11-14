# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
  The checks that need a build but no GPU, with no window: clang-format on
  the fork's C++ files, a smoke run (--version, --help), the C++ tests and
  the pack tool's tests. Python lint is scripts\lint.ps1.
  Tests that need the original game skip without it, and each skip is
  listed; when the game is expected (-GamePath, PLEGACY_TEST_GAME_PATH or
  PLEGACY_REQUIRE_GAME=1) a skipped test fails the run.
.PARAMETER GamePath
  Folder with PT.EXE; enables the tests that use the original art.
#>
param([string]$GamePath = $env:PLEGACY_TEST_GAME_PATH)
. "$PSScriptRoot\common.ps1"
$env:SDL_VIDEO_DRIVER = 'offscreen'
$env:SDL_AUDIO_DRIVER = 'dummy'
if ($GamePath) { $env:PLEGACY_TEST_GAME_PATH = $GamePath }
$gameFix = 'set PLEGACY_TEST_GAME_PATH (or -GamePath) to the folder with PT.EXE'

Write-Host '== format (clang-format, files this fork adds or changes)'
$files = @(Get-ChangedCppFile)
if ($files.Count -gt 0) {
    Invoke-Checked clang-format (@('--dry-run', '--Werror') + $files)
    Write-Ok "clang-format: $($files.Count) files"
} else {
    Skip-Check -Name 'clang-format' -Reason 'not a git checkout, so the fork''s files are unknown'
}

Write-Host '== smoke run (the game starts and exits cleanly; no game files needed)'
Invoke-Game @('--version')
Invoke-Game @('--help')
Write-Ok 'smoke: --version and --help exit with 0'

Write-Host '== C++ tests'
$tests = Get-TestExe
if (-not (Test-Path $tests)) { throw "No test build at $tests. Run scripts\build.ps1 (it builds the tests too)." }
$run = Invoke-Captured -File $tests
if ($run.ExitCode -ne 0) { throw "C++ tests failed (exit code $($run.ExitCode))" }
$skipped = $run.Lines | Select-String -CaseSensitive -Pattern '(\d+) skipped' | Select-Object -Last 1
if ($skipped) {
    Skip-Check -Name 'C++ tests' -Reason "$($skipped.Matches[0].Groups[1].Value) test case(s) skipped (they need the original game)" -Fix $gameFix -Kind game
}
Write-Ok 'C++ tests'

Write-Host '== pack tool tests (pytest)'
$tmp = $null
Push-Location (Join-Path $script:Root 'tools\hdpack')
try {
    if ($GamePath) {
        $tmp = Join-Path ([IO.Path]::GetTempPath()) ('plegacy-' + [guid]::NewGuid())
        Invoke-Game @('--config', "$tmp\settings", '--game-path', $GamePath, '--export-assets', "$tmp\export")
        $env:PLEGACY_TEST_EXPORT_DIR = "$tmp\export"
    }
    $pytest = Get-PyTool -Module pytest
    if (-not $pytest) { throw 'pytest is not installed. Fix: python -m pip install -r tools\hdpack\requirements-dev.txt' }
    $run = Invoke-Captured -File $pytest[0] -Arguments (@($pytest | Select-Object -Skip 1) + @('-q', '-rs'))
    if ($run.ExitCode -ne 0) { throw "pack tool tests failed (exit code $($run.ExitCode))" }
    $skipped = $run.Lines | Select-String -CaseSensitive -Pattern '(\d+) skipped' | Select-Object -Last 1
    if ($skipped) {
        Skip-Check -Name 'pack tool tests' -Reason "$($skipped.Matches[0].Groups[1].Value) test(s) skipped (they need the original game)" -Fix $gameFix -Kind game
    }
} finally {
    Pop-Location
    if ($tmp -and (Test-Path $tmp)) { Remove-Item -Recurse -Force $tmp }
}
Write-Ok 'tests'
