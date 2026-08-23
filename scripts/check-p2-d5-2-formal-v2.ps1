[CmdletBinding()]
param(
    # v2 formal result JSON。W2 wiring がこの shape を produce する。
    [Parameter(Mandatory=$true)][string]$Json,
    [string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
# fail-close は performance FAIL ではない。authority invalid として区別する。
function Fail([string]$Reason,[string]$Message){throw ("{0}: {1}" -f $Reason,$Message)}
function Require($Object,[string]$Name,[string]$Reason='ACCOUNTING_IDENTITY_VIOLATION'){
    if($null-eq$Object-or-not($Object.PSObject.Properties.Name-contains$Name)){
        Fail $Reason "v2 canonical fieldがありません: $Name"
    }
    return $Object.$Name
}
function I64($Value){return [long]$Value}

$validReasons=@(
    'NONE','COMPOSITION_TOKEN_PRESENT_MISSING','COMPOSITION_TOKEN_PRESENT_AMBIGUOUS',
    'PRESENT_EVENT_MISSING','PRESENT_EVENT_AMBIGUOUS','PRESENT_OUTCOME_UNKNOWN',
    'DISPLAYED_QPC_MISSING','PHYSICAL_DISPLAY_IDENTITY_AMBIGUOUS',
    'PHYSICAL_VBLANK_SEQUENCE_BREAK','PHYSICAL_VBLANK_OBSERVER_INVALID',
    'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED','OUTPUT_OR_MODE_CHANGED',
    'PRESENTED_FRAME_MISMATCH','INTENT_IDENTITY_MISSING','INTENT_IDENTITY_AMBIGUOUS',
    'INTENT_ORDINAL_OUT_OF_DOMAIN','INTENT_DUPLICATE_DISPLAY','ETW_LOSS','RING_OVERFLOW',
    'ACCOUNTING_IDENTITY_VIOLATION','RUNTIME_AUTHORITY_OVERRIDE')
# W0.5-A で legacy frameSwapped invariant と証明済み。v2 に出現してはならない。
# OPPORTUNITY_REGRESSION は swapQpc 由来のものが legacy であるため含める。
$retiredReasons=@('RENDER_SWAP_MISMATCH','RENDER_WITHOUT_SWAP','SWAP_WITHOUT_RENDER',
                  'RENDER_NOT_COMPLETED','SWAP_ORDINAL_MISMATCH','OPPORTUNITY_REGRESSION')

if(-not(Test-Path -LiteralPath $Json)){Fail 'ACCOUNTING_IDENTITY_VIOLATION' "formal jsonがありません: $Json"}
$formal=Get-Content -LiteralPath $Json -Raw -Encoding utf8|ConvertFrom-Json

# --- メタ / authority profile ---
if([string](Require $formal 'formal_contract_version')-ne'P2-D5-2-v2'){
    Fail 'RUNTIME_AUTHORITY_OVERRIDE' 'contract versionがP2-D5-2-v2ではありません'
}
if([string](Require $formal 'formal_authority_profile')-ne'P2-D5-2-v2'){
    Fail 'RUNTIME_AUTHORITY_OVERRIDE' 'authority profileが一致しません'
}
if([bool](Require $formal 'formal_runtime_authority_override')){
    Fail 'RUNTIME_AUTHORITY_OVERRIDE' 'canonical authorityから逸脱しています'
}
$declaredError=[string](Require $formal 'formal_authority_error')
if($retiredReasons-contains$declaredError){
    Fail 'RUNTIME_AUTHORITY_OVERRIDE' "retired済みのlegacy reasonが出現しました: $declaredError"
}
if($validReasons-notcontains$declaredError){
    Fail 'ACCOUNTING_IDENTITY_VIOLATION' "未知のauthority errorです: $declaredError"
}
$declaredValid=[bool](Require $formal 'formal_authority_valid')
if($declaredValid-and$declaredError-ne'NONE'){
    Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'authority validなのにerrorが設定されています'
}
if(-not$declaredValid){
    if($declaredError-eq'NONE'){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'authority invalidなのにerrorがNONEです'}
    # fail-close した run は performance を評価しない。
    # performance_pass / drop_rate は null。false にすると threshold 超過の
    # 正式な performance FAIL と区別できなくなる。
    $result=[ordered]@{
        schema='mvm-p2-d5-2-formal-v2-proof-1';status='AUTHORITY_INVALID'
        formal_contract_version='P2-D5-2-v2';authority='formal'
        formal_authority_valid=$false;formal_authority_error=$declaredError
        performance_evaluated=$false;performance_pass=$null;drop_rate=$null
    }
    if(-not[string]::IsNullOrWhiteSpace($Output)){
        $result|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $Output -Encoding utf8
    }
    Write-Host "P2-D5-2 formal v2: AUTHORITY_INVALID ($declaredError)"
    exit 0
}

# --- Layer 1B fail-closed contract ---
$vblank=Require $formal 'physical_vblank' 'PHYSICAL_VBLANK_OBSERVER_INVALID'
if([string](Require $vblank 'sequence_status' 'PHYSICAL_VBLANK_SEQUENCE_BREAK')-ne'OK'){
    Fail 'PHYSICAL_VBLANK_SEQUENCE_BREAK' 'VBlank sequenceが破れています'
}
foreach($zeroField in @('ring_overflow_count','wait_failure_count','long_interval_count',
                        'short_interval_count')){
    if((I64 (Require $vblank $zeroField 'PHYSICAL_VBLANK_OBSERVER_INVALID'))-ne0){
        Fail 'PHYSICAL_VBLANK_OBSERVER_INVALID' "$zeroField が0ではありません"
    }
}
if(-not[bool](Require $vblank 'cumulative_consistent' 'PHYSICAL_VBLANK_OBSERVER_INVALID')){
    Fail 'PHYSICAL_VBLANK_OBSERVER_INVALID' 'cumulative consistencyが成立していません'
}
if(-not[bool](Require $vblank 'ordinal_consecutive' 'PHYSICAL_VBLANK_SEQUENCE_BREAK')){
    Fail 'PHYSICAL_VBLANK_SEQUENCE_BREAK' 'physical_vblank_ordinalがstrictly consecutiveではありません'
}
if(-not[bool](Require $formal 'formal_physical_output_stable' 'OUTPUT_OR_MODE_CHANGED')){
    Fail 'OUTPUT_OR_MODE_CHANGED' 'adapter/output/refresh rationalが変化しました'
}
# 両端の bracket が無いと tail の exact accounting が閉じない。
if(-not[bool](Require $formal 'formal_physical_vblank_boundary_bracketed' 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED')){
    Fail 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED' 'measurement両端がbracketされていません'
}

# --- ETW / ring ---
foreach($lossField in @('etw_events_lost','etw_buffers_lost')){
    if((I64 (Require $formal $lossField 'ETW_LOSS'))-ne0){Fail 'ETW_LOSS' "$lossField が0ではありません"}
}
if((I64 (Require $formal 'present_event_overflow_count' 'RING_OVERFLOW'))-ne0){
    Fail 'RING_OVERFLOW' 'present event ringがoverflowしました'
}

# --- 保持する invariant ---
if((I64 (Require $formal 'formal_presented_frame_mismatch_count' 'PRESENTED_FRAME_MISMATCH'))-ne0){
    Fail 'PRESENTED_FRAME_MISMATCH' 'predicted targetFrameとrendered source frameが一致しません'
}
# 1 physical ordinal に 2件以上の Presented event が map されたら曖昧。
if((I64 (Require $formal 'formal_physical_ordinal_multi_presented_count' 'PHYSICAL_DISPLAY_IDENTITY_AMBIGUOUS'))-ne0){
    Fail 'PHYSICAL_DISPLAY_IDENTITY_AMBIGUOUS' '同一physical ordinalへ複数のPresented eventがmapされています'
}

# --- intent identity closure (W1.2) ---
# satisfied_intent_count を算術上の自由変数にしない。exact identity chain
#   intent_ordinal -> scheduler decision/targetFrame -> rendered source
#   -> composition token -> successful native Present -> exact PresentEvent
#   -> DisplayedQPC -> in-domain physical_vblank_ordinal
# が閉じた distinct intent_ordinal の cardinality として定義する。
if((I64 (Require $formal 'formal_intent_identity_missing_count' 'INTENT_IDENTITY_MISSING'))-ne0){
    Fail 'INTENT_IDENTITY_MISSING' '観測されたdisplayにintent identityがありません'
}
if((I64 (Require $formal 'formal_intent_identity_ambiguous_count' 'INTENT_IDENTITY_AMBIGUOUS'))-ne0){
    Fail 'INTENT_IDENTITY_AMBIGUOUS' 'display candidateのintent identityが曖昧です'
}
if((I64 (Require $formal 'formal_intent_ordinal_out_of_domain_count' 'INTENT_ORDINAL_OUT_OF_DOMAIN'))-ne0){
    Fail 'INTENT_ORDINAL_OUT_OF_DOMAIN' 'intent_ordinalがintent domainの外です'
}
# 1 intent_ordinal に対する in-domain Presented outcome は高々1件。
if((I64 (Require $formal 'formal_intent_duplicate_display_count' 'INTENT_DUPLICATE_DISPLAY'))-ne0){
    Fail 'INTENT_DUPLICATE_DISPLAY' '同一intent_ordinalが複数回in-domain表示されています'
}

# --- Layer 1A: workload intent ---
$required=I64 (Require $formal 'formal_required_intent_count')
$satisfied=I64 (Require $formal 'formal_satisfied_intent_count' 'INTENT_IDENTITY_AMBIGUOUS')
# 前 measurement 由来の intent を持つ in-domain Presented event。
# physical opportunity は埋めるが今回の intent satisfaction には寄与しない。
$foreignIntent=I64 (Require $formal 'formal_in_domain_presented_foreign_intent_count')
# --- Layer 1B: physical opportunity ---
$physical=I64 (Require $formal 'formal_physical_opportunity_count')
# --- Layer 2: PresentEvent outcome cohort ---
$native=I64 (Require $formal 'formal_successful_native_present_count')
$events=I64 (Require $formal 'formal_present_event_count')
$presentedEvents=I64 (Require $formal 'formal_presented_event_count')
$discardedEvents=I64 (Require $formal 'formal_discarded_event_count')
$unknown=I64 (Require $formal 'formal_present_outcome_unknown_count')
# --- Layer 3: measurement physical domain occupancy ---
$inDomainPresented=I64 (Require $formal 'formal_in_domain_presented_event_count')
$filled=I64 (Require $formal 'formal_filled_physical_opportunity_count')
$unique=I64 (Require $formal 'formal_displayed_unique_physical_count')
$repeated=I64 (Require $formal 'formal_repeated_physical_count')
$unfilled=I64 (Require $formal 'formal_physical_unfilled_count')
$tailUnfilled=I64 (Require $formal 'formal_tail_physical_unfilled_count')
# --- intent vs physical ---
$overhang=I64 (Require $formal 'formal_intent_overhang_count')
$surplus=I64 (Require $formal 'formal_intent_surplus_count')
$trueDrop=I64 (Require $formal 'formal_true_drop_count')

if($required-le0){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'required_intent_countが正ではありません'}
if($physical-lt0){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'physical_opportunity_countが負です'}
if($unknown-ne0){Fail 'PRESENT_OUTCOME_UNKNOWN' "terminalでないPresent outcomeがあります: $unknown"}

# E: Layer 2 PresentEvent cohort
if($presentedEvents+$discardedEvents+$unknown-ne$events){
    Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'E1違反: presented + discarded + unknown != present_event_count'
}
if($native-ne$events){
    Fail 'PRESENT_EVENT_MISSING' 'E2違反: successful native Present数とPresentEvent数が一致しません'
}

# P: Layer 3 measurement physical domain occupancy
# Layer 2 cohort と Layer 3 domain は別集合である。両者の大小関係は要求しない。
# (measurement前submitでdomain内Displayed / domain外Displayedの双方が存在しうる)
if($filled+$unfilled-ne$physical){
    Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'P1違反: filled + unfilled != physical_opportunity_count'
}
if($unique+$repeated-ne$filled){
    Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'P2違反: unique + repeated != filled_physical_opportunity_count'
}
# 1 ordinal あたり 0 or 1 の Presented event を要求済みなので同値になる。
if($inDomainPresented-ne$filled){
    Fail 'PHYSICAL_DISPLAY_IDENTITY_AMBIGUOUS' 'P3違反: in-domain Presented eventとfilled physical opportunityが一致しません'
}
# U: physical 側の observational bound。required では縛らない。
if($unique-lt0-or$unique-gt$physical){
    Fail 'ACCOUNTING_IDENTITY_VIOLATION' "U1違反: displayed_unique_physicalがphysical opportunityを超えています ($unique > $physical)"
}
if($tailUnfilled-lt0-or$tailUnfilled-gt$unfilled){
    Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'P4違反: tail_unfilledがunfilledを超えています'
}

# S1: satisfied_intent_count 自体が intent domain 内にあること。
if($satisfied-lt0-or$satisfied-gt$required){
    Fail 'INTENT_IDENTITY_AMBIGUOUS' "S1違反: satisfied_intent_countが範囲外です ($satisfied / $required)"
}

# N: intent satisfaction を Layer 3 occupancy に対して閉じる。
# in-domain の Presented event は「今回 intent を満たしたもの」か
# 「前 measurement 由来の intent を持つもの」かのいずれか。
# duplicate display は 0 を要求済みなので 1:1 で数えられる。
if($foreignIntent-lt0){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'foreign intent countが負です'}
if($satisfied+$foreignIntent-ne$inDomainPresented){
    Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'N1違反: satisfied_intent + foreign_intent != in_domain_presented_event_count'
}
if($trueDrop-ne$required-$satisfied){
    Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'S2違反: true_drop != required_intent - satisfied_intent'
}

# X: intent と physical の関係
if($overhang-ne[Math]::Max($required-$physical,0)){
    Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'X1違反: intent_overhangの定義と一致しません'
}
if($surplus-ne[Math]::Max($physical-$required,0)){
    Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'X2違反: intent_surplusの定義と一致しません'
}
if($overhang-ne0-and$surplus-ne0){
    Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'X3違反: overhangとsurplusが同時に非0です'
}

$dropRate=[double]$trueDrop/[double]$required
$result=[ordered]@{
    schema='mvm-p2-d5-2-formal-v2-proof-1';status='PASS';authority='formal'
    formal_contract_version='P2-D5-2-v2';formal_authority_profile='P2-D5-2-v2'
    formal_authority_valid=$true;formal_authority_error='NONE'
    formal_runtime_authority_override=$false
    performance_evaluated=$true
    # threshold 自体は変更しない。評価は W3 で行う。
    formal_required_intent_count=$required
    formal_satisfied_intent_count=$satisfied
    # scheduled intent に in-domain display が無いのは performance drop であり
    # authority invalid ではない。
    formal_unsatisfied_intent_count=($required-$satisfied)
    formal_in_domain_presented_foreign_intent_count=$foreignIntent
    formal_physical_opportunity_count=$physical
    formal_present_event_count=$events
    formal_presented_event_count=$presentedEvents
    formal_discarded_event_count=$discardedEvents
    formal_in_domain_presented_event_count=$inDomainPresented
    formal_filled_physical_opportunity_count=$filled
    formal_displayed_unique_physical_count=$unique
    formal_repeated_physical_count=$repeated
    formal_physical_unfilled_count=$unfilled
    formal_tail_physical_unfilled_count=$tailUnfilled
    formal_intent_overhang_count=$overhang
    formal_intent_surplus_count=$surplus
    formal_true_drop_count=$trueDrop
    drop_rate=$dropRate
    retired_reasons_absent=$true
}
if(-not[string]::IsNullOrWhiteSpace($Output)){
    $result|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $Output -Encoding utf8
}
Write-Host ("P2-D5-2 formal v2: PASS required={0} satisfied={1} physical={2} filled={3} trueDrop={4} dropRate={5:P3}" -f `
    $required,$satisfied,$physical,$filled,$trueDrop,$dropRate)
