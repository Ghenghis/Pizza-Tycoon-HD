# Shared helpers for the Windows scripts. Dot-source: . "$PSScriptRoot\common.ps1"
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Write-Ok([string]$Text)   { Write-Host "PASS $Text" -ForegroundColor Green }
function Write-Bad([string]$Text)  { Write-Host "FAIL $Text" -ForegroundColor Red }
function Write-Note([string]$Text) { Write-Host "     $Text" -ForegroundColor DarkGray }

# Runs a program in this console (never a new window) and stops on failure.
function Invoke-Checked {
    param([Parameter(Mandatory)][string]$File, [string[]]$Arguments = @())
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$File $($Arguments -join ' ') failed with exit code $LASTEXITCODE"
    }
}

# The game is a GUI program (no console window). Start-Process -Wait is the
# way to wait for one and read its exit code; -NoNewWindow keeps its output
# in this console.
function Invoke-Game {
    param([string[]]$Arguments = @())
    $exe = Get-GameExe
    if (-not (Test-Path $exe)) { throw "No build at $exe. Run scripts\build.ps1 first." }
    $quoted = $Arguments | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }
    $p = Start-Process -FilePath $exe -ArgumentList $quoted -NoNewWindow -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "$exe failed with exit code $($p.ExitCode)" }
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
function Get-Python {
    foreach ($candidate in 'py', 'python') {
        if (Get-Command $candidate -ErrorAction SilentlyContinue) { return $candidate }
    }
    throw 'Python 3.10+ not found. Install it from python.org and tick "Add to PATH".'
}

# C++ files this fork adds or changes since the upstream import. Upstream
# files keep upstream's formatting (clang-format versions differ slightly).
function Get-ChangedCppFiles {
    $base = 'upstream-pizzalegacy-1.0.0-beta.2'
    git -C $script:Root rev-parse -q --verify $base *> $null
    if ($LASTEXITCODE -ne 0) { return @() }
    $names = @(git -C $script:Root diff --name-only $base -- src tests) +
             @(git -C $script:Root ls-files --others --exclude-standard -- src tests)
    return $names | Where-Object { $_ -match '\.(cpp|h|inl)$' } | Sort-Object -Unique |
        ForEach-Object { Join-Path $script:Root $_ } | Where-Object { Test-Path $_ }
}
