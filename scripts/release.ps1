# SPDX-FileCopyrightText: 2026 ShadowByte
# SPDX-License-Identifier: GPL-3.0-or-later

<#
.SYNOPSIS
  Packages a Windows release from a finished build: a standalone zip (game,
  data, DLLs, pack tools, docs) and the bare exe, each with a SHA-256 file.
  Never includes original game files, exports or packs: the staged files
  are checked for them. Then the release gate (scripts\check-release.ps1)
  unpacks the zip into a temporary folder and checks it: every SHA-256, the
  file list against tools\ci\release-manifest.txt, the unpacked game's
  --version, and a short --exec run when game files are available. A
  release that fails it is removed.

  The version comes from the build: build\release\version.txt, which CMake
  writes from project(VERSION) in CMakeLists.txt, the one source of truth.

  A release (the default) needs a "Pizza Tycoon HD <version>" heading in
  docs\CHANGELOG.md, a clean working tree, HEAD tagged v<version>, and no
  earlier files of that version in release\ (-Force overwrites them; only
  for files that were never shared).
.PARAMETER Snapshot
  What local CI runs: none of those checks (a development version has no
  release notes yet), written to
  release\snapshot\PizzaTycoonHD_v<version>+<commit>_win64.*, so a CI run
  never overwrites a real release.
.PARAMETER Version
  The version you expect (optional): stops when the build's differs.
#>
param([string]$Version = '', [switch]$Snapshot, [switch]$Force)
. "$PSScriptRoot\common.ps1"
$build = Get-BuildDir
$exe = Get-GameExe
if (-not (Test-Path $exe)) { throw "No build at $exe. Run scripts\build.ps1 first." }

$buildVersion = Get-BuildVersion
if ($Version -and $Version -ne $buildVersion) {
    throw "-Version $Version was asked for, but the build is $buildVersion ($(Join-Path $build 'version.txt')). Rebuild, or leave -Version out."
}
$Version = $buildVersion
$commit = @(git -C $script:Root rev-parse --short HEAD) | Select-Object -First 1
if (-not $commit) { $commit = 'unknown' }

if ($Snapshot) {
    $out = Join-Path $script:Root 'release\snapshot'
    $name = "PizzaTycoonHD_v${Version}+${commit}_win64"
} else {
    $changelog = Join-Path $script:Root 'docs\CHANGELOG.md'
    if (-not (Select-String -Path $changelog -Pattern ('^Pizza Tycoon HD ' + [regex]::Escape($Version) + '(\s|$)') -Quiet)) {
        throw "No 'Pizza Tycoon HD $Version' heading in docs\CHANGELOG.md. Add the release notes for $Version first."
    }
    $dirty = @(git -C $script:Root status --porcelain)
    if ($dirty.Count -gt 0) {
        throw "The working tree has $($dirty.Count) uncommitted change(s). Commit them first (a release must match a commit), or use -Snapshot."
    }
    $tag = "v$Version"
    if (@(git -C $script:Root tag --points-at HEAD) -notcontains $tag) {
        throw "HEAD ($commit) is not tagged $tag. Tag the release commit (git tag -a $tag -m 'Pizza Tycoon HD $Version'), or use -Snapshot."
    }
    $out = Join-Path $script:Root 'release'
    $name = "PizzaTycoonHD_v${Version}_win64"
    $earlier = @("$name.zip", "$name.exe") | Where-Object { Test-Path (Join-Path $out $_) }
    if ($earlier -and -not $Force) {
        throw "release\$($earlier -join ', ') already exist(s): a release is never overwritten. Bump the version, or use -Force if those files were never shared."
    }
}
$stage = Join-Path $out $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item $exe $stage
Copy-Item -Recurse (Join-Path $build 'data') (Join-Path $stage 'data')
# Original Pizza Tycoon HD art (title screen); used when hd_scale is 2 or more.
Copy-Item -Recurse (Join-Path $script:Root 'assets\hd') (Join-Path $stage 'hd')
Get-ChildItem $build -Filter *.dll | Copy-Item -Destination $stage
foreach ($dll in 'SDL3.dll') {
    if (-not (Test-Path (Join-Path $stage $dll))) { throw "$dll missing from $build" }
}
$tools = Join-Path $stage 'tools\hdpack'
New-Item -ItemType Directory -Force -Path $tools | Out-Null
Copy-Item (Join-Path $script:Root 'tools\hdpack\hdpack.py'), (Join-Path $script:Root 'tools\hdpack\export.py'),
    (Join-Path $script:Root 'tools\hdpack\pixelart.py'), (Join-Path $script:Root 'tools\hdpack\depthmaps.py'),
    (Join-Path $script:Root 'tools\hdpack\redraw.py'), (Join-Path $script:Root 'tools\hdpack\review_pairs.py'),
    (Join-Path $script:Root 'tools\hdpack\provenance.py'), (Join-Path $script:Root 'tools\hdpack\maps.py'),
    (Join-Path $script:Root 'tools\hdpack\furniture_redraw.json'),
    (Join-Path $script:Root 'tools\hdpack\palettes.json'), (Join-Path $script:Root 'tools\hdpack\prompts.json'),
    (Join-Path $script:Root 'tools\hdpack\requirements.txt') $tools
Copy-Item -Recurse (Join-Path $script:Root 'tools\hdpack\workflows') $tools
foreach ($doc in 'README.md', 'SECURITY.md', 'docs\HD_REMASTER.md', 'docs\CHANGELOG.md', 'docs\CREDITS.md') {
    Copy-Item (Join-Path $script:Root $doc) $stage
}
Copy-Item -Recurse (Join-Path $script:Root 'LICENSES') (Join-Path $stage 'LICENSES')
Copy-Item (Join-Path $script:Root 'LICENSE') $stage

# The original game's files are not ours to ship. The only VGA picture in a
# release is the game location screen the engine draws itself.
$gameFiles = @(Get-ChildItem -Recurse -File $stage | Where-Object {
    $_.Extension -in '.bob', '.fnt', '.e', '.cmf', '.voc', '.pc', '.xmi', '.mid' -or
    ($_.Extension -eq '.vga' -and $_.Name -ne 'game_location.vga')
})
if ($gameFiles.Count -gt 0) {
    $gameFiles | ForEach-Object { Write-Note $_.FullName }
    throw "The release stage holds $($gameFiles.Count) original game file(s) (listed above). Remove them from the build's data folder."
}

$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
$bareExe = Join-Path $out "$name.exe"
Copy-Item $exe $bareExe
foreach ($f in $zip, $bareExe) {
    $hash = (Get-FileHash -Algorithm SHA256 $f).Hash.ToLower()
    "$hash  $(Split-Path -Leaf $f)" | Set-Content -Encoding ascii "$f.sha256"
}

# The release gate: unpack what a player gets and check it (scripts\check-release.ps1).
# A release that fails it is removed, so nothing half-checked is left to ship.
try {
    & "$PSScriptRoot\check-release.ps1" -Zip $zip -Exe $bareExe -Version $Version -Require 'SDL3.dll'
} catch {
    foreach ($f in $zip, "$zip.sha256", $bareExe, "$bareExe.sha256") {
        if (Test-Path $f) { Remove-Item -Force $f }
    }
    Write-Bad "release ${Version}: the release check failed, so its zip and exe were removed (the stage $stage is kept)"
    throw
}
Write-Ok "release ${Version} ($commit): $zip"
Write-Ok "release ${Version} ($commit): $bareExe (needs the DLLs and the data folder from the zip next to it)"
