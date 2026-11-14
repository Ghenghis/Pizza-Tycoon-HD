# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
  End-to-end HD gates on this PC's real GPU, hidden (tools\ci\hd_gates.py):
  the HD screens render, GPU and CPU compositing draw the same picture,
  smooth motion leaves the game state byte-for-byte unchanged, and a fully
  redrawn 4K restaurant frame stays under 8 ms (99th percentile) and opens
  in under 16 ms. Screenshots and game logs of a failed run are kept under
  build\hd-gates.
  Without game files the gates are skipped (listed); with
  PLEGACY_REQUIRE_GAME=1 that is a failure. With game files every gate must
  run: none may be skipped.
.PARAMETER GamePath
  Folder with PT.EXE (default: $env:PLEGACY_TEST_GAME_PATH).
.PARAMETER Pack
  A 4K HD pack (default: $env:PLEGACY_TEST_HD_PACK). Without one, a
  baseline pack is built from the game's art first.
#>
param(
    [string]$GamePath = $env:PLEGACY_TEST_GAME_PATH,
    [string]$Pack = $env:PLEGACY_TEST_HD_PACK
)
. "$PSScriptRoot\common.ps1"
if (-not $GamePath) {
    Skip-Check -Name 'HD gates' -Reason 'no game folder' -Fix 'set PLEGACY_TEST_GAME_PATH to the folder with PT.EXE' -Kind game
    return
}
if (-not (Test-Path (Join-Path $GamePath 'PT.EXE'))) { throw "No PT.EXE in $GamePath (PLEGACY_TEST_GAME_PATH)." }
$py = Get-Python
$gates = Join-Path $script:Root 'tools\ci\hd_gates.py'
$gateArgs = @($gates, '--game', (Get-GameExe), '--game-path', $GamePath, '--fail-on-skip',
    '--artifacts', (Join-Path $script:Root 'build\hd-gates'))
if ($Pack) { $gateArgs += @('--pack', $Pack) } else { $gateArgs += '--make-pack' }
Invoke-Checked -File $py -Arguments $gateArgs
Write-Ok 'HD gates'
