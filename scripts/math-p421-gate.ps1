<#
.SYNOPSIS
    P4-2.1 の新規証拠を取得する。P4-2 の証拠を上書きしない。
#>
[CmdletBinding()]
param(
    [ValidateSet('Focused', 'Real', 'Regressions', 'BuildIndependent', 'Lint', 'Release', 'Mutation')]
    [string]$Stage = 'Focused'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$evidence = Join-Path $repoRoot ('build/math-p421-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Stage)
if ($Stage -eq 'Mutation') {
    # 共通 gate の snapshot と記録を使い、追加の負例だけを選択する。
    & (Join-Path $PSScriptRoot 'math-p42-gate.ps1') -Stage PublicationMutation -EvidenceDirectory $evidence
} else {
    & (Join-Path $PSScriptRoot 'math-p42-gate.ps1') -Stage $Stage -EvidenceDirectory $evidence
}
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
