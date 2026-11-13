<#
.SYNOPSIS
  Packages a Windows release from a finished build: a standalone zip (game,
  data, DLLs, pack tools, docs) and the bare exe, each with a SHA-256 file.
  Never includes original game files, exports or packs.
#>
param([string]$Version = '')
. "$PSScriptRoot\common.ps1"
$build = Get-BuildDir
$exe = Get-GameExe
if (-not (Test-Path $exe)) { throw "No build at $exe. Run scripts\build.ps1 first." }
if (-not $Version) {
    $Version = (Select-String -Path (Join-Path $script:Root 'docs\CHANGELOG.md') -Pattern '^Pizza Tycoon HD (\S+)' | Select-Object -First 1).Matches[0].Groups[1].Value
}
$out = Join-Path $script:Root 'release'
$name = "PizzaTycoonHD_v${Version}_win64"
$stage = Join-Path $out $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item $exe $stage
Copy-Item -Recurse (Join-Path $build 'data') (Join-Path $stage 'data')
Get-ChildItem $build -Filter *.dll | Copy-Item -Destination $stage
foreach ($dll in 'SDL3.dll') {
    if (-not (Test-Path (Join-Path $stage $dll))) { throw "$dll missing from $build" }
}
$tools = Join-Path $stage 'tools\hdpack'
New-Item -ItemType Directory -Force -Path $tools | Out-Null
Copy-Item (Join-Path $script:Root 'tools\hdpack\hdpack.py'), (Join-Path $script:Root 'tools\hdpack\prompts.json'),
    (Join-Path $script:Root 'tools\hdpack\requirements.txt') $tools
Copy-Item -Recurse (Join-Path $script:Root 'tools\hdpack\workflows') $tools
foreach ($doc in 'README.md', 'SECURITY.md', 'docs\HD_REMASTER.md', 'docs\CHANGELOG.md', 'docs\CREDITS.md') {
    Copy-Item (Join-Path $script:Root $doc) $stage
}
Copy-Item -Recurse (Join-Path $script:Root 'LICENSES') (Join-Path $stage 'LICENSES')

$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
$bareExe = Join-Path $out "PizzaTycoonHD_v${Version}_win64.exe"
Copy-Item $exe $bareExe
foreach ($f in $zip, $bareExe) {
    $hash = (Get-FileHash -Algorithm SHA256 $f).Hash.ToLower()
    "$hash  $(Split-Path -Leaf $f)" | Set-Content -Encoding ascii "$f.sha256"
}
Write-Ok "release: $zip"
Write-Ok "release: $bareExe (needs the DLLs from the zip next to it)"
