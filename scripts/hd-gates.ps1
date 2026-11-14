<#
.SYNOPSIS
  End-to-end HD gates on this PC's real GPU, hidden: GPU and CPU compositing
  draw the same picture, smooth motion leaves the game state byte-for-byte
  unchanged, and a fully redrawn 4K restaurant frame stays under 8 ms (99th
  percentile) and opens in under 16 ms.
.PARAMETER GamePath
  Folder with PT.EXE (default: $env:PLEGACY_TEST_GAME_PATH).
.PARAMETER Pack
  A 4K HD pack (default: $env:PLEGACY_TEST_HD_PACK). Without one the speed
  gate is skipped.
#>
param(
    [string]$GamePath = $env:PLEGACY_TEST_GAME_PATH,
    [string]$Pack = $env:PLEGACY_TEST_HD_PACK
)
. "$PSScriptRoot\common.ps1"
if (-not $GamePath) { Write-Note 'HD gates skipped: set PLEGACY_TEST_GAME_PATH'; return }
$py = Get-Python
$gates = Join-Path $script:Root 'tools\ci\hd_gates.py'
$gateArgs = @($gates, '--game', (Get-GameExe), '--game-path', $GamePath)
if ($Pack) { $gateArgs += @('--pack', $Pack) }
Invoke-Checked -File $py -Arguments $gateArgs
Write-Ok 'HD gates'
