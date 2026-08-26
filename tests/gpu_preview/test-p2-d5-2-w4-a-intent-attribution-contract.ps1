[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'GoodExactPartition','GoodNoPrimaryDecision','GoodDownstreamLossBuckets',
        'NegativeRequiredIntentMissingFromAllBuckets','NegativeRequiredIntentInTwoBuckets',
        'NegativeMultiplePrimaryDecision','NegativeDuplicateCallbackCountedAsDrop',
        'NegativeOutsideRequiredCountedAsDrop','NegativeMissingSetMutation',
        'NegativeSatisfiedSetMutation','NegativeAggregateOnlyForgery')][string]$Case,
    [Parameter(Mandatory=$true)][string]$Core,
    [Parameter(Mandatory=$true)][string]$SharedReplay
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
. $Core
. $SharedReplay

# 期待値は実装の式を呼ばず fixture 側で決める。
function Decision([string]$Ordinal,[bool]$Eligible=$true,[bool]$Duplicate=$false,
                  [bool]$Membership=$true,[bool]$MembershipExact=$true,
                  [string]$Scope='CURRENT_MEASUREMENT'){
    return [pscustomobject][ordered]@{
        intent_ordinal=$Ordinal;intent_scope=$Scope
        required_current_membership=$Membership
        required_current_membership_exact=$MembershipExact
        duplicate_callback=$Duplicate
        formal_transport_eligible=$Eligible
    }
}
function NativePresent([string]$Ordinal){
    return [pscustomobject][ordered]@{intent_ordinal=$Ordinal;intent_ordinal_valid=$true}
}
function Mapping([string]$Ordinal,[bool]$InDomain=$true){
    return [pscustomobject][ordered]@{
        intent_ordinal=$Ordinal;intent_scope='CURRENT_MEASUREMENT'
        mapping_exact=$true;in_measurement_physical_domain=$InDomain
    }
}

# required {0,1,2,3}: 0 と 2 は satisfied、1 と 3 は decision なし。
$required=@('0','1','2','3')
$decisions=@((Decision '0'),(Decision '2'))
$natives=@((NativePresent '0'),(NativePresent '2'))
$mappings=@((Mapping '0'),(Mapping '2'))
$expected=@{A=2;C=0;D=0;E=0;F=0;G=2}

switch($Case){
    'GoodNoPrimaryDecision'{
        $decisions=@();$natives=@();$mappings=@()
        $expected=@{A=4;C=0;D=0;E=0;F=0;G=0}
    }
    'GoodDownstreamLossBuckets'{
        # C/D/E/F を 1 件ずつ作り、partition が排他的に効くことを確認する。
        $required=@('0','1','2','3','4')
        $decisions=@((Decision '0' $false),(Decision '1'),(Decision '2'),(Decision '3'),(Decision '4'))
        $natives=@((NativePresent '2'),(NativePresent '3'),(NativePresent '4'))
        $mappings=@((Mapping '3' $false),(Mapping '4' $true))
        $expected=@{A=0;C=1;D=1;E=1;F=1;G=1}
    }
    'NegativeMultiplePrimaryDecision'{
        # 同一 current intent の primary decision が 2 件。bucket ではなく authority INVALID。
        $decisions=@((Decision '0'),(Decision '0'),(Decision '2'))
    }
    'NegativeDuplicateCallbackCountedAsDrop'{
        # duplicate callback は付帯 record。satisfied + suppressed の 2 intent にしない。
        $decisions=@((Decision '0'),(Decision '0' $false $true),(Decision '2'))
    }
    'NegativeOutsideRequiredCountedAsDrop'{
        # outside-required decision は母集団に入れない。
        $decisions=@((Decision '0'),(Decision '99' $true $false $false),(Decision '2'))
    }
}

$actual=Invoke-MvmW4IntentAttribution -RequiredIntentOrdinals $required `
    -DecisionRecords $decisions -NativePresentRecords $natives -FormalMappingRecords $mappings

if($Case-eq'NegativeMultiplePrimaryDecision'){
    if([bool]$actual.attribution_exact){throw 'multiple primary decisionをfail-closeしていません'}
    if('REQUIRED_INTENT_PRIMARY_DECISION_DUPLICATE'-notin@($actual.blockers)){
        throw "multiple primary decisionのblockerが出ません: $(@($actual.blockers)-join',')"
    }
    # performance bucket へ落としていないこと。
    if([int64]$actual.buckets.A_NO_PRIMARY_SCHEDULER_DECISION-ne2){
        throw 'multiple primaryをdrop bucketへ混ぜています'
    }
    Write-Output "W4-A attribution contract $Case : PASS";exit 0
}
if($Case-eq'NegativeDuplicateCallbackCountedAsDrop'){
    if(-not[bool]$actual.attribution_exact){throw "duplicate callbackでfail-closeしました: $(@($actual.blockers)-join',')"}
    if([int64]$actual.buckets.G_SATISFIED_IN_DOMAIN-ne2){throw 'duplicate callbackでsatisfiedが変化しました'}
    if([int64]$actual.bucket_sum-ne4){throw 'duplicate callbackを母集団へ混ぜています'}
    if([int64]$actual.duplicate_callback_suppressed_count-ne1){throw 'duplicate callbackをdiagnosticへ分離していません'}
    Write-Output "W4-A attribution contract $Case : PASS";exit 0
}
if($Case-eq'NegativeOutsideRequiredCountedAsDrop'){
    if(-not[bool]$actual.attribution_exact){throw "outside-requiredでfail-closeしました: $(@($actual.blockers)-join',')"}
    if([int64]$actual.bucket_sum-ne4){throw 'outside-required decisionを母集団へ混ぜています'}
    if([int64]$actual.outside_required_decision_count-ne1){throw 'outside-requiredをdiagnosticへ分離していません'}
    Write-Output "W4-A attribution contract $Case : PASS";exit 0
}

# --- artifact 改変系。sealed records からの再構築と一致しないことを固定する ---
$mutations=@{
    'NegativeRequiredIntentMissingFromAllBuckets'={param($m)$m.buckets.A_NO_PRIMARY_SCHEDULER_DECISION-=1}
    'NegativeRequiredIntentInTwoBuckets'={param($m)$m.buckets.G_SATISFIED_IN_DOMAIN+=1}
    'NegativeMissingSetMutation'={param($m)$m.missing_primary_decision_ordinals=@('0')}
    'NegativeSatisfiedSetMutation'={param($m)$m.satisfied_intent_ordinals=@('1','3')}
    'NegativeAggregateOnlyForgery'={param($m)
        $m.satisfied_intent_count=4
        $m.unsatisfied_intent_count=0}
}
if($mutations.ContainsKey($Case)){
    $mutated=$actual|ConvertTo-Json -Depth 20|ConvertFrom-Json
    & $mutations[$Case] $mutated
    $rejected=$false
    try{Assert-MvmW4Proof -Expected $actual -Actual $mutated}catch{$rejected=$true}
    if(-not$rejected){throw "$Case が受理されました"}
    Write-Output "W4-A attribution contract $Case : PASS";exit 0
}

# --- Good 系 ---
if(-not[bool]$actual.attribution_exact){throw "$Case が不成立です: $(@($actual.blockers)-join',')"}
foreach($name in @('A_NO_PRIMARY_SCHEDULER_DECISION','C_NO_FORMAL_TRANSPORT_ELIGIBLE_PRIMARY',
    'D_NO_NATIVE_PRESENT','E_NO_EXACT_FORMAL_PRESENTED','F_FORMAL_PRESENTED_OUTSIDE_DOMAIN',
    'G_SATISFIED_IN_DOMAIN')){
    $key=$name.Substring(0,1)
    if([int64]$actual.buckets.$name-ne[int64]$expected[$key]){
        throw "$Case の$name が期待値と一致しません: $($actual.buckets.$name) (expected $($expected[$key]))"
    }
}
# 1 intent identity につきちょうど 1 bucket。
if([int64]$actual.bucket_sum-ne$required.Count){throw "$Case のbucket sumがrequired countと一致しません"}
if(-not[bool]$actual.partition_exhaustive){throw "$Case のpartitionが網羅的ではありません"}
if(([int64]$actual.satisfied_intent_count+[int64]$actual.unsatisfied_intent_count)-ne$required.Count){
    throw "$Case のsatisfied + unsatisfied がrequired countと一致しません"
}
# W4-A は attribution の段であり原因判定の段ではない。
if([int64]$actual.multiple_primary_decision_count-ne0){throw "$Case でmultiple primaryが出ています"}
$ordinalSets=@('required_intent_ordinals','primary_decision_ordinals',
    'missing_primary_decision_ordinals','satisfied_intent_ordinals')
foreach($setName in $ordinalSets){
    # core は ordered dictionary を返す。PSObject.Properties では key を列挙できない。
    if(-not $actual.Contains($setName)){throw "$Case に$setName がありません"}
}
if(@($actual.required_intent_ordinals).Count-ne$required.Count){throw "$Case のrequired ordinal setが不正です"}
if(@($actual.missing_primary_decision_ordinals).Count-ne[int64]$actual.buckets.A_NO_PRIMARY_SCHEDULER_DECISION){
    throw "$Case のmissing setがbucket Aと一致しません"
}
if(@($actual.satisfied_intent_ordinals).Count-ne[int64]$actual.buckets.G_SATISFIED_IN_DOMAIN){
    throw "$Case のsatisfied setがbucket Gと一致しません"
}
Write-Output "W4-A attribution contract $Case : PASS"
