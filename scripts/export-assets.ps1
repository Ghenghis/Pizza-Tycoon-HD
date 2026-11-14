# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
  Writes every original picture and sprite frame to export\ (PNG + manifest.json).
#>
param([Parameter(Mandatory)][string]$GamePath, [string]$Out = (Join-Path (Resolve-Path "$PSScriptRoot\..") 'export'))
. "$PSScriptRoot\common.ps1"
$env:SDL_VIDEO_DRIVER = 'offscreen'   # no window
$env:SDL_AUDIO_DRIVER = 'dummy'
Invoke-Game @('--game-path', $GamePath, '--export-assets', $Out)
Write-Ok "export: $Out"
