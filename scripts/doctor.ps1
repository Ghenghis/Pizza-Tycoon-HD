<#
.SYNOPSIS
  Checks everything Pizza Tycoon HD needs and prints the fix for anything missing.
.PARAMETER GamePath
  Folder with the original PT.EXE (optional).
.PARAMETER ComfyServer
  ComfyUI address to check (optional), e.g. http://127.0.0.1:8188
#>
param([string]$GamePath = '', [string]$ComfyServer = '')
. "$PSScriptRoot\common.ps1"
$failed = 0
function Need([string]$Name, [scriptblock]$Test, [string]$Fix) {
    try { $r = & $Test } catch { $r = $null }
    if ($r) { Write-Ok "$Name $r" } else { Write-Bad $Name; Write-Note $Fix; $script:failed++ }
}

Need 'git' { (git --version) } 'winget install --id Git.Git -e'
Need 'cmake >= 3.25' { $v = (cmake --version | Select-Object -First 1); if ($v -match '(\d+)\.(\d+)' -and ([int]$Matches[1] -gt 3 -or [int]$Matches[2] -ge 25)) { $v } } 'winget install --id Kitware.CMake -e'
Need 'ninja' { (ninja --version) } 'winget install --id Ninja-build.Ninja -e'
Need 'Visual Studio 2022 C++' {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property displayName }
} 'winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive"'
Need 'python 3.10+' { $py = Get-Python; $v = (& $py --version 2>&1); if ("$v" -match '3\.(\d+)' -and [int]$Matches[1] -ge 10) { "$v" } } 'winget install --id Python.Python.3.12 -e'
Need 'python packages (Pillow, numpy)' { $py = Get-Python; & $py -c "import PIL, numpy; print('Pillow', PIL.__version__, 'numpy', numpy.__version__)" } "python -m pip install -r tools\hdpack\requirements-dev.txt"
Need 'clang-format' { (clang-format --version) } 'Included with Visual Studio C++ (Clang tools), or: winget install --id LLVM.LLVM -e'
Need 'dependencies built' { if (Test-Path (Join-Path (Get-DepsPrefix) 'include\SDL3\SDL.h')) { Get-DepsPrefix } } 'scripts\fetch-deps.ps1'
Need 'game built' { if (Test-Path (Get-GameExe)) { Get-GameExe } } 'scripts\build.ps1'

if ($GamePath) {
    Need 'original game' { if (Test-Path (Join-Path $GamePath 'PT.EXE')) { $GamePath } } 'Point -GamePath at the folder that contains PT.EXE (GOG/Steam: the Pizza Connection folder).'
}
if ($ComfyServer) {
    Need "ComfyUI at $ComfyServer" {
        $s = Invoke-RestMethod -Uri "$ComfyServer/system_stats" -TimeoutSec 5
        ($s.devices | ForEach-Object { "$($_.name) $([math]::Round($_.vram_total / 1GB, 1)) GiB" }) -join ', '
    } 'Start ComfyUI (run_nvidia_gpu.bat), then run this again.'
}

if ($failed) { Write-Host "$failed check(s) failed." -ForegroundColor Red; exit 1 }
Write-Host 'All checks passed.' -ForegroundColor Green
