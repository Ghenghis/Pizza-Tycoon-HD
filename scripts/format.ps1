<# .SYNOPSIS Formats C++ (clang-format) and Python (black, ruff --fix). #>
. "$PSScriptRoot\common.ps1"
$files = @(Get-ChangedCppFiles)
if ($files.Count -gt 0) { Invoke-Checked clang-format (@('-i') + $files) }
$py = Get-Python
Push-Location (Join-Path $script:Root 'tools\hdpack')
try { Invoke-Checked $py @('-m', 'black', '-q', '.'); Invoke-Checked $py @('-m', 'ruff', 'check', '--fix', '-q', '.') } finally { Pop-Location }
Write-Ok 'formatted'
