# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
  Formats the fork's C++ files (clang-format) and the Python tools
  (black, ruff --fix): tools\hdpack, tools\ci, tools\gen_game_location_screen.
#>
. "$PSScriptRoot\common.ps1"
$files = @(Get-ChangedCppFile)
if ($files.Count -gt 0) { Invoke-Checked clang-format (@('-i') + $files) }
foreach ($dir in 'tools\hdpack', 'tools\ci') {
    Push-Location (Join-Path $script:Root $dir)
    try { Invoke-PyTool black @('-q', '.'); Invoke-PyTool ruff @('check', '--fix', '-q', '.') } finally { Pop-Location }
}
$cfg = Join-Path $script:Root 'tools\ci\pyproject.toml'
$gen = Join-Path $script:Root 'tools\gen_game_location_screen'
Invoke-PyTool black @('-q', '--config', $cfg, $gen)
Invoke-PyTool ruff @('check', '--fix', '-q', '--config', $cfg, $gen)
Write-Ok 'formatted'
