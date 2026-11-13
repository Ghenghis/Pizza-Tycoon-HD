<#
.SYNOPSIS
  Builds the pinned dependencies (SDL3, zlib, Catch2) into deps\prefix and
  copies libADLMIDI to third_party\libadlmidi. Safe to run again.
#>
. "$PSScriptRoot\common.ps1"
Enter-VsDevEnvironment
$deps = Read-DepsEnv
$src = Join-Path $script:Root 'deps\src'
$prefix = Get-DepsPrefix
New-Item -ItemType Directory -Force -Path $src, $prefix | Out-Null

function Get-Source([string]$Name, [string]$Url, [string]$Tag) {
    $dir = Join-Path $src $Name
    if (-not (Test-Path (Join-Path $dir '.git'))) {
        Invoke-Checked git @('clone', '-q', '--depth', '1', '--branch', $Tag, $Url, $dir)
    }
    return $dir
}
function Build-Dep([string]$Name, [string[]]$Extra = @()) {
    $dir = Join-Path $src $Name
    $rev = (git -C $dir rev-parse --short HEAD)
    $stamp = Join-Path $dir ".installed-$rev"
    if (Test-Path $stamp) { Write-Ok "$Name (already built)"; return }
    $build = Join-Path $dir 'build'
    Invoke-Checked cmake (@('-S', $dir, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_INSTALL_PREFIX=$prefix") + $Extra)
    Invoke-Checked cmake @('--build', $build)
    Invoke-Checked cmake @('--install', $build)
    New-Item -ItemType File -Path $stamp | Out-Null
    Write-Ok $Name
}

Get-Source 'SDL' 'https://github.com/libsdl-org/SDL.git' $deps.SDL_TAG | Out-Null
Get-Source 'zlib' 'https://github.com/madler/zlib.git' $deps.ZLIB_TAG | Out-Null
Get-Source 'Catch2' 'https://github.com/catchorg/Catch2.git' $deps.CATCH2_TAG | Out-Null
$adl = Get-Source 'libADLMIDI' 'https://github.com/Wohlstand/libADLMIDI.git' $deps.ADLMIDI_TAG

Build-Dep 'SDL' @('-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF')
Build-Dep 'zlib'
Build-Dep 'Catch2' @('-DBUILD_TESTING=OFF')

$vendored = Join-Path $script:Root 'third_party\libadlmidi'
if (-not (Test-Path (Join-Path $vendored 'CMakeLists.txt'))) {
    if (Test-Path $vendored) { Remove-Item -Recurse -Force $vendored }
    Copy-Item -Recurse $adl $vendored
    Remove-Item -Recurse -Force (Join-Path $vendored '.git')
}
Write-Ok 'libADLMIDI -> third_party\libadlmidi'
Write-Host "Dependencies in $prefix"
