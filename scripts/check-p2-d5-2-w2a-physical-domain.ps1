[CmdletBinding()]
param(
    # run artifact (presentation_opportunity.physical_vblank_domain_shadow) か、
    # shadow object 単体のどちらでもよい。
    [Parameter(Mandatory=$true)][string]$Json,
    [string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# W2-A は shadow only である。fail-close は performance FAIL ではなく
# authority invalid として扱う。legacy formal scheduler / counters / shutdown /
# threshold は一切変更しない。
function Fail([string]$Reason,[string]$Message){throw ("{0}: {1}" -f $Reason,$Message)}
function Require($Object,[string]$Name,[string]$Reason='PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION'){
    if($null-eq$Object-or-not($Object.PSObject.Properties.Name-contains$Name)){
        Fail $Reason "W2-A shadow fieldがありません: $Name"
    }
    return $Object.$Name
}
function I64($Value){return [long]$Value}

# W2-A 固有 reason。observer の「取りこぼし」と「本当に長い物理interval」を
# 混同しないため、observer invalid を分解している。どちらにせよ authority
# invalid であり performance FAIL ではない。
$w2aReasons=@{
    'NONE'                                      = 'NONE'
    'PHYSICAL_VBLANK_MEASUREMENT_WINDOW_INVALID'= 'RUNTIME_AUTHORITY_OVERRIDE'
    'PHYSICAL_VBLANK_OBSERVER_UNAVAILABLE'      = 'PHYSICAL_VBLANK_OBSERVER_INVALID'
    'PHYSICAL_VBLANK_RING_OVERFLOW'             = 'PHYSICAL_VBLANK_OBSERVER_INVALID'
    'PHYSICAL_VBLANK_WAIT_FAILURE'              = 'PHYSICAL_VBLANK_OBSERVER_INVALID'
    'PHYSICAL_VBLANK_OBSERVER_STALL'            = 'PHYSICAL_VBLANK_OBSERVER_INVALID'
    'PHYSICAL_VBLANK_PREROLL_TIMEOUT'           = 'PHYSICAL_VBLANK_OBSERVER_INVALID'
    'PHYSICAL_VBLANK_PREROLL_NOT_BEFORE_START'  = 'PHYSICAL_VBLANK_OBSERVER_INVALID'
    'PHYSICAL_VBLANK_SEQUENCE_BREAK'            = 'PHYSICAL_VBLANK_SEQUENCE_BREAK'
    'OUTPUT_OR_MODE_CHANGED'                    = 'OUTPUT_OR_MODE_CHANGED'
    'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED'    = 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED'
}
# W0.5-A で legacy frameSwapped invariant と証明済み。W2-A shadow に出現しては
# ならない。
$retiredReasons=@('RENDER_SWAP_MISMATCH','RENDER_WITHOUT_SWAP','SWAP_WITHOUT_RENDER',
                  'RENDER_NOT_COMPLETED','SWAP_ORDINAL_MISMATCH','OPPORTUNITY_REGRESSION')
# W2-A は physical domain が正しく観測できたかだけを見る。verdict / performance
# semantics に属する field を shadow が持ち込んでいないことを固定する。
$forbiddenFields=@('drop_rate','performance_pass','performance_evaluated',
                   'formal_true_drop_count','true_drop_count','satisfied_intent_count',
                   'filled_physical_opportunity_count','physical_unfilled_count',
                   'displayed_unique_physical_count','effective_fps')

if(-not(Test-Path -LiteralPath $Json)){
    Fail 'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION' "shadow jsonがありません: $Json"
}
$raw=Get-Content -LiteralPath $Json -Raw -Encoding utf8|ConvertFrom-Json
$shadow=$raw
if($raw.PSObject.Properties.Name-contains'presentation_opportunity'){
    $shadow=Require $raw.presentation_opportunity 'physical_vblank_domain_shadow'
} elseif($raw.PSObject.Properties.Name-contains'physical_vblank_domain_shadow'){
    $shadow=$raw.physical_vblank_domain_shadow
}

foreach($name in $forbiddenFields){
    if($shadow.PSObject.Properties.Name-contains$name){
        Fail 'RUNTIME_AUTHORITY_OVERRIDE' "W2-A shadowにperformance semanticsのfieldがあります: $name"
    }
}

# --- shadow only であること ---
if(-not[bool](Require $shadow 'shadow_only' 'RUNTIME_AUTHORITY_OVERRIDE')){
    Fail 'RUNTIME_AUTHORITY_OVERRIDE' 'W2-Aはshadow onlyでなければなりません'
}
if([bool](Require $shadow 'formal_counter_authority_changed' 'RUNTIME_AUTHORITY_OVERRIDE')){
    Fail 'RUNTIME_AUTHORITY_OVERRIDE' 'legacy formal counter authorityが変更されています'
}
if([bool](Require $shadow 'performance_semantics_connected' 'RUNTIME_AUTHORITY_OVERRIDE')){
    Fail 'RUNTIME_AUTHORITY_OVERRIDE' 'W2-Aをperformance semanticsへ接続してはいけません'
}
# 窓のauthorityはmeasurement lifecycle、physical opportunityのauthorityは
# VBlank observer。collectorが独自にendを作ることを禁じる。
if([string](Require $shadow 'measurement_window_authority' 'RUNTIME_AUTHORITY_OVERRIDE')-ne'formal measurement lifecycle'){
    Fail 'RUNTIME_AUTHORITY_OVERRIDE' 'measurement窓のproducerがformal measurement lifecycleではありません'
}
if([string](Require $shadow 'physical_opportunity_authority' 'RUNTIME_AUTHORITY_OVERRIDE')-ne'window output physical VBlank observer'){
    Fail 'RUNTIME_AUTHORITY_OVERRIDE' 'physical opportunityのauthorityがVBlank observerではありません'
}
# domain member の定義は half-open。end と完全一致した VBlank は successor 側。
if([string](Require $shadow 'domain_relation')-ne'measurement_start_qpc <= vblank.qpc < measurement_end_qpc'){
    Fail 'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION' 'domain relationがhalf-openで宣言されていません'
}

# --- reason 語彙 ---
$reason=[string](Require $shadow 'shadow_authority_error')
if($retiredReasons-contains$reason){
    Fail 'RUNTIME_AUTHORITY_OVERRIDE' "retired済みのlegacy reasonが出現しました: $reason"
}
if(-not$w2aReasons.ContainsKey($reason)){
    Fail 'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION' "未知のW2-A authority errorです: $reason"
}
$canonical=[string](Require $shadow 'shadow_authority_canonical_reason')
if($canonical-ne$w2aReasons[$reason]){
    Fail 'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION' `
        "canonical reasonの射影が不正です: $reason -> $canonical (expected $($w2aReasons[$reason]))"
}

$valid=[bool](Require $shadow 'shadow_authority_valid')
if($valid-and$reason-ne'NONE'){
    Fail 'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION' 'validなのにerrorが設定されています'
}
if(-not$valid-and$reason-eq'NONE'){
    Fail 'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION' 'invalidなのにerrorがNONEです'
}

$start=I64 (Require $shadow 'measurement_start_qpc')
$end=I64 (Require $shadow 'measurement_end_qpc_exclusive')
$physical=I64 (Require $shadow 'physical_opportunity_count')
$required=I64 (Require $shadow 'required_intent_count')
$overhang=I64 (Require $shadow 'intent_overhang_count')
$surplus=I64 (Require $shadow 'intent_surplus_count')

if(-not$valid){
    # fail-close した run は W2-A の domain closure を評価しない。
    $result=[ordered]@{
        schema='mvm-p2-d5-2-w2a-physical-domain-1';status='AUTHORITY_INVALID'
        stage='P2-D5-2-W2-A';shadow_only=$true
        shadow_authority_valid=$false;shadow_authority_error=$reason
        shadow_authority_canonical_reason=$canonical
        domain_evaluated=$false
    }
    if(-not[string]::IsNullOrWhiteSpace($Output)){
        $result|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $Output -Encoding utf8
    }
    Write-Host "P2-D5-2 W2-A physical domain: AUTHORITY_INVALID ($reason -> $canonical)"
    exit 0
}

# --- Layer 1B fail-closed contract (valid を主張した run のみ) ---
if([string](Require $shadow 'sequence_status' 'PHYSICAL_VBLANK_SEQUENCE_BREAK')-ne'OK'){
    Fail 'PHYSICAL_VBLANK_SEQUENCE_BREAK' 'VBlank sequenceが破れているのにvalidを主張しています'
}
foreach($zeroField in @('long_interval_count','short_interval_count','ring_overflow_count',
                        'wait_failure_count')){
    if((I64 (Require $shadow $zeroField 'PHYSICAL_VBLANK_OBSERVER_STALL'))-ne0){
        Fail 'PHYSICAL_VBLANK_OBSERVER_STALL' "$zeroField が0ではないのにvalidを主張しています"
    }
}
if(-not[bool](Require $shadow 'cumulative_consistent' 'PHYSICAL_VBLANK_OBSERVER_STALL')){
    Fail 'PHYSICAL_VBLANK_OBSERVER_STALL' 'cumulative consistencyが成立していません'
}
if(-not[bool](Require $shadow 'output_stable' 'OUTPUT_OR_MODE_CHANGED')){
    Fail 'OUTPUT_OR_MODE_CHANGED' 'adapter/output/HMONITOR/refresh rationalが変化しました'
}
if(-not[bool](Require $shadow 'boundary_bracketed' 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED')){
    Fail 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED' 'measurement両端がbracketされていません'
}
if(-not[bool](Require $shadow 'predecessor_valid' 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED')){
    Fail 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED' 'predecessorがありません'
}
if(-not[bool](Require $shadow 'successor_valid' 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED')){
    Fail 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED' 'successorがありません'
}

if($start-le0-or$end-le$start){
    Fail 'RUNTIME_AUTHORITY_OVERRIDE' 'measurement窓が確定していません'
}

# --- W2-A.1 lower boundary preroll ---
# 下側bracketがraceではなく構造的に保証されたこと。確認する不変量は
# ordinalが0かどうかではなく preroll sample.qpc < measurement_start_qpc である。
if(-not[bool](Require $shadow 'prestart_vblank_preroll_completed' 'PHYSICAL_VBLANK_PREROLL_TIMEOUT')){
    Fail 'PHYSICAL_VBLANK_PREROLL_TIMEOUT' 'measurement開始前のphysical VBlank prerollが成立していません'
}
if([bool](Require $shadow 'prestart_vblank_preroll_timeout' 'PHYSICAL_VBLANK_PREROLL_TIMEOUT')){
    Fail 'PHYSICAL_VBLANK_PREROLL_TIMEOUT' 'preroll timeoutなのにvalidを主張しています'
}
$prerollQpc=I64 (Require $shadow 'prestart_vblank_sample_qpc' 'PHYSICAL_VBLANK_PREROLL_NOT_BEFORE_START')
if($prerollQpc-le0-or$prerollQpc-ge$start){
    Fail 'PHYSICAL_VBLANK_PREROLL_NOT_BEFORE_START' `
        "preroll sampleがmeasurement_start_qpcより前ではありません ($prerollQpc >= $start)"
}
$prerollOrdinal=I64 (Require $shadow 'prestart_vblank_sample_ordinal')
$prerollWait=I64 (Require $shadow 'prestart_wait_elapsed_qpc')
if($prerollWait-lt0){Fail 'PHYSICAL_VBLANK_PREROLL_TIMEOUT' 'preroll wait時間が負です'}

$predOrdinal=I64 (Require $shadow 'predecessor_ordinal')
$predQpc=I64 (Require $shadow 'predecessor_qpc')
$succOrdinal=I64 (Require $shadow 'successor_ordinal')
$succQpc=I64 (Require $shadow 'successor_qpc')
$originOrdinal=I64 (Require $shadow 'origin_ordinal')
$originQpc=I64 (Require $shadow 'origin_qpc')
$lastOrdinal=I64 (Require $shadow 'last_ordinal')
$lastQpc=I64 (Require $shadow 'last_qpc')

if($physical-lt0){Fail 'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION' 'physical_opportunity_countが負です'}

# B1/B2: predecessor / successor は domain member ではなく boundary authority。
if($predQpc-ge$start){
    Fail 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED' 'B1違反: predecessor.qpc < measurement_start_qpc が成立しません'
}
if($succQpc-lt$end){
    Fail 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED' 'B2違反: successor.qpc >= measurement_end_qpc が成立しません'
}

if($physical-gt0){
    # D1: domain は連続した physical_vblank_ordinal の区間である。
    if($physical-ne$lastOrdinal-$originOrdinal+1){
        Fail 'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION' 'D1違反: count != last_ordinal - origin_ordinal + 1'
    }
    # D2/D3: bracket は domain の直前・直後でなければならない。
    if($predOrdinal+1-ne$originOrdinal){
        Fail 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED' 'D2違反: predecessorがoriginの直前ではありません'
    }
    if($succOrdinal-ne$lastOrdinal+1){
        Fail 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED' 'D3違反: successorがlastの直後ではありません'
    }
    # D4: domain member は half-open 区間に収まる。
    if($originQpc-lt$start-or$lastQpc-ge$end-or$lastQpc-lt$originQpc){
        Fail 'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION' 'D4違反: domain memberが[start,end)に収まっていません'
    }
} else {
    # 窓が2本のVBlankの間に収まる場合。domain は空だが bracket は成立する。
    if($originOrdinal-ne-1-or$lastOrdinal-ne-1){
        Fail 'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION' 'D5違反: 空domainなのにorigin/lastが設定されています'
    }
    if($succOrdinal-ne$predOrdinal+1){
        Fail 'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED' 'D6違反: 空domainのsuccessorがpredecessorの直後ではありません'
    }
}

# X: intent と physical の差。shadow 出力のみで、verdict には接続しない。
# required_intent_count と physical_opportunity_count の一致は要求しない。
if($overhang-ne[Math]::Max($required-$physical,0)){
    Fail 'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION' 'X1違反: intent_overhangの定義と一致しません'
}
if($surplus-ne[Math]::Max($physical-$required,0)){
    Fail 'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION' 'X2違反: intent_surplusの定義と一致しません'
}
if($overhang-ne0-and$surplus-ne0){
    Fail 'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION' 'X3違反: overhangとsurplusが同時に非0です'
}

$result=[ordered]@{
    schema='mvm-p2-d5-2-w2a-physical-domain-1';status='PASS'
    stage='P2-D5-2-W2-A';shadow_only=$true
    shadow_authority_valid=$true;shadow_authority_error='NONE'
    shadow_authority_canonical_reason='NONE'
    domain_evaluated=$true
    # W2-A は physical domain を exact に観測できたかだけを判定する。
    performance_semantics_connected=$false
    measurement_start_qpc=$start
    measurement_end_qpc_exclusive=$end
    predecessor_ordinal=$predOrdinal
    successor_ordinal=$succOrdinal
    origin_ordinal=$originOrdinal
    last_ordinal=$lastOrdinal
    physical_opportunity_count=$physical
    required_intent_count=$required
    intent_overhang_count=$overhang
    intent_surplus_count=$surplus
    boundary_bracketed=$true
    prestart_vblank_preroll_completed=$true
    prestart_vblank_preroll_timeout=$false
    prestart_vblank_sample_ordinal=$prerollOrdinal
    prestart_vblank_sample_qpc=$prerollQpc
    prestart_wait_elapsed_qpc=$prerollWait
    retired_reasons_absent=$true
}
if(-not[string]::IsNullOrWhiteSpace($Output)){
    $result|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $Output -Encoding utf8
}
Write-Host ("P2-D5-2 W2-A physical domain: PASS origin={0} last={1} physical={2} required={3} overhang={4} surplus={5}" -f `
    $originOrdinal,$lastOrdinal,$physical,$required,$overhang,$surplus)
