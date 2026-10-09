# 集中検証の後、既存の正式 gate を同じ source に対して順番に実行する。
[CmdletBinding()]
param([string]$Prefix = ('build/math-p44-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pwshExe = (Get-Process -Id $PID).Path
$records = @()
Write-Host '【操作可】通常検証は背面・入力透過 window と既存 display-power lease を使います。'
& $pwshExe scripts/math-p44-focused.ps1 -EvidenceDirectory ($Prefix + '-Focused')
$records += @{ stage = 'Focused'; exit = $LASTEXITCODE }
foreach ($stage in @('Regressions', 'BuildIndependent', 'Lint', 'Release')) {
    & $pwshExe scripts/math-p43-gate.ps1 -Stage $stage -EvidenceDirectory ($Prefix + '-' + $stage)
    $records += @{ stage = $stage; exit = $LASTEXITCODE }
}
$records | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath ($Prefix + '-validation.json') -Encoding utf8NoBOM
if (@($records | Where-Object { $_.exit -ne 0 }).Count -gt 0) { exit 1 }
