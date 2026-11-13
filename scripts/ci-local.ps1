<#
.SYNOPSIS
  Local CI on your own PC (no hosted runners): doctor, dependencies, build
  with zero warnings, all tests, then packaging.
#>
param([string]$GamePath = $env:PLEGACY_TEST_GAME_PATH)
. "$PSScriptRoot\common.ps1"
& "$PSScriptRoot\doctor.ps1"
& "$PSScriptRoot\fetch-deps.ps1"
& "$PSScriptRoot\build.ps1"
& "$PSScriptRoot\test.ps1" -GamePath $GamePath
& "$PSScriptRoot\lint.ps1"
& "$PSScriptRoot\release.ps1"
Write-Ok 'local CI passed'
