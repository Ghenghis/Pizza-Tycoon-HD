# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
  Plays the game in HD with a pack. Builds it first when there is no build.
#>
param(
    [ValidateRange(1, 8)][int]$HdScale = 3,
    [string]$Pack = (Join-Path (Resolve-Path "$PSScriptRoot\..") 'pack'),
    [string]$GamePath = ''
)
. "$PSScriptRoot\common.ps1"
if (-not (Test-Path (Get-GameExe))) { & "$PSScriptRoot\build.ps1" }
$toolArgs = @('--hd-scale', "$HdScale")
if (Test-Path $Pack) { $toolArgs += @('--hd-pack', $Pack) } else { Write-Note "no pack at $Pack; drawing the original art at ${HdScale}x" }
if ($GamePath) { $toolArgs += @('--game-path', $GamePath) }
Invoke-Game $toolArgs
