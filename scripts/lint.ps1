# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
  Static checks, each required: PSScriptAnalyzer on scripts\*.ps1 (rules in
  scripts\PSScriptAnalyzerSettings.psd1); ruff, black --check and mypy
  (strict) on every Python tool (tools\hdpack\*.py, tools\ci\*.py,
  tools\gen_game_location_screen); shellcheck on scripts\*.sh when it is
  installed (scripts/lint.sh, and PLEGACY_REQUIRE_TOOLS=1, require it).
.PARAMETER PowerShellOnly
  Only PSScriptAnalyzer (scripts/lint.sh uses this through pwsh).
#>
param([switch]$PowerShellOnly)
. "$PSScriptRoot\common.ps1"

Write-Host '== PowerShell scripts (PSScriptAnalyzer)'
if (-not (Get-Module -ListAvailable PSScriptAnalyzer)) {
    Write-Bad 'PSScriptAnalyzer is not installed'
    Write-Note 'fix: Install-Module PSScriptAnalyzer -Scope CurrentUser'
    throw 'PSScriptAnalyzer is not installed'
}
$settings = Join-Path $PSScriptRoot 'PSScriptAnalyzerSettings.psd1'
$scripts = @(Get-ChildItem -Path $PSScriptRoot -Filter '*.ps1')
$issues = @($scripts | ForEach-Object { Invoke-ScriptAnalyzer -Path $_.FullName -Settings $settings })
if ($issues.Count -gt 0) {
    $issues | Format-Table -AutoSize -Wrap RuleName, Severity, ScriptName, Line, Message | Out-Host
    Write-Bad "PSScriptAnalyzer: $($issues.Count) problem(s)"
    throw 'PSScriptAnalyzer found problems.'
}
Write-Ok "PSScriptAnalyzer: $($scripts.Count) scripts"
if ($PowerShellOnly) { return }

Write-Host '== Python tools (ruff, black, mypy --strict)'
foreach ($dir in 'tools\hdpack', 'tools\ci') {
    Push-Location (Join-Path $script:Root $dir)
    try {
        $modules = @(Get-ChildItem -Filter '*.py' | ForEach-Object Name)
        Invoke-PyTool ruff @('check', '-q', '.')
        Invoke-PyTool black @('--check', '-q', '.')
        Invoke-PyTool mypy $modules
    } finally { Pop-Location }
    Write-Ok "$dir ($($modules.Count) modules)"
}
# The extensionless script, with the CI tools' settings.
$cfg = Join-Path $script:Root 'tools\ci\pyproject.toml'
$gen = Join-Path $script:Root 'tools\gen_game_location_screen'
Invoke-PyTool ruff @('check', '-q', '--config', $cfg, $gen)
Invoke-PyTool black @('--check', '-q', '--config', $cfg, $gen)
Invoke-PyTool mypy @('--config-file', $cfg, $gen)
Write-Ok 'tools\gen_game_location_screen'

Write-Host '== sh scripts (shellcheck)'
if (Get-Command shellcheck -ErrorAction SilentlyContinue) {
    Push-Location $script:Root
    try { Invoke-Checked shellcheck (@('-x') + @(Get-ChildItem 'scripts\*.sh' | ForEach-Object { "scripts/$($_.Name)" })) }
    finally { Pop-Location }
    Write-Ok 'shellcheck'
} else {
    Skip-Check -Name 'shellcheck' -Reason 'not installed (scripts/lint.sh requires it)' -Fix 'winget install --id koalaman.shellcheck -e' -Kind tool
}
Write-Ok 'lint'
