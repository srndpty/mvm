[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$InputJson,
    [string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
# S2-f3: fail-close時にもdiagnostic artifactを書く。
# 判定条件そのものは変更しない。violation codeは既存のfail siteへ1:1で割り当てる。
# これがないとcheckerのusage crashとintended semantic rejectionを、呼び出し側が
# 非0 exitだけで区別できず、negativeがvacuous PASSになる。
function Write-Violation([string]$Code,[string]$Message){
    if($Output){
        [ordered]@{
            schema='mvm-p2-d5-2-w2-c11-physical-mapping-support-check-1'
            stage='P2-D5-2-W2-C1.1';pass=$false
            violation=$Code;detail=$Message
            verdict='PHYSICAL_MAPPING_SUPPORT_ENVELOPE_NOT_CLOSED'
        }|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $Output -Encoding utf8
    }
}
function Fail([string]$Code,[string]$Message){
    Write-Violation $Code $Message
    throw $Message
}
function Need($Object,[string]$Name){
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){
        Fail 'REQUIRED_FIELD_MISSING' "必須fieldがありません: $Name"
    }
    return $Object.$Name
}
if(-not(Test-Path -LiteralPath $InputJson)){Fail 'INPUT_MISSING' "W2-C1.1 inputがありません: $InputJson"}
$app=Get-Content -LiteralPath $InputJson -Raw -Encoding utf8|ConvertFrom-Json
$opportunity=Need $app 'presentation_opportunity'
$physical=Need $opportunity 'physical_vblank'
$support=Need $opportunity 'physical_mapping_support_envelope_shadow'
$samples=@(Need $physical 'samples')
if([string](Need $support 'schema')-ne'mvm-p2-d5-2-w2-c11-physical-mapping-support-envelope-1'){
    Fail 'SUPPORT_SCHEMA_INVALID' 'W2-C1.1 support schemaが不正です'
}
if(-not[bool](Need $support 'shadow_only')-or[bool](Need $support 'performance_semantics_connected')-or
   [bool](Need $support 'intent_satisfaction_connected')){
    Fail 'SHADOW_ISOLATION_INVALID' 'W2-C1.1 shadow isolationが不正です'
}
$captureBegin=[int64](Need $support 'capture_begin_qpc')
$captureClose=[int64](Need $support 'capture_close_qpc')
$postrollBoundary=[int64](Need $support 'postroll_boundary_qpc')
$predecessorOrdinal=[int64](Need $support 'predecessor_ordinal')
$predecessorQpc=[int64](Need $support 'predecessor_qpc')
$successorOrdinal=[int64](Need $support 'successor_ordinal')
$successorQpc=[int64](Need $support 'successor_qpc')
$predecessorSamples=@($samples|Where-Object{[int64]$_.ordinal-eq$predecessorOrdinal-and[int64]$_.qpc-eq$predecessorQpc})
$successorSamples=@($samples|Where-Object{[int64]$_.ordinal-eq$successorOrdinal-and[int64]$_.qpc-eq$successorQpc})
if($predecessorSamples.Count-ne1-or$successorSamples.Count-ne1){
    Fail 'BOUNDARY_SAMPLE_WITNESS_NOT_EXACT' 'W2-C1.1 boundary sample witnessがexactではありません'
}
if(-not[bool](Need $support 'predecessor_valid')-or
   -not[bool](Need $support 'lower_closed_before_candidate_capture')-or
   $captureBegin-le0-or$predecessorQpc-ge$captureBegin){
    Fail 'LOWER_SUPPORT_NOT_CLOSED' 'W2-C1.1 lower supportが閉じていません'
}
if(-not[bool](Need $support 'producer_teardown_completed')-or
   -not[bool](Need $support 'postroll_wait_completed')-or[bool](Need $support 'postroll_wait_timeout')-or
   -not[bool](Need $support 'successor_valid')-or
   -not[bool](Need $support 'upper_closed_after_candidate_capture_and_teardown')-or
   $captureClose-le0-or$captureClose-gt$postrollBoundary-or$postrollBoundary-gt$successorQpc){
    Fail 'UPPER_SUPPORT_NOT_CLOSED' 'W2-C1.1 upper supportが閉じていません'
}
if($predecessorOrdinal-ge$successorOrdinal-or$predecessorQpc-ge$successorQpc){
    Fail 'CLOSED_SUPPORT_ORDER_INVALID' 'W2-C1.1 closed support順序が不正です'
}
foreach($field in @('ring_overflow_count','wait_failure_count')){
    if([int64](Need $support $field)-ne0){
        Fail 'SUPPORT_COUNTER_NONZERO' "W2-C1.1 $field が0ではありません"
    }
}
if(-not[bool](Need $support 'output_stable')-or-not[bool](Need $support 'authority_valid')){
    Fail 'SUPPORT_AUTHORITY_INVALID' 'W2-C1.1 support authorityが不成立です'
}
$result=[ordered]@{
    schema='mvm-p2-d5-2-w2-c11-physical-mapping-support-check-1';stage='P2-D5-2-W2-C1.1'
    pass=$true;predecessor_ordinal=$predecessorOrdinal;predecessor_qpc=$predecessorQpc
    successor_ordinal=$successorOrdinal;successor_qpc=$successorQpc
    capture_begin_qpc=$captureBegin;capture_close_qpc=$captureClose
    postroll_boundary_qpc=$postrollBoundary
    verdict='PHYSICAL_MAPPING_SUPPORT_ENVELOPE_CLOSED_EXACT'
}
if($Output){$result|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $Output -Encoding utf8}
Write-Output 'P2-D5-2 W2-C1.1 physical support envelope: PASS'
