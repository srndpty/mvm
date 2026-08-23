[CmdletBinding()]
param(
    # v2 formal result JSON。W2 wiring がこの shape を produce する。
    [Parameter(Mandatory=$true)][string]$Json,
    [string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
# fail-close は performance FAIL ではない。authority invalid として区別する。
$script:Reason=$null
function Fail([string]$Reason,[string]$Message){
    if($null-eq$script:Reason){$script:Reason=$Reason}
    throw ("{0}: {1}" -f $Reason,$Message)
}
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
    'PRESENTED_FRAME_MISMATCH','ETW_LOSS','RING_OVERFLOW',
    'ACCOUNTING_IDENTITY_VIOLATION','RUNTIME_AUTHORITY_OVERRIDE')
# W0.5-A で legacy frameSwapped invariant と証明済み。v2 に出現してはならない。
$retiredReasons=@('RENDER_SWAP_MISMATCH','RENDER_WITHOUT_SWAP','SWAP_WITHOUT_RENDER',
                  'RENDER_NOT_COMPLETED','SWAP_ORDINAL_MISMATCH')

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
    # fail-closeしたrunはperformanceを評価しない。
    $result=[ordered]@{
        schema='mvm-p2-d5-2-formal-v2-proof-1';status='AUTHORITY_INVALID'
        formal_contract_version='P2-D5-2-v2';authority='formal'
        formal_authority_valid=$false;formal_authority_error=$declaredError
        performance_evaluated=$false
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
    Fail 'PHYSICAL_VBLANK_SEQUENCE_BREAK' 'ordinalがstrictly consecutiveではありません'
}
if(-not[bool](Require $formal 'formal_physical_output_stable' 'OUTPUT_OR_MODE_CHANGED')){
    Fail 'OUTPUT_OR_MODE_CHANGED' 'adapter/output/refresh rationalが変化しました'
}
# 両端のbracketが無いとtailのexact accountingが閉じない。
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

# --- accounting identities ---
$required=I64 (Require $formal 'formal_required_intent_count')
$physical=I64 (Require $formal 'formal_physical_opportunity_count')
$native=I64 (Require $formal 'formal_successful_native_present_count')
$events=I64 (Require $formal 'formal_present_event_count')
$displayed=I64 (Require $formal 'formal_displayed_count')
$discarded=I64 (Require $formal 'formal_discarded_count')
$unknown=I64 (Require $formal 'formal_present_outcome_unknown_count')
$unique=I64 (Require $formal 'formal_displayed_unique_physical_count')
$repeated=I64 (Require $formal 'formal_repeated_physical_count')
$unfilled=I64 (Require $formal 'formal_physical_unfilled_count')
$tailUnfilled=I64 (Require $formal 'formal_tail_physical_unfilled_count')
$overhang=I64 (Require $formal 'formal_intent_overhang_count')
$surplus=I64 (Require $formal 'formal_intent_surplus_count')
$trueDrop=I64 (Require $formal 'formal_true_drop_count')

if($required-le0){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'required_intent_countが正ではありません'}
if($physical-lt0){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'physical_opportunity_countが負です'}
if($unknown-ne0){Fail 'PRESENT_OUTCOME_UNKNOWN' "terminalでないPresent outcomeがあります: $unknown"}
# I1
if($displayed+$unfilled-ne$physical){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'I1違反: displayed + unfilled != physical_opportunity'}
# I2
if($unique+$repeated-ne$displayed){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'I2違反: unique + repeated != displayed'}
# I3
if($displayed+$discarded+$unknown-ne$events){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'I3違反: displayed + discarded + unknown != present_event_count'}
# I4  token -> native Present -> PresentEvent の 1:1
if($native-ne$events){Fail 'PRESENT_EVENT_MISSING' 'I4違反: successful native Present数とPresentEvent数が一致しません'}
# I5
$uniqueUpper=[Math]::Min($required,$physical)
if($unique-lt0-or$unique-gt$uniqueUpper){Fail 'ACCOUNTING_IDENTITY_VIOLATION' "I5違反: displayed_unique_physicalが範囲外です ($unique > $uniqueUpper)"}
# I6
if($tailUnfilled-lt0-or$tailUnfilled-gt$unfilled){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'I6違反: tail_unfilledがunfilledを超えています'}
# I7 / I8
if($overhang-ne[Math]::Max($required-$physical,0)){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'I7違反: intent_overhangの定義と一致しません'}
if($surplus-ne[Math]::Max($physical-$required,0)){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'I8違反: intent_surplusの定義と一致しません'}
# I9
if($trueDrop-ne[Math]::Max($required-$unique,0)){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'I9違反: true_dropの定義と一致しません'}
# I10
if($overhang-ne0-and$surplus-ne0){Fail 'ACCOUNTING_IDENTITY_VIOLATION' 'I10違反: overhangとsurplusが同時に非0です'}

$dropRate=[double]$trueDrop/[double]$required
$result=[ordered]@{
    schema='mvm-p2-d5-2-formal-v2-proof-1';status='PASS';authority='formal'
    formal_contract_version='P2-D5-2-v2';formal_authority_profile='P2-D5-2-v2'
    formal_authority_valid=$true;formal_authority_error='NONE'
    formal_runtime_authority_override=$false
    performance_evaluated=$true
    # threshold自体は変更しない。評価はW3で行う。
    formal_required_intent_count=$required
    formal_physical_opportunity_count=$physical
    formal_displayed_count=$displayed
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
Write-Host ("P2-D5-2 formal v2: PASS required={0} physical={1} unique={2} trueDrop={3} dropRate={4:P3}" -f `
    $required,$physical,$unique,$trueDrop,$dropRate)
