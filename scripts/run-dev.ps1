<#
.SYNOPSIS
  Plays the game in HD with a pack.
#>
param(
    [ValidateRange(1, 8)][int]$HdScale = 3,
    [string]$Pack = (Join-Path (Resolve-Path "$PSScriptRoot\..") 'pack'),
    [string]$GamePath = ''
)
. "$PSScriptRoot\common.ps1"
$toolArgs = @('--hd-scale', "$HdScale")
if (Test-Path $Pack) { $toolArgs += @('--hd-pack', $Pack) } else { Write-Note "no pack at $Pack; drawing the original art at ${HdScale}x" }
if ($GamePath) { $toolArgs += @('--game-path', $GamePath) }
Invoke-Game $toolArgs
