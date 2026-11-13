<# .SYNOPSIS Static checks: PSScriptAnalyzer on these scripts, ruff and mypy on the pack tool. #>
. "$PSScriptRoot\common.ps1"
if (Get-Module -ListAvailable PSScriptAnalyzer) {
    $issues = Invoke-ScriptAnalyzer -Path $PSScriptRoot -Severity Warning, Error
    if ($issues) { $issues | Format-Table -AutoSize; throw 'PSScriptAnalyzer found problems.' }
    Write-Ok 'PSScriptAnalyzer'
} else {
    Write-Note 'PSScriptAnalyzer not installed: Install-Module PSScriptAnalyzer -Scope CurrentUser'
}
$py = Get-Python
Push-Location (Join-Path $script:Root 'tools\hdpack')
try { Invoke-Checked $py @('-m', 'ruff', 'check', '.'); Invoke-Checked $py @('-m', 'mypy', 'hdpack.py') } finally { Pop-Location }
Write-Ok 'lint'
