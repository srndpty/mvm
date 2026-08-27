[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('Good','GoodSuccessorAfterEnd','GoodEmptyDomain','GoodPhysicalOverhang',
                 'GoodPhysicalSurplus','AuthorityInvalidStall','AuthorityInvalidWindow',
                 'NegativeNotShadowOnly','NegativeCounterAuthorityChanged',
                 'NegativePerformanceConnected','NegativeWindowAuthority',
                 'NegativeOpportunityAuthority','NegativeDomainRelation',
                 'NegativePerformanceField','NegativeRetiredReason','NegativeUnknownReason',
                 'NegativeCanonicalMapping','NegativeValidWithError','NegativeInvalidWithoutError',
                 'NegativeSequenceStatus','NegativeLongInterval','NegativeShortInterval',
                 'NegativeRingOverflow','NegativeWaitFailure','NegativeCumulative',
                 'NegativeOutputChanged','NegativeBoundaryNotBracketed',
                 'NegativePredecessorInDomain','NegativeSuccessorBeforeEnd',
                 'NegativeIdentityD1','NegativeIdentityD2','NegativeIdentityD3',
                 'NegativeIdentityD4','NegativeEmptyDomainOrigin','NegativeEmptyDomainBracket',
                 'NegativeIdentityX1','NegativeIdentityX2','NegativeIdentityX3',
                 'AuthorityInvalidPrerollTimeout','NegativePrerollNotCompleted',
                 'NegativePrerollTimeoutFlag','NegativePrerollNotBeforeStart',
                 'NegativePrerollAtStart','GoodPrerollOrdinalNonZero')]
    [string]$Case,
    [Parameter(Mandatory=$true)][string]$Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(Test-Path -LiteralPath $Directory){Remove-Item -LiteralPath $Directory -Recurse -Force}
New-Item -ItemType Directory -Path $Directory|Out-Null

# 基準: 60Hz / 10 MHz QPC。measurement窓は既存formal measurement lifecycleが
# 確定したexact QPCとする。
#   domain      V1000..V4599 (3600本)
#   predecessor V999   (qpc < measurement_start_qpc)
#   successor   V4600  (qpc == measurement_end_qpc。half-openなのでsuccessor側)
$period=166666
$start=10000000
$originOrdinal=1000;$originQpc=$start
$physical=3600
$lastOrdinal=$originOrdinal+$physical-1
$lastQpc=$originQpc+($physical-1)*$period
$end=$lastQpc+$period
$predOrdinal=$originOrdinal-1;$predQpc=$originQpc-$period
$succOrdinal=$lastOrdinal+1;$succQpc=$end
$required=3600
$sequence='OK';$longInterval=0;$shortInterval=0;$ringOverflow=0;$waitFailure=0
$cumulative=$true;$outputStable=$true;$bracketed=$true
$predValid=$true;$succValid=$true
$valid=$true;$reason='NONE';$canonical='NONE'
# W2-A.1。下側bracketの witness。ordinalではなくqpcが不変量。
$prerollCompleted=$true;$prerollTimeout=$false
$prerollOrdinal=$predOrdinal;$prerollQpc=$predQpc;$prerollWait=12345
$shadowOnly=$true;$counterAuthorityChanged=$false;$performanceConnected=$false
$windowAuthority='formal measurement lifecycle'
$opportunityAuthority='window output physical VBlank observer'
$domainRelation='measurement_start_qpc <= vblank.qpc < measurement_end_qpc'
$extra=[ordered]@{}

switch($Case){
    'GoodSuccessorAfterEnd'{
        # successor.qpc > end。bracket条件は >= なので成立する。
        $end=$lastQpc+$period-1000;$succQpc=$lastQpc+$period
    }
    'GoodEmptyDomain'{
        # 窓が2本のVBlankの間に収まる。domainは空だがbracketは成立する。
        $physical=0;$originOrdinal=-1;$originQpc=0;$lastOrdinal=-1;$lastQpc=0
        $start=$predQpc+10;$end=$predQpc+20
        $succOrdinal=$predOrdinal+1;$succQpc=$predQpc+$period
        $required=0
    }
    'GoodPhysicalOverhang'{
        # required=3600 / physical=3597。W2-Aではこれを 3 opportunities missing と
        # 判定してはならない。
        $physical=3597;$lastOrdinal=$originOrdinal+$physical-1
        $lastQpc=$originQpc+($physical-1)*$period;$end=$lastQpc+$period
        $succOrdinal=$lastOrdinal+1;$succQpc=$end
    }
    'GoodPhysicalSurplus'{
        $physical=3603;$lastOrdinal=$originOrdinal+$physical-1
        $lastQpc=$originQpc+($physical-1)*$period;$end=$lastQpc+$period
        $succOrdinal=$lastOrdinal+1;$succQpc=$end
    }
    'AuthorityInvalidStall'{
        # observerが起きられなかったのか実displayが長かったのかは区別しないが、
        # どちらにせよauthority invalidであってperformance FAILではない。
        $valid=$false;$reason='PHYSICAL_VBLANK_OBSERVER_STALL'
        $canonical='PHYSICAL_VBLANK_OBSERVER_INVALID';$longInterval=1
    }
    'AuthorityInvalidWindow'{
        $valid=$false;$reason='PHYSICAL_VBLANK_MEASUREMENT_WINDOW_INVALID'
        $canonical='RUNTIME_AUTHORITY_OVERRIDE';$start=0;$end=0
    }
    'NegativeNotShadowOnly'          {$shadowOnly=$false}
    'NegativeCounterAuthorityChanged'{$counterAuthorityChanged=$true}
    'NegativePerformanceConnected'   {$performanceConnected=$true}
    'NegativeWindowAuthority'        {$windowAuthority='physical vblank collector'}
    'NegativeOpportunityAuthority'   {$opportunityAuthority='DwmGetCompositionTimingInfo'}
    'NegativeDomainRelation'         {$domainRelation='measurement_start_qpc <= vblank.qpc <= measurement_end_qpc'}
    'NegativePerformanceField'       {$extra['drop_rate']=0.0}
    'NegativeRetiredReason'          {$valid=$false;$reason='OPPORTUNITY_REGRESSION';$canonical='OUTPUT_OR_MODE_CHANGED'}
    'NegativeUnknownReason'          {$valid=$false;$reason='PHYSICAL_VBLANK_MYSTERY';$canonical='PHYSICAL_VBLANK_OBSERVER_INVALID'}
    'NegativeCanonicalMapping'       {$valid=$false;$reason='PHYSICAL_VBLANK_OBSERVER_STALL';$canonical='PHYSICAL_VBLANK_SEQUENCE_BREAK'}
    'NegativeValidWithError'         {$reason='PHYSICAL_VBLANK_OBSERVER_STALL';$canonical='PHYSICAL_VBLANK_OBSERVER_INVALID'}
    'NegativeInvalidWithoutError'    {$valid=$false}
    'NegativeSequenceStatus'         {$sequence='ORDINAL_GAP'}
    'NegativeLongInterval'           {$longInterval=1}
    'NegativeShortInterval'          {$shortInterval=1}
    'NegativeRingOverflow'           {$ringOverflow=1}
    'NegativeWaitFailure'            {$waitFailure=1}
    'NegativeCumulative'             {$cumulative=$false}
    'NegativeOutputChanged'          {$outputStable=$false}
    'NegativeBoundaryNotBracketed'   {$bracketed=$false}
    # predecessor / successor は domain member ではなく boundary authority である。
    'NegativePredecessorInDomain'    {$predQpc=$originQpc}
    'NegativeSuccessorBeforeEnd'     {$succQpc=$end-1}
    'NegativeIdentityD1'             {$physical=$physical+1}
    'NegativeIdentityD2'             {$predOrdinal=$originOrdinal-2}
    'NegativeIdentityD3'             {$succOrdinal=$lastOrdinal+2}
    'NegativeIdentityD4'             {$lastQpc=$end}
    'NegativeEmptyDomainOrigin'{
        $physical=0;$start=$predQpc+10;$end=$predQpc+20
        $succOrdinal=$predOrdinal+1;$succQpc=$predQpc+$period;$required=0
        # 空domainなのにorigin/lastが残っている。
    }
    'NegativeEmptyDomainBracket'{
        $physical=0;$originOrdinal=-1;$originQpc=0;$lastOrdinal=-1;$lastQpc=0
        $start=$predQpc+10;$end=$predQpc+20
        $succOrdinal=$predOrdinal+3;$succQpc=$predQpc+$period;$required=0
    }
    'NegativeIdentityX1'             {$required=3610}
    'NegativeIdentityX2'             {$required=3590}
    'NegativeIdentityX3'             {$required=3600}
    'AuthorityInvalidPrerollTimeout'{
        $valid=$false;$reason='PHYSICAL_VBLANK_PREROLL_TIMEOUT'
        $canonical='PHYSICAL_VBLANK_OBSERVER_INVALID'
        $prerollCompleted=$false;$prerollTimeout=$true;$prerollQpc=0;$prerollOrdinal=-1
    }
    # validを主張しながらprerollが成立していないartifactは受理しない。
    'NegativePrerollNotCompleted'    {$prerollCompleted=$false}
    'NegativePrerollTimeoutFlag'     {$prerollTimeout=$true}
    'NegativePrerollNotBeforeStart'  {$prerollQpc=$originQpc+$period}
    # half-open。start と完全一致した preroll sample は下側witnessにならない。
    'NegativePrerollAtStart'         {$prerollQpc=$start}
    # 確認する不変量はordinalが0かどうかではない。
    'GoodPrerollOrdinalNonZero'      {$prerollOrdinal=$predOrdinal;$prerollQpc=$predQpc-$period}
}

$overhang=[Math]::Max($required-$physical,0)
$surplus=[Math]::Max($physical-$required,0)
switch($Case){
    'NegativeIdentityX1'{$overhang=0}                 # 定義と食い違わせる
    'NegativeIdentityX2'{$surplus=0}
    'NegativeIdentityX3'{$overhang=1;$surplus=1}      # 同時に非0
}

$shadow=[ordered]@{
    shadow_only=$shadowOnly
    formal_counter_authority_changed=$counterAuthorityChanged
    performance_semantics_connected=$performanceConnected
    measurement_window_authority=$windowAuthority
    physical_opportunity_authority=$opportunityAuthority
    domain_relation=$domainRelation
    measurement_start_qpc=$start
    measurement_end_qpc_exclusive=$end
    prestart_vblank_preroll_completed=$prerollCompleted
    prestart_vblank_preroll_timeout=$prerollTimeout
    prestart_vblank_sample_ordinal=$prerollOrdinal
    prestart_vblank_sample_qpc=$prerollQpc
    prestart_wait_elapsed_qpc=$prerollWait
    predecessor_valid=$predValid
    predecessor_ordinal=$predOrdinal
    predecessor_qpc=$predQpc
    successor_valid=$succValid
    successor_ordinal=$succOrdinal
    successor_qpc=$succQpc
    origin_ordinal=$originOrdinal
    origin_qpc=$originQpc
    last_ordinal=$lastOrdinal
    last_qpc=$lastQpc
    physical_opportunity_count=$physical
    sequence_status=$sequence
    long_interval_count=$longInterval
    short_interval_count=$shortInterval
    ring_overflow_count=$ringOverflow
    wait_failure_count=$waitFailure
    cumulative_consistent=$cumulative
    output_stable=$outputStable
    boundary_bracketed=$bracketed
    shadow_authority_valid=$valid
    shadow_authority_error=$reason
    shadow_authority_canonical_reason=$canonical
    required_intent_count=$required
    intent_overhang_count=$overhang
    intent_surplus_count=$surplus
}
foreach($key in $extra.Keys){$shadow[$key]=$extra[$key]}

# 実artifactと同じ入れ子で渡す。
$artifact=[ordered]@{
    presentation_opportunity=[ordered]@{
        enabled=$true
        physical_vblank_domain_shadow=$shadow
    }
}
$jsonPath=Join-Path $Directory 'run.json'
$artifact|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $jsonPath -Encoding utf8

$output=Join-Path $Directory 'proof.json'
$checkerLog=Join-Path $Directory 'checker.txt'
$failed=$false
try{
    & pwsh -NoProfile -File $Checker -Json $jsonPath -Output $output *> $checkerLog
    if($LASTEXITCODE-ne0){$failed=$true}
}catch{$failed=$true}
$checkerText=if(Test-Path -LiteralPath $checkerLog){Get-Content -LiteralPath $checkerLog -Raw}else{''}

$expectedReason=switch($Case){
    'NegativeNotShadowOnly'          {'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativeCounterAuthorityChanged'{'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativePerformanceConnected'   {'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativeWindowAuthority'        {'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativeOpportunityAuthority'   {'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativePerformanceField'       {'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativeRetiredReason'          {'RUNTIME_AUTHORITY_OVERRIDE'}
    'NegativeDomainRelation'         {'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION'}
    'NegativeUnknownReason'          {'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION'}
    'NegativeCanonicalMapping'       {'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION'}
    'NegativeValidWithError'         {'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION'}
    'NegativeInvalidWithoutError'    {'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION'}
    'NegativeSequenceStatus'         {'PHYSICAL_VBLANK_SEQUENCE_BREAK'}
    'NegativeLongInterval'           {'PHYSICAL_VBLANK_OBSERVER_STALL'}
    'NegativeShortInterval'          {'PHYSICAL_VBLANK_OBSERVER_STALL'}
    'NegativeRingOverflow'           {'PHYSICAL_VBLANK_OBSERVER_STALL'}
    'NegativeWaitFailure'            {'PHYSICAL_VBLANK_OBSERVER_STALL'}
    'NegativeCumulative'             {'PHYSICAL_VBLANK_OBSERVER_STALL'}
    'NegativeOutputChanged'          {'OUTPUT_OR_MODE_CHANGED'}
    'NegativeBoundaryNotBracketed'   {'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED'}
    'NegativePredecessorInDomain'    {'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED'}
    'NegativeSuccessorBeforeEnd'     {'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED'}
    'NegativeIdentityD1'             {'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION'}
    'NegativeIdentityD2'             {'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED'}
    'NegativeIdentityD3'             {'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED'}
    'NegativeIdentityD4'             {'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION'}
    'NegativeEmptyDomainOrigin'      {'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION'}
    'NegativeEmptyDomainBracket'     {'PHYSICAL_VBLANK_BOUNDARY_NOT_BRACKETED'}
    'NegativeIdentityX1'             {'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION'}
    'NegativeIdentityX2'             {'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION'}
    'NegativeIdentityX3'             {'PHYSICAL_VBLANK_DOMAIN_IDENTITY_VIOLATION'}
    'NegativePrerollNotCompleted'    {'PHYSICAL_VBLANK_PREROLL_TIMEOUT'}
    'NegativePrerollTimeoutFlag'     {'PHYSICAL_VBLANK_PREROLL_TIMEOUT'}
    'NegativePrerollNotBeforeStart'  {'PHYSICAL_VBLANK_PREROLL_NOT_BEFORE_START'}
    'NegativePrerollAtStart'         {'PHYSICAL_VBLANK_PREROLL_NOT_BEFORE_START'}
    default                          {$null}
}

if($null-ne$expectedReason){
    if(-not$failed){throw "negative caseが違反を検出できませんでした: $Case"}
    if($checkerText-notmatch [regex]::Escape($expectedReason)){
        throw "期待した fail-close reason が出ていません: expected=$expectedReason`n$checkerText"
    }
    Write-Host "P2-D5-2 W2-A physical domain contract: PASS ($Case -> $expectedReason)"
    exit 0
}
if($failed){throw "正の契約が失敗しました: $Case`n$checkerText"}
$proof=Get-Content -LiteralPath $output -Raw -Encoding utf8|ConvertFrom-Json
if($Case-like'AuthorityInvalid*'){
    if([string]$proof.status-ne'AUTHORITY_INVALID'){throw "statusが不正です: $($proof.status)"}
    if([bool]$proof.domain_evaluated){throw 'authority invalidなのにdomainを評価しました'}
    if([string]$proof.shadow_authority_error-ne$reason){throw 'reasonが伝播していません'}
    if([string]$proof.shadow_authority_canonical_reason-ne$canonical){
        throw 'canonical reasonが伝播していません'
    }
    Write-Host "P2-D5-2 W2-A physical domain contract: PASS ($Case -> AUTHORITY_INVALID)"
    exit 0
}
if([string]$proof.status-ne'PASS'){throw "statusがPASSではありません: $($proof.status)"}
if([bool]$proof.performance_semantics_connected){throw 'performance semanticsへ接続されています'}
if([long]$proof.physical_opportunity_count-ne$physical){throw 'physical countが一致しません'}
if($Case-eq'GoodPhysicalOverhang'){
    # required と physical の差は記録するが verdict にしない。
    if([long]$proof.intent_overhang_count-ne3){throw 'overhangが3ではありません'}
    if([long]$proof.intent_surplus_count-ne0){throw 'surplusが0ではありません'}
}
if($Case-eq'GoodPhysicalSurplus'){
    if([long]$proof.intent_surplus_count-ne3){throw 'surplusが3ではありません'}
    if([long]$proof.intent_overhang_count-ne0){throw 'overhangが0ではありません'}
}
if($Case-eq'GoodEmptyDomain'){
    if([long]$proof.physical_opportunity_count-ne0){throw '空domainになっていません'}
    if(-not[bool]$proof.boundary_bracketed){throw '空domainでbracketが成立していません'}
}
if($Case-eq'GoodPrerollOrdinalNonZero'){
    if([long]$proof.prestart_vblank_sample_qpc-ge[long]$proof.measurement_start_qpc){
        throw 'preroll sampleがmeasurement_start_qpcより前ではありません'
    }
}
if($Case-eq'Good'){
    # half-open。end と完全一致した VBlank は successor 側に置かれる。
    if([long]$proof.successor_ordinal-ne[long]$proof.last_ordinal+1){
        throw 'successorがlastの直後ではありません'
    }
    $span=[long]$proof.last_ordinal-[long]$proof.origin_ordinal+1
    if([long]$proof.physical_opportunity_count-ne$span){throw 'ordinal算術が閉じていません'}
}
Write-Host "P2-D5-2 W2-A physical domain contract: PASS ($Case)"
