[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Proof,
    [string]$SourceRoot=(Split-Path -Parent $PSScriptRoot),
    [string]$W4BChecker=(Join-Path $PSScriptRoot 'check-p2-d5-2-w4-b-producer-semantics.ps1'),
    [switch]$SkipW4BReplay,
    [string]$WorkDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Proof)){throw "W4-C1 proofがありません: $Proof"}
$actual=Get-Content -LiteralPath $Proof -Raw -Encoding utf8|ConvertFrom-Json
if([string]$actual.schema-ne'mvm-p2-d5-2-w4-c1-causal-compatibility-1'){
    throw 'W4-C1 schemaが不正です'
}
foreach($falseFlag in @('new_capture_performed','producer_instrumentation_changed',
        'canonical_performance_authority','historical_w3_verdict_rewritten',
        'root_cause_determined','branch_execution_exact','nearest_qpc_binding_used',
        'missing_state_interpolated','source_domain_required_domain_conflated',
        'alternative_stop_reason_excluded')){
    if($actual.PSObject.Properties.Name-notcontains$falseFlag-or[bool]$actual.$falseFlag){
        throw "W4-C1禁止flagが欠損またはtrueです: $falseFlag"
    }
}
if(-not[bool]$actual.c2_instrumentation_required){throw 'C2 instrumentation要否がfalseです'}
if([string]::IsNullOrWhiteSpace($WorkDirectory)){
    $WorkDirectory=Join-Path (Split-Path -Parent (Resolve-Path -LiteralPath $Proof).Path) `
        ((Split-Path -Leaf $Proof)+'.w4c1-check')
}
. (Join-Path $PSScriptRoot 'p2-d5-2-w4-c1-shared-replay.ps1')
$expected=Invoke-MvmW4C1FromProducerSemantics -W4BProofPath ([string]$actual.source_w4b_proof) `
    -SourceRoot $SourceRoot -W4BChecker $W4BChecker -WorkDirectory $WorkDirectory `
    -ReplayW4BChecker (-not$SkipW4BReplay)
Assert-MvmW4C1Proof -Expected $expected -Actual $actual
if(-not[bool]$expected.attribution_exact){
    throw "W4-C1 attributionがINVALIDです: $(@($expected.blockers)-join', ')"
}
Write-Host ("P2-D5-2 W4-C1 checker: PASS compatible={0} not_observable={1}" -f `
    $expected.compatible_transition_count,$expected.not_observable_transition_count)
