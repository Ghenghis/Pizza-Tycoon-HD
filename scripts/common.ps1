# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shared helpers for the Windows scripts. Dot-source: . "$PSScriptRoot\common.ps1"
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Write-Ok([string]$Text)   { Write-Host "PASS $Text" -ForegroundColor Green }
function Write-Bad([string]$Text)  { Write-Host "FAIL $Text" -ForegroundColor Red }
function Write-Skip([string]$Text) { Write-Host "SKIP $Text" -ForegroundColor Yellow }
function Write-Note([string]$Text) { Write-Host "     $Text" -ForegroundColor DarkGray }

# The original game's files are expected when PLEGACY_REQUIRE_GAME=1 or
# PLEGACY_TEST_GAME_PATH is set: a check that needs them then fails instead
# of skipping. PLEGACY_REQUIRE_TOOLS=1 does the same for optional tools.
function Test-GameRequired { return ($env:PLEGACY_REQUIRE_GAME -eq '1') -or [bool]$env:PLEGACY_TEST_GAME_PATH }
function Test-ToolsRequired { return $env:PLEGACY_REQUIRE_TOOLS -eq '1' }

# Reports a check that cannot run. It fails instead when it needs the game
# (Kind game) or an optional tool (Kind tool) and that is required. Skips are
# listed in $env:PLEGACY_SKIP_LOG, which ci-local summarises at the end.
function Skip-Check {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Reason,
        [string]$Fix = '',
        [ValidateSet('game', 'tool', 'other')][string]$Kind = 'other'
    )
    $required = ($Kind -eq 'game' -and (Test-GameRequired)) -or ($Kind -eq 'tool' -and (Test-ToolsRequired))
    if ($required) {
        $why = 'optional tools are required (PLEGACY_REQUIRE_TOOLS=1)'
        if ($Kind -eq 'game') { $why = 'game files are expected (PLEGACY_TEST_GAME_PATH or PLEGACY_REQUIRE_GAME=1)' }
        Write-Bad "${Name}: $Reason, and $why"
        if ($Fix) { Write-Note "fix: $Fix" }
        throw "${Name}: $Reason"
    }
    Write-Skip "${Name}: $Reason"
    if ($Fix) { Write-Note "fix: $Fix" }
    if ($env:PLEGACY_SKIP_LOG) { Add-Content -Path $env:PLEGACY_SKIP_LOG -Value "${Name}: $Reason" -Encoding utf8 }
}

# Runs a program in this console (never a new window) and stops on failure.
function Invoke-Checked {
    param([Parameter(Mandatory)][string]$File, [string[]]$Arguments = @())
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$File $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

# Runs a program in this console, shows its output as it comes and returns
# it (stdout and stderr, as text) with the exit code. Text on stderr does not
# stop the script; the caller decides from the exit code.
function Invoke-Captured {
    param([Parameter(Mandatory)][string]$File, [string[]]$Arguments = @())
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $captured = @()
    try {
        & $File @Arguments 2>&1 | ForEach-Object { "$_" } | Tee-Object -Variable captured | Out-Host
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $saved }
    return [pscustomobject]@{ ExitCode = $code; Lines = @($captured) }
}

# Runs a program in this console with all its output in a log file, and
# returns the exit code. Text on stderr goes to the log, never stops the
# script (as it would in Windows PowerShell 5.1 with redirection).
function Invoke-Logged {
    param([Parameter(Mandatory)][string]$File, [string[]]$Arguments = @(), [Parameter(Mandatory)][string]$Log)
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $File @Arguments *> $Log
        return $LASTEXITCODE
    } finally { $ErrorActionPreference = $saved }
}

# The game is a GUI program (no console window). Start-Process -Wait is the
# way to wait for one and read its exit code; -NoNewWindow keeps its output
# in this console. Scripted runs (--exec, --export-assets, --version) open
# no game window either.
function Invoke-Game {
    param([string[]]$Arguments = @())
    $exe = Get-GameExe
    if (-not (Test-Path $exe)) { throw "No build at $exe. Run scripts\build.ps1 first." }
    $quoted = $Arguments | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }
    $p = Start-Process -FilePath $exe -ArgumentList $quoted -NoNewWindow -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "$exe $($Arguments -join ' ') failed with exit code $($p.ExitCode)" }
}

function Read-DepsEnv {
    $map = @{}
    foreach ($line in Get-Content (Join-Path $script:Root 'scripts\deps.env')) {
        if ($line -match '^\s*([A-Z0-9_]+)=(.+)$') { $map[$Matches[1]] = $Matches[2].Trim() }
    }
    return $map
}

# Loads the Visual Studio 2022 x64 build environment into this session, once.
function Enter-VsDevEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'Visual Studio 2022 (Desktop development with C++) is required. Run scripts\doctor.ps1.'
    }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio C++ tools not found. Run scripts\doctor.ps1.' }
    $devShell = Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    Import-Module $devShell
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

function Get-BuildDir { if ($env:BUILD_DIR) { $env:BUILD_DIR } else { Join-Path $script:Root 'build\release' } }
function Get-DepsPrefix { if ($env:DEPS_PREFIX) { $env:DEPS_PREFIX } else { Join-Path $script:Root 'deps\prefix' } }
function Get-GameExe { Join-Path (Get-BuildDir) 'Pizza Legacy.exe' }
function Get-TestExe { Join-Path (Get-BuildDir) 'test_plegacy.exe' }
function Get-Python {
    foreach ($candidate in 'py', 'python') {
        if (Get-Command $candidate -ErrorAction SilentlyContinue) { return $candidate }
    }
    throw 'Python 3.10+ not found. Fix: winget install --id Python.Python.3.12 -e (tick "Add to PATH").'
}

# A Python tool (ruff, black, mypy, pytest, bandit, pip-audit): as a module of
# the project's Python when it is installed there, else from PATH. Returns the
# command as an array, or $null when it is nowhere.
function Get-PyTool {
    param([Parameter(Mandatory)][string]$Module, [string]$Exe = $Module)
    $py = Get-Python
    & $py -c "import importlib.util, sys; sys.exit(0 if importlib.util.find_spec('$Module') else 1)" | Out-Null
    if ($LASTEXITCODE -eq 0) { return , @($py, '-m', $Module) }
    $cmd = Get-Command $Exe -ErrorAction SilentlyContinue
    if ($cmd) { return , @($cmd.Source) }
    return $null
}

# Runs a required Python tool, or stops with the command that installs it.
function Invoke-PyTool {
    param([Parameter(Mandatory)][string]$Module, [string[]]$Arguments = @(), [string]$Exe = $Module)
    $tool = Get-PyTool -Module $Module -Exe $Exe
    if (-not $tool) {
        Write-Bad "$Module is not installed"
        Write-Note 'fix: python -m pip install -r tools\hdpack\requirements-dev.txt'
        throw "$Module is not installed"
    }
    Invoke-Checked -File $tool[0] -Arguments (@($tool | Select-Object -Skip 1) + $Arguments)
}

# C++ files this fork adds or changes since the upstream import. Upstream
# files keep upstream's formatting (clang-format versions differ slightly).
function Get-ChangedCppFile {
    # The upstream import: its tag, or the first commit (which is that import).
    $base = 'upstream-pizzalegacy-1.0.0-beta.2'
    git -C $script:Root rev-parse -q --verify $base | Out-Null
    if ($LASTEXITCODE -ne 0) {
        $base = @(git -C $script:Root rev-list --max-parents=0 HEAD) | Select-Object -Last 1
        if (-not $base) { return @() }
    }
    $names = @(git -C $script:Root diff --name-only $base -- src tests) +
             @(git -C $script:Root ls-files --others --exclude-standard -- src tests)
    return $names | Where-Object { $_ -match '\.(cpp|h|inl)$' } | Sort-Object -Unique |
        ForEach-Object { Join-Path $script:Root $_ } | Where-Object { Test-Path $_ }
}

# The version of the configured build: CMake writes it from project(VERSION)
# into <build>\version.txt at configure time (the one source of truth).
function Get-BuildVersion {
    $file = Join-Path (Get-BuildDir) 'version.txt'
    if (-not (Test-Path $file)) {
        throw ("No version file at $file. CMake writes it at configure time from project(VERSION) in " +
               'CMakeLists.txt; run scripts\build.ps1 (with a CMakeLists.txt that has the version.txt step).')
    }
    $v = (Get-Content -Path $file -TotalCount 1).Trim()
    if ($v -notmatch '^\d+\.\d+\.\d+([-+][0-9A-Za-z.-]+)?$') { throw "Bad version '$v' in $file" }
    return $v
}
