[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('Good','GoodPhysicalSurplus','AuthorityInvalidPropagated',
                 'NegativeRetiredReason','NegativeRuntimeOverride','NegativeUnknownOutcome',
                 'NegativeVblankSequence','NegativeVblankObserver','NegativeBoundaryNotBracketed',
                 'NegativeOutputChanged','NegativeEtwLoss','NegativeRingOverflow',
                 'NegativePresentedFrameMismatch','NegativeIdentityI1','NegativeIdentityI2',
                 'NegativeIdentityI3','NegativeIdentityI4','NegativeIdentityI5',
                 'NegativeIdentityI6','NegativeIdentityI9','NegativeContractVersion')]
    [string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null

# 基準は required=3600, physical=3600, displayed=3590 (unique 3580 / repeated 10),
# unfilled=10 (うち tail 4), discarded=5。
$required=3600;$physical=3600;$displayed=3590;$unique=3580;$repeated=10
$unfilled=$physical-$displayed;$tailUnfilled=4;$discarded=5;$unknown=0
$events=$displayed+$discarded+$unknown;$native=$events
$overhang=[Math]::Max($required-$physical,0)
$surplus=[Math]::Max($physical-$required,0)
$trueDrop=[Math]::Max($required-$unique,0)
$contractVersion='P2-D5-2-v2';$profile='P2-D5-2-v2';$override=$false
$authorityValid=$true;$authorityError='NONE'
$mismatch=0;$etwEvents=0;$etwBuffers=0;$ringOverflow=0
$sequence='OK';$overflowCount=0;$waitFailure=0;$longInterval=0;$shortInterval=0
$cumulative=$true;$ordinalConsecutive=$true;$outputStable=$true;$bracketed=$true

switch($Case){
    'GoodPhysicalSurplus'{
        # 60.05Hz などで physical が required を上回るケース。
        $physical=3603;$displayed=3590;$unfilled=$physical-$displayed
        $surplus=[Math]::Max($physical-$required,0);$overhang=0
    }
    'AuthorityInvalidPropagated'{$authorityValid=$false;$authorityError='PRESENT_EVENT_AMBIGUOUS'}
    'NegativeRetiredReason'     {$authorityValid=$false;$authorityError='RENDER_SWAP_MISMATCH'}
    'NegativeRuntimeOverride'   {$override=$true}
    'NegativeUnknownOutcome'    {$unknown=3;$events=$displayed+$discarded+$unknown;$native=$events}
    'NegativeVblankSequence'    {$sequence='BREAK'}
    'NegativeVblankObserver'    {$longInterval=1}
    'NegativeBoundaryNotBracketed'{$bracketed=$false}
    'NegativeOutputChanged'     {$outputStable=$false}
    'NegativeEtwLoss'           {$etwEvents=1}
    'NegativeRingOverflow'      {$ringOverflow=1}
    'NegativePresentedFrameMismatch'{$mismatch=2}
    'NegativeIdentityI1'        {$unfilled=$unfilled+1}
    'NegativeIdentityI2'        {$repeated=$repeated+1}
    'NegativeIdentityI3'        {$events=$events+1;$native=$events}
    'NegativeIdentityI4'        {$native=$events+1}
    'NegativeIdentityI5'        {$unique=$required+1}
    'NegativeIdentityI6'        {$tailUnfilled=$unfilled+1}
    'NegativeIdentityI9'        {$trueDrop=$trueDrop+1}
    'NegativeContractVersion'   {$contractVersion='P2-D5-2'}
}

$formal=[ordered]@{
    formal_contract_version=$contractVersion
    formal_authority_profile=$profile
    formal_runtime_authority_override=$override
    formal_authority_valid=$authorityValid
    formal_authority_error=$authorityError
    physical_vblank=[ordered]@{
        sequence_status=$sequence;ring_overflow_count=$overflowCount
        wait_failure_count=$waitFailure;long_interval_count=$longInterval
        short_interval_count=$shortInterval;cumulative_consistent=$cumulative
        ordinal_consecutive=$ordinalConsecutive}
    formal_physical_output_stable=$outputStable
    formal_physical_vblank_boundary_bracketed=$bracketed
    formal_physical_vblank_origin_ordinal=1000
    formal_physical_vblank_origin_qpc=123456789
    etw_events_lost=$etwEvents;etw_buffers_lost=$etwBuffers
    present_event_overflow_count=$ringOverflow
    formal_presented_frame_mismatch_count=$mismatch
    formal_required_intent_count=$required
    formal_physical_opportunity_count=$physical
    formal_successful_native_present_count=$native
    formal_present_event_count=$events
    formal_displayed_count=$displayed
    formal_discarded_count=$discarded
    formal_present_outcome_unknown_count=$unknown
    formal_displayed_unique_physical_count=$unique
    formal_repeated_physical_count=$repeated
    formal_physical_unfilled_count=$unfilled
    formal_tail_physical_unfilled_count=$tailUnfilled
    formal_intent_overhang_count=$overhang
    formal_intent_surplus_count=$surplus
    formal_true_drop_count=$trueDrop
}
$formalPath=Join-Path $Directory 'formal.json'
$formal|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $formalPath -Encoding utf8

$output=Join-Path $Directory 'proof.json'
$stderrPath=Join-Path $Directory 'checker.txt'
$failed=$false
try{
    & pwsh -NoProfile -File $Checker -Json $formalPath -Output $output *> $stderrPath
    if($LASTEXITCODE-ne0){$failed=$true}
}catch{$failed=$true}
$checkerText=if(Test-Path -LiteralPath $stderrPath){Get-Content -LiteralPath $stderrPath -Raw}else{''}

$expectedReason=switch($Case){
    'NegativeRetiredReason'         {'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativeRuntimeOverride'       {'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativeContractVersion'       {'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativeUnknownOutcome'        {'PRESENT_OUTCOME_UNKNOWN'}
    'NegativeVblankSequence'        {'PHYSICAL_VBLANK_SEQUENCE_BREAK'}
    'NegativeVblankObserver'        {'PHYSICAL_VBLANK_OBSERVER_INVALID'}
    'NegativeBoundaryNotBracketed'  {'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED'}
    'NegativeOutputChanged'         {'OUTPUT_OR_MODE_CHANGED'}
    'NegativeEtwLoss'               {'ETW_LOSS'}
    'NegativeRingOverflow'          {'RING_OVERFLOW'}
    'NegativePresentedFrameMismatch'{'PRESENTED_FRAME_MISMATCH'}
    'NegativeIdentityI4'            {'PRESENT_EVENT_MISSING'}
    default                         {$null}
}
if($Case-like'NegativeIdentityI*'-and$null-eq$expectedReason){
    $expectedReason='ACCOUNTING_IDENTITY_VIOLATION'
}

if($null-ne$expectedReason){
    if(-not$failed){throw "negative caseが違反を検出できませんでした: $Case"}
    # reason が固有に発火していることを要求する。
    if($checkerText-notmatch [regex]::Escape($expectedReason)){
        throw "期待した fail-close reason が出ていません: expected=$expectedReason"
    }
    Write-Host "P2-D5-2 formal v2 contract: PASS ($Case -> $expectedReason)"
    exit 0
}
if($failed){throw "正の契約が失敗しました: $Case"}
$proof=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
if($Case-eq'AuthorityInvalidPropagated'){
    if([string]$proof.status-ne'AUTHORITY_INVALID'){throw "statusが不正です: $($proof.status)"}
    if([bool]$proof.performance_evaluated){throw 'authority invalidなのにperformanceを評価しました'}
    if([string]$proof.formal_authority_error-ne'PRESENT_EVENT_AMBIGUOUS'){throw 'errorが伝播していません'}
    Write-Host "P2-D5-2 formal v2 contract: PASS ($Case -> AUTHORITY_INVALID)"
    exit 0
}
if([string]$proof.status-ne'PASS'){throw "statusがPASSではありません: $($proof.status)"}
if(-not[bool]$proof.performance_evaluated){throw 'performanceが評価されていません'}
if([long]$proof.formal_true_drop_count-ne$trueDrop){throw 'true_dropが一致しません'}
if($Case-eq'GoodPhysicalSurplus'){
    # physical > required でも I9 は required 基準で閉じる。
    if([long]$proof.formal_intent_surplus_count-le0){throw 'intent_surplusが記録されていません'}
    if([long]$proof.formal_intent_overhang_count-ne0){throw 'overhangが0ではありません'}
}
Write-Host "P2-D5-2 formal v2 contract: PASS ($Case)"
