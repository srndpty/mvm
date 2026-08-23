[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('Good','GoodPhysicalSurplus','GoodBoundaryCohortDivergence',
                 'AuthorityInvalidPropagated',
                 'NegativeRetiredReason','NegativeRetiredOpportunityRegression',
                 'NegativeRuntimeOverride','NegativeUnknownOutcome',
                 'NegativeVblankSequence','NegativeVblankObserver','NegativeBoundaryNotBracketed',
                 'NegativeOutputChanged','NegativeEtwLoss','NegativeRingOverflow',
                 'NegativePresentedFrameMismatch','NegativeMultiPresentedOrdinal',
                 'NegativeIdentityE1','NegativeIdentityE2','NegativeIdentityP1',
                 'NegativeIdentityP2','NegativeIdentityP3','NegativeIdentityP4',
                 'NegativeIdentityU1','NegativeIdentityS1','NegativeIdentityS2',
                 'NegativeIdentityN1','NegativeContractVersion',
                 'GoodSourceHalfRate','GoodForeignIntentAtBoundary',
                 'NegativeIntentMissing','NegativeIntentAmbiguous',
                 'NegativeIntentOutOfDomain','NegativeIntentDuplicateDisplay')]
    [string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null

# 基準: required=3600, physical=3600
# Layer 2 cohort   present_event=3595 (presented 3590 / discarded 5 / unknown 0)
# Layer 3 domain   filled=3590 (unique 3580 / repeated 10), unfilled=10 (tail 4)
# Layer 1A bridge  satisfied_intent=3590 (foreign intent 0), true_drop=10
$required=3600;$satisfied=3590;$physical=3600
$presentedEvents=3590;$discardedEvents=5;$unknown=0
$events=$presentedEvents+$discardedEvents+$unknown;$native=$events
$inDomainPresented=3590;$filled=3590;$unique=3580;$repeated=10
$unfilled=$physical-$filled;$tailUnfilled=4
$overhang=[Math]::Max($required-$physical,0)
$surplus=[Math]::Max($physical-$required,0)
$trueDrop=$required-$satisfied
$multiPresented=0
$foreignIntent=0;$intentMissing=0;$intentAmbiguous=0;$intentOutOfDomain=0;$intentDuplicate=0
$contractVersion='P2-D5-2-v2';$authorityProfile='P2-D5-2-v2';$override=$false
$authorityValid=$true;$authorityError='NONE'
$mismatch=0;$etwEvents=0;$etwBuffers=0;$ringOverflow=0
$sequence='OK';$overflowCount=0;$waitFailure=0;$longInterval=0;$shortInterval=0
$cumulative=$true;$ordinalConsecutive=$true;$outputStable=$true;$bracketed=$true

switch($Case){
    'GoodPhysicalSurplus'{
        # 60.05Hz 等で physical が required を上回る。unique は required を超えてよい。
        $physical=3603;$filled=3603;$unique=3603;$repeated=0
        $inDomainPresented=$filled;$unfilled=$physical-$filled;$tailUnfilled=0
        $satisfied=3600;$foreignIntent=3;$trueDrop=$required-$satisfied
        $surplus=[Math]::Max($physical-$required,0);$overhang=0
        $presentedEvents=3603;$events=$presentedEvents+$discardedEvents+$unknown;$native=$events
    }
    'GoodBoundaryCohortDivergence'{
        # Layer2 cohort と Layer3 domain は別集合。大小関係を要求しない。
        # measurement前submitでdomain内Displayed 8件、domain外Displayed 6件。
        $presentedEvents=3584          # cohort 側は domain 外 Displayed を含む
        $events=$presentedEvents+$discardedEvents+$unknown;$native=$events
        $inDomainPresented=3590;$filled=3590   # domain 側は前段 submit 分を含む
    }
    'AuthorityInvalidPropagated'{$authorityValid=$false;$authorityError='PRESENT_EVENT_AMBIGUOUS'}
    'NegativeRetiredReason'     {$authorityValid=$false;$authorityError='RENDER_SWAP_MISMATCH'}
    'NegativeRetiredOpportunityRegression'{$authorityValid=$false;$authorityError='OPPORTUNITY_REGRESSION'}
    'NegativeRuntimeOverride'   {$override=$true}
    'NegativeUnknownOutcome'    {$unknown=3;$events=$presentedEvents+$discardedEvents+$unknown;$native=$events}
    'NegativeVblankSequence'    {$sequence='BREAK'}
    'NegativeVblankObserver'    {$longInterval=1}
    'NegativeBoundaryNotBracketed'{$bracketed=$false}
    'NegativeOutputChanged'     {$outputStable=$false}
    'NegativeEtwLoss'           {$etwEvents=1}
    'NegativeRingOverflow'      {$ringOverflow=1}
    'NegativePresentedFrameMismatch'{$mismatch=2}
    'NegativeMultiPresentedOrdinal'{$multiPresented=1}
    'NegativeIdentityE1'        {$events=$events+1;$native=$events}
    'NegativeIdentityE2'        {$native=$events+1}
    'NegativeIdentityP1'        {$unfilled=$unfilled+1}
    'NegativeIdentityP2'        {$repeated=$repeated+1}
    'NegativeIdentityP3'        {$inDomainPresented=$filled+1}
    'NegativeIdentityP4'        {$tailUnfilled=$unfilled+1}
    'NegativeIdentityU1'        {$unique=$physical+1;$repeated=$filled-$unique}
    'NegativeIdentityS1'        {$satisfied=$required+1;$trueDrop=$required-$satisfied}
    'NegativeIdentityS2'        {$trueDrop=$trueDrop+1}
    'NegativeContractVersion'   {$contractVersion='P2-D5-2'}
    'GoodSourceHalfRate'{
        # 30fps source を 60Hz で表示。distinct intent 2つが同一 source frame を
        # 正しく表示する。satisfied_intent と unique_physical は一致しない。
        $unique=1795;$repeated=$filled-$unique
    }
    'GoodForeignIntentAtBoundary'{
        # 前 measurement 由来 intent の in-domain Presented event が 12 件。
        # physical opportunity は埋めるが intent satisfaction には寄与しない。
        $foreignIntent=12;$satisfied=$inDomainPresented-$foreignIntent
        $trueDrop=$required-$satisfied
    }
    'NegativeIntentMissing'        {$intentMissing=1}
    'NegativeIntentAmbiguous'      {$intentAmbiguous=1}
    'NegativeIntentOutOfDomain'    {$intentOutOfDomain=1}
    'NegativeIntentDuplicateDisplay'{$intentDuplicate=1}
    'NegativeIdentityN1'           {$foreignIntent=5}
}

$formal=[ordered]@{
    formal_contract_version=$contractVersion
    formal_authority_profile=$authorityProfile
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
    formal_physical_ordinal_multi_presented_count=$multiPresented
    formal_required_intent_count=$required
    formal_satisfied_intent_count=$satisfied
    formal_in_domain_presented_foreign_intent_count=$foreignIntent
    formal_intent_identity_missing_count=$intentMissing
    formal_intent_identity_ambiguous_count=$intentAmbiguous
    formal_intent_ordinal_out_of_domain_count=$intentOutOfDomain
    formal_intent_duplicate_display_count=$intentDuplicate
    formal_physical_opportunity_count=$physical
    formal_successful_native_present_count=$native
    formal_present_event_count=$events
    formal_presented_event_count=$presentedEvents
    formal_discarded_event_count=$discardedEvents
    formal_present_outcome_unknown_count=$unknown
    formal_in_domain_presented_event_count=$inDomainPresented
    formal_filled_physical_opportunity_count=$filled
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
$checkerLog=Join-Path $Directory 'checker.txt'
$failed=$false
try{
    & pwsh -NoProfile -File $Checker -Json $formalPath -Output $output *> $checkerLog
    if($LASTEXITCODE-ne0){$failed=$true}
}catch{$failed=$true}
$checkerText=if(Test-Path -LiteralPath $checkerLog){Get-Content -LiteralPath $checkerLog -Raw}else{''}

$expectedReason=switch($Case){
    'NegativeRetiredReason'               {'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativeRetiredOpportunityRegression'{'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativeRuntimeOverride'             {'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativeContractVersion'             {'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativeUnknownOutcome'              {'PRESENT_OUTCOME_UNKNOWN'}
    'NegativeVblankSequence'              {'PHYSICAL_VBLANK_SEQUENCE_BREAK'}
    'NegativeVblankObserver'              {'PHYSICAL_VBLANK_OBSERVER_INVALID'}
    'NegativeBoundaryNotBracketed'        {'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED'}
    'NegativeOutputChanged'               {'OUTPUT_OR_MODE_CHANGED'}
    'NegativeEtwLoss'                     {'ETW_LOSS'}
    'NegativeRingOverflow'                {'RING_OVERFLOW'}
    'NegativePresentedFrameMismatch'      {'PRESENTED_FRAME_MISMATCH'}
    'NegativeMultiPresentedOrdinal'       {'PHYSICAL_DISPLAY_IDENTITY_AMBIGUOUS'}
    'NegativeIdentityE2'                  {'PRESENT_EVENT_MISSING'}
    'NegativeIdentityP3'                  {'PHYSICAL_DISPLAY_IDENTITY_AMBIGUOUS'}
    'NegativeIdentityS1'                  {'INTENT_IDENTITY_AMBIGUOUS'}
    'NegativeIdentityN1'                  {'ACCOUNTING_IDENTITY_VIOLATION'}
    'NegativeIntentMissing'               {'INTENT_IDENTITY_MISSING'}
    'NegativeIntentAmbiguous'             {'INTENT_IDENTITY_AMBIGUOUS'}
    'NegativeIntentOutOfDomain'           {'INTENT_ORDINAL_OUT_OF_DOMAIN'}
    'NegativeIntentDuplicateDisplay'      {'INTENT_DUPLICATE_DISPLAY'}
    'NegativeIdentityE1'                  {'ACCOUNTING_IDENTITY_VIOLATION'}
    'NegativeIdentityP1'                  {'ACCOUNTING_IDENTITY_VIOLATION'}
    'NegativeIdentityP2'                  {'ACCOUNTING_IDENTITY_VIOLATION'}
    'NegativeIdentityP4'                  {'ACCOUNTING_IDENTITY_VIOLATION'}
    'NegativeIdentityU1'                  {'ACCOUNTING_IDENTITY_VIOLATION'}
    'NegativeIdentityS2'                  {'ACCOUNTING_IDENTITY_VIOLATION'}
    default                               {$null}
}

if($null-ne$expectedReason){
    if(-not$failed){throw "negative caseが違反を検出できませんでした: $Case"}
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
    # performance_pass / drop_rate は null。false にすると threshold 超過と混同する。
    if($null-ne$proof.performance_pass){throw 'performance_passがnullではありません'}
    if($null-ne$proof.drop_rate){throw 'drop_rateがnullではありません'}
    if([string]$proof.formal_authority_error-ne'PRESENT_EVENT_AMBIGUOUS'){throw 'errorが伝播していません'}
    Write-Host "P2-D5-2 formal v2 contract: PASS ($Case -> AUTHORITY_INVALID)"
    exit 0
}
if([string]$proof.status-ne'PASS'){throw "statusがPASSではありません: $($proof.status)"}
if(-not[bool]$proof.performance_evaluated){throw 'performanceが評価されていません'}
if([long]$proof.formal_true_drop_count-ne$trueDrop){throw 'true_dropが一致しません'}
if($Case-eq'GoodPhysicalSurplus'){
    if([long]$proof.formal_intent_surplus_count-le0){throw 'intent_surplusが記録されていません'}
    if([long]$proof.formal_intent_overhang_count-ne0){throw 'overhangが0ではありません'}
    # unique が required を超えても契約違反にしない。
    if([long]$proof.formal_displayed_unique_physical_count-le[long]$proof.formal_required_intent_count){
        throw 'physical surplus caseでuniqueがrequiredを超えていません'
    }
}
if($Case-eq'GoodSourceHalfRate'){
    # source-frame uniqueness と intent satisfaction を同一視しない。
    if([long]$proof.formal_satisfied_intent_count-le[long]$proof.formal_displayed_unique_physical_count){
        throw 'satisfied_intentがunique_physicalを上回っていません'
    }
}
if($Case-eq'GoodForeignIntentAtBoundary'){
    if([long]$proof.formal_in_domain_presented_foreign_intent_count-le0){
        throw 'foreign intentが記録されていません'
    }
    if([long]$proof.formal_satisfied_intent_count-ge[long]$proof.formal_in_domain_presented_event_count){
        throw 'foreign intent分がsatisfiedから除かれていません'
    }
}
if($Case-eq'GoodBoundaryCohortDivergence'){
    if([long]$proof.formal_presented_event_count-ge[long]$proof.formal_in_domain_presented_event_count){
        throw 'cohort divergenceが再現していません'
    }
}
Write-Host "P2-D5-2 formal v2 contract: PASS ($Case)"
