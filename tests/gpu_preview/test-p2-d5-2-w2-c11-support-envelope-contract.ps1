[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'Good','NegativeLowerAfterCapture','NegativePostrollBeforeClose',
        'NegativeMissingSuccessor','NegativeAuthorityFalse')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
# S2-f2: invocationごとに一意なdirectoryへ書き、warm build treeでのstale artifact
# 継承を構造的に不可能にする。expectation semanticsは変更しない。
$Directory=Join-Path $Directory ("inv-$PID-"+[guid]::NewGuid().ToString('N').Substring(0,12))
New-Item -ItemType Directory -Path $Directory -Force|Out-Null
$samples=@([ordered]@{ordinal=0;qpc=100},[ordered]@{ordinal=1;qpc=200},[ordered]@{ordinal=2;qpc=300})
$support=[ordered]@{
    schema='mvm-p2-d5-2-w2-c11-physical-mapping-support-envelope-1';shadow_only=$true
    performance_semantics_connected=$false;intent_satisfaction_connected=$false
    capture_begin_qpc=150;capture_close_qpc=220;producer_teardown_completed=$true;postroll_boundary_qpc=250
    predecessor_valid=$true;predecessor_ordinal=0;predecessor_qpc=100
    successor_valid=$true;successor_ordinal=2;successor_qpc=300
    postroll_wait_completed=$true;postroll_wait_timeout=$false;postroll_wait_elapsed_qpc=10
    lower_closed_before_candidate_capture=$true;upper_closed_after_candidate_capture_and_teardown=$true
    ring_overflow_count=0;wait_failure_count=0;output_stable=$true;authority_valid=$true
}
switch($Case){
    'NegativeLowerAfterCapture'{$support.predecessor_qpc=160}
    'NegativePostrollBeforeClose'{$support.postroll_boundary_qpc=210}
    'NegativeMissingSuccessor'{$support.successor_ordinal=9}
    'NegativeAuthorityFalse'{$support.authority_valid=$false}
}
$app=[ordered]@{presentation_opportunity=[ordered]@{
    physical_vblank=[ordered]@{samples=$samples}
    physical_mapping_support_envelope_shadow=$support
}}
$inputPath=Join-Path $Directory 'input.json';$outputPath=Join-Path $Directory 'proof.json'
$app|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $inputPath -Encoding utf8
& pwsh -NoProfile -File $Checker -InputJson $inputPath -Output $outputPath *> $null
if($Case-like'Negative*'){
    if($LASTEXITCODE-eq0){throw "$Case がfail-closeされませんでした"}
}elseif($LASTEXITCODE-ne0-or-not(Test-Path -LiteralPath $outputPath)){
    throw 'Good support envelopeが成立しません'
}
Write-Output "W2-C1.1 $Case support envelope contract: PASS"
