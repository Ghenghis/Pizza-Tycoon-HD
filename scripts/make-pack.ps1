<#
.SYNOPSIS
  Builds an HD pack from export\ and validates it.
.PARAMETER Mode
  upscale = baseline, no AI. comfy = generate with a local ComfyUI.
.EXAMPLE
  .\scripts\make-pack.ps1 -Mode comfy -Checkpoint juggernautXL_v9.safetensors -UpscaleModel 4x-UltraSharp.pth -Limit 10
#>
param(
    [ValidateSet('upscale', 'comfy')][string]$Mode = 'upscale',
    [string]$Export = (Join-Path (Resolve-Path "$PSScriptRoot\..") 'export'),
    [string]$Pack = (Join-Path (Resolve-Path "$PSScriptRoot\..") 'pack'),
    [int]$Scale = 6,
    [string[]]$Only = @(),
    [string]$Server = 'http://127.0.0.1:8188',
    [string]$Workflow = '',
    [string]$Checkpoint = '',
    [string]$UpscaleModel = '',
    [int]$Limit = 0,
    [switch]$Force
)
. "$PSScriptRoot\common.ps1"
$py = Get-Python
$tool = Join-Path $script:Root 'tools\hdpack\hdpack.py'
$toolArgs = @($tool, $Mode, $Export, $Pack, '--scale', "$Scale")
foreach ($o in $Only) { $toolArgs += @('--only', $o) }
if ($Force) { $toolArgs += '--force' }
if ($Mode -eq 'comfy') {
    $toolArgs += @('--server', $Server, '--checkpoint', $Checkpoint, '--upscale-model', $UpscaleModel)
    if ($Workflow) { $toolArgs += @('--workflow', $Workflow) }
    if ($Limit -gt 0) { $toolArgs += @('--limit', "$Limit") }
}
Invoke-Checked $py $toolArgs
Invoke-Checked $py @($tool, 'validate', $Export, $Pack)
Write-Ok "pack: $Pack"
