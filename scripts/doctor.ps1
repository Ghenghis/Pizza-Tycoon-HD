# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
  Checks what Pizza Tycoon HD needs and prints the fix for anything missing.
  REQUIRED checks fail the run (exit code 1). OPTIONAL ones are reported
  only, so a clean clone passes before anything is built.
.PARAMETER GamePath
  Folder with the original PT.EXE (default: $env:PLEGACY_TEST_GAME_PATH).
  When one is given, it is REQUIRED.
.PARAMETER RequireGame
  Game files are expected (also PLEGACY_REQUIRE_GAME=1): fail without them.
.PARAMETER RequireTools
  Optional tools (gitleaks, pip-audit, bandit, shellcheck) become REQUIRED
  (also PLEGACY_REQUIRE_TOOLS=1).
.PARAMETER ComfyServer
  ComfyUI address to check (REQUIRED when given), e.g. http://127.0.0.1:8188
#>
param(
    [string]$GamePath = $env:PLEGACY_TEST_GAME_PATH,
    [switch]$RequireGame,
    [switch]$RequireTools,
    [string]$ComfyServer = ''
)
. "$PSScriptRoot\common.ps1"
if ($RequireGame) { $env:PLEGACY_REQUIRE_GAME = '1' }
if ($RequireTools) { $env:PLEGACY_REQUIRE_TOOLS = '1' }
$script:failed = @()
$script:missing = @()

# One check. $Test returns a description when the thing is there, else
# nothing (or throws).
function Test-Need {
    param([string]$Name, [scriptblock]$Test, [string]$Fix, [switch]$Optional)
    try { $r = & $Test } catch { $r = $null }
    $class = if ($Optional) { 'optional' } else { 'required' }
    if ($r) { Write-Ok "$class  $Name  ($r)"; return }
    if ($Optional) {
        Write-Skip "$class  ${Name}: not found"
        $script:missing += $Name
    } else {
        Write-Bad "$class  ${Name}: not found"
        $script:failed += $Name
    }
    Write-Note "fix: $Fix"
}
function Get-ToolVersion([string]$Module, [string]$Exe) {
    $tool = Get-PyTool -Module $Module -Exe $Exe
    if (-not $tool) { return $null }
    $ErrorActionPreference = 'Continue'  # a version banner on stderr is not a failure
    $out = @(& $tool[0] @(@($tool | Select-Object -Skip 1) + '--version') 2>&1)
    if ($LASTEXITCODE -eq 0 -and $out) { "$($out[0])".Trim() }
}
$toolsOptional = -not (Test-ToolsRequired)

Write-Host '== build tools'
Test-Need 'git' { (git --version) } 'winget install --id Git.Git -e'
Test-Need 'cmake >= 3.25' {
    $v = (cmake --version | Select-Object -First 1)
    if ($v -match '(\d+)\.(\d+)' -and ([int]$Matches[1] -gt 3 -or [int]$Matches[2] -ge 25)) { $v }
} 'winget install --id Kitware.CMake -e'
Test-Need 'ninja' { (ninja --version) } 'winget install --id Ninja-build.Ninja -e'
Test-Need 'Visual Studio 2022 C++' {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property displayName }
} 'winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive"'
Test-Need 'clang-format' { (clang-format --version) } 'Included with Visual Studio C++ (Clang tools), or: winget install --id LLVM.LLVM -e'

Write-Host '== Python and the lint tools'
Test-Need 'python 3.10+' {
    $py = Get-Python; $v = (& $py --version 2>&1)
    if ("$v" -match '3\.(\d+)' -and [int]$Matches[1] -ge 10) { "$v" }
} 'winget install --id Python.Python.3.12 -e'
Test-Need 'Pillow and numpy' {
    $py = Get-Python
    & $py -c "import PIL, numpy; print('Pillow', PIL.__version__, 'numpy', numpy.__version__)"
} 'python -m pip install -r tools\hdpack\requirements-dev.txt'
foreach ($t in 'ruff', 'black', 'mypy', 'pytest') {
    Test-Need $t { Get-ToolVersion $t $t } 'python -m pip install -r tools\hdpack\requirements-dev.txt'
}
Test-Need 'PSScriptAnalyzer' {
    $m = Get-Module -ListAvailable PSScriptAnalyzer | Sort-Object Version -Descending | Select-Object -First 1
    if ($m) { "$($m.Version)" }
} 'Install-Module PSScriptAnalyzer -Scope CurrentUser'

Write-Host '== scanners (scripts\scan.ps1 skips what is missing)'
Test-Need 'gitleaks' { (gitleaks version) } 'winget install --id Gitleaks.Gitleaks -e' -Optional:$toolsOptional
Test-Need 'pip-audit' { Get-ToolVersion 'pip_audit' 'pip-audit' } 'python -m pip install pip-audit' -Optional:$toolsOptional
Test-Need 'bandit' { Get-ToolVersion 'bandit' 'bandit' } 'python -m pip install bandit' -Optional:$toolsOptional
Test-Need 'shellcheck (sh scripts; scripts/lint.sh needs it)' { (shellcheck --version | Select-String 'version:') } 'winget install --id koalaman.shellcheck -e' -Optional:$toolsOptional

Write-Host '== build state (made by the scripts; not needed before the first build)'
Test-Need 'dependencies built' { if (Test-Path (Join-Path (Get-DepsPrefix) 'include\SDL3\SDL.h')) { Get-DepsPrefix } } 'scripts\fetch-deps.ps1 (scripts\build.ps1 runs it)' -Optional
Test-Need 'game built' { if (Test-Path (Get-GameExe)) { Get-GameExe } } 'scripts\build.ps1' -Optional

Write-Host '== game files and ComfyUI'
$gameFix = 'Point -GamePath (or PLEGACY_TEST_GAME_PATH) at the folder that contains PT.EXE (GOG/Steam: the Pizza Connection folder).'
if ($GamePath -or (Test-GameRequired)) {
    Test-Need 'original game' { if ($GamePath -and (Test-Path (Join-Path $GamePath 'PT.EXE'))) { $GamePath } } $gameFix
} else {
    Test-Need 'original game (for the HD gates and the tests that use the original art)' { $null } $gameFix -Optional
}
if ($ComfyServer) {
    Test-Need "ComfyUI at $ComfyServer" {
        $s = Invoke-RestMethod -Uri "$ComfyServer/system_stats" -TimeoutSec 5
        ($s.devices | ForEach-Object { "$($_.name) $([math]::Round($_.vram_total / 1GB, 1)) GiB" }) -join ', '
    } 'Start ComfyUI (run_nvidia_gpu.bat), then run this again.'
}

Write-Host ''
if ($script:missing) { Write-Note "optional, not found: $($script:missing -join ', ')" }
if ($script:failed) {
    Write-Bad "doctor: $($script:failed.Count) required check(s) failed: $($script:failed -join ', ')"
    exit 1
}
Write-Ok "doctor: every required check passed ($($script:missing.Count) optional missing)"
exit 0
