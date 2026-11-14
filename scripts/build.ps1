# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
  Release build with tests (pinned dependencies from fetch-deps.ps1, CMake,
  Ninja, MSVC). Fails on any compiler or linker warning and on any CMake
  warning from this project's own CMake files.
#>
. "$PSScriptRoot\common.ps1"
Enter-VsDevEnvironment
$prefix = Get-DepsPrefix
if (-not (Test-Path (Join-Path $prefix 'include\SDL3\SDL.h')) -or
    -not (Test-Path (Join-Path $script:Root 'third_party\libadlmidi\CMakeLists.txt'))) {
    & "$PSScriptRoot\fetch-deps.ps1"
}
$build = Get-BuildDir
New-Item -ItemType Directory -Force -Path $build | Out-Null

$configureLog = Join-Path $build 'configure.log'
$code = Invoke-Logged -File cmake -Log $configureLog -Arguments @('-S', $script:Root, '-B', $build, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$prefix", '-DPLEGACY_BUILD_TESTS=ON', '-DUSE_CCACHE=OFF', '-Wno-dev')
if ($code -ne 0) { Get-Content $configureLog -Tail 40; throw "CMake configure failed (log: $configureLog)." }
# Third-party subprojects are not ours to fix; this project's own warnings are.
$cmakeWarnings = Select-String -Path $configureLog -Pattern '^CMake Warning( \(dev\))? at (CMakeLists\.txt|cmake[\\/])'
if ($cmakeWarnings) {
    $cmakeWarnings | ForEach-Object { $_.Line }
    throw "CMake warnings (zero allowed; log: $configureLog)."
}

$log = Join-Path $build 'build.log'
$code = Invoke-Logged -File cmake -Arguments @('--build', $build) -Log $log
if ($code -ne 0) { Get-Content $log -Tail 40; throw "Build failed (log: $log)." }
# Compiler (C4xxx), linker (LNK4xxx) and command-line (D9xxx) warnings, and
# GCC/Clang style ones. Only files compiled in this run print theirs.
$warnings = Select-String -Path $log -Pattern 'warning (C|LNK|D)\d+|warning:'
if ($warnings) { $warnings | ForEach-Object { $_.Line }; throw "Compiler or linker warnings (zero allowed; log: $log)." }
# SDL3.dll etc. next to the programs, for running and packaging.
Copy-Item (Join-Path $prefix 'bin\*.dll') $build -Force
Write-Ok "build: $(Get-GameExe) (0 warnings)"
