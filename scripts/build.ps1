<#
.SYNOPSIS
  Release build with tests. Fails on any compiler warning.
#>
. "$PSScriptRoot\common.ps1"
Enter-VsDevEnvironment
$prefix = Get-DepsPrefix
if (-not (Test-Path (Join-Path $prefix 'include\SDL3\SDL.h')) -or
    -not (Test-Path (Join-Path $script:Root 'third_party\libadlmidi\CMakeLists.txt'))) {
    & "$PSScriptRoot\fetch-deps.ps1"
}
$build = Get-BuildDir
Invoke-Checked cmake @('-S', $script:Root, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_PREFIX_PATH=$prefix", '-DPLEGACY_BUILD_TESTS=ON', '-DUSE_CCACHE=OFF')
$log = Join-Path $build 'build.log'
& cmake --build $build *> $log
if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 40; throw 'Build failed.' }
$warnings = Select-String -Path $log -Pattern 'warning C\d+|warning:' -SimpleMatch:$false
if ($warnings) { $warnings | ForEach-Object { $_.Line }; throw 'Compiler warnings (zero allowed).' }
# SDL3.dll etc. next to the programs, for running and packaging.
Copy-Item (Join-Path $prefix 'bin\*.dll') $build -Force
Write-Ok "build: $(Get-GameExe) (0 warnings)"
