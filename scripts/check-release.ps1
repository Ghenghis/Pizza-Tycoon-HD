# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
  The release gate (tools\ci\check_release.py), run by scripts\release.ps1
  on every zip it makes, and usable alone on an existing release: checks
  every SHA-256, unpacks the zip into a temporary folder (always removed),
  checks its files against tools\ci\release-manifest.txt (game, data\,
  LICENSE and notices there; no original game files), then runs the
  unpacked game hidden: --version, and a short --exec run (quick start,
  restaurant, 5 game seconds, screenshot) when game files are available.
  Without game files that run is skipped and listed; with PLEGACY_TEST_GAME_PATH
  or PLEGACY_REQUIRE_GAME=1 it is required.
.PARAMETER Zip
  The release zip.
.PARAMETER Exe
  The bare exe released next to it (optional): must be the zip's exe.
.PARAMETER Version
  The version the game must report (optional).
.PARAMETER Require
  More files the zip must hold, e.g. SDL3.dll for the MSVC build.
.PARAMETER GamePath
  Folder with PT.EXE (default: $env:PLEGACY_TEST_GAME_PATH).
.EXAMPLE
  .\scripts\check-release.ps1 -Zip release\PizzaTycoonHD_v1.1.0_win64.zip -Exe release\PizzaTycoonHD_v1.1.0_win64.exe -Version 1.1.0
#>
param(
    [Parameter(Mandatory)][string]$Zip,
    [string]$Exe = '',
    [string]$Version = '',
    [string[]]$Require = @(),
    [string]$GamePath = $env:PLEGACY_TEST_GAME_PATH
)
. "$PSScriptRoot\common.ps1"
$py = Get-Python
# This is Windows: the unpacked game must start.
$checkArgs = @((Join-Path $script:Root 'tools\ci\check_release.py'), $Zip, '--require-run')
if ($Exe) { $checkArgs += @('--exe', $Exe) }
if ($Version) { $checkArgs += @('--version-expected', $Version) }
foreach ($r in $Require) { $checkArgs += @('--require', $r) }
if ($GamePath) { $checkArgs += @('--game-path', $GamePath) }
if (Test-GameRequired) { $checkArgs += '--require-game' }
Invoke-Checked -File $py -Arguments $checkArgs
