<#
.SYNOPSIS
  All quality gates that need no GPU. No windows open: SDL runs offscreen.
.PARAMETER GamePath
  Folder with PT.EXE; enables the tests that use the original art.
#>
param([string]$GamePath = $env:PLEGACY_TEST_GAME_PATH)
. "$PSScriptRoot\common.ps1"
$build = Get-BuildDir
$env:SDL_VIDEO_DRIVER = 'offscreen'
$env:SDL_AUDIO_DRIVER = 'dummy'
if ($GamePath) { $env:PLEGACY_TEST_GAME_PATH = $GamePath }

Write-Host '== format (clang-format, files this fork adds or changes)'
$files = @(Get-ChangedCppFiles)
if ($files.Count -gt 0) { Invoke-Checked clang-format (@('--dry-run', '--Werror') + $files) }
else { Write-Note 'skipped: not a git checkout' }

Write-Host '== C++ tests'
Invoke-Checked (Join-Path $build 'test_plegacy.exe')

Write-Host '== pack tool: format, lint, types, tests'
$py = Get-Python
Push-Location (Join-Path $script:Root 'tools\hdpack')
try {
    Invoke-Checked $py @('-m', 'black', '--check', '-q', '.')
    Invoke-Checked $py @('-m', 'ruff', 'check', '-q', '.')
    Invoke-Checked $py @('-m', 'mypy', 'hdpack.py')
    if ($GamePath) {
        $tmp = Join-Path ([IO.Path]::GetTempPath()) ("plegacy-" + [guid]::NewGuid())
        Invoke-Game @('--config', "$tmp\settings", '--game-path', $GamePath, '--export-assets', "$tmp\export")
        $env:PLEGACY_TEST_EXPORT_DIR = "$tmp\export"
        try { Invoke-Checked $py @('-m', 'pytest', '-q') } finally { Remove-Item -Recurse -Force $tmp }
    } else {
        Invoke-Checked $py @('-m', 'pytest', '-q')
    }
} finally { Pop-Location }
Write-Ok 'all gates passed'
