[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$InputJson,
    [string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Need($Object,[string]$Name){
    if($null-eq$Object-or$Object.PSObject.Properties.Name-notcontains$Name){Fail "必須fieldがありません: $Name"}
    return $Object.$Name
}
if(-not(Test-Path -LiteralPath $InputJson)){Fail "C0.1 inputがありません: $InputJson"}
$app=Get-Content -LiteralPath $InputJson -Raw -Encoding utf8|ConvertFrom-Json
$opportunity=Need $app 'presentation_opportunity'
$shadow=Need $opportunity 'physical_vblank_domain_shadow'
$hook=Need $app 'native_present_hook'
$envelope=Need $hook 'capture_envelope'
$records=@(Need $hook 'capture_envelope_records')
$b1Records=@(Need $hook 'records')
if([string](Need $envelope 'schema')-ne'mvm-p2-d5-2-w2-c01-capture-envelope-1'){Fail 'C0.1 schemaが不正です'}
$begin=[int64](Need $envelope 'begin_qpc')
$arm=[int64](Need $envelope 'measurement_arm_qpc')
$start=[int64](Need $envelope 'measurement_start_qpc')
$frozenEnd=[int64](Need $envelope 'frozen_measurement_end_qpc')
$serializedEnd=[int64](Need $envelope 'serialized_measurement_end_qpc')
$opportunityEnd=[int64](Need $opportunity 'measurement_end_qpc_exclusive')
$successor=[int64](Need $envelope 'successor_sample_qpc')
$close=[int64](Need $envelope 'close_qpc')
if(-not[bool](Need $envelope 'enabled')){Fail 'capture envelopeが有効ではありません'}
if([string](Need $envelope 'lower_intent_producer')-ne'formal opportunity scheduler preroll'-or
   -not[bool](Need $envelope 'lower_intent_producer_started')-or
   -not[bool](Need $envelope 'lower_intent_producer_completed_before_measurement')){
    Fail 'lower envelopeのscheduler-produced intent provenanceが不成立です'
}
if(-not[bool](Need $envelope 'lower_closed_before_measurement_arm')-or
   $begin-le0-or$begin-gt$arm-or$arm-gt$start){Fail 'native captureがmeasurement arm前に開いていません'}
if([bool](Need $envelope 'measurement_window_extended')-or
   -not[bool](Need $envelope 'measurement_window_unchanged')-or
   $frozenEnd-le$start-or$frozenEnd-ne$serializedEnd-or$frozenEnd-ne$opportunityEnd){
    Fail 'frozen measurement windowが変更されています'
}
if(-not[bool](Need $envelope 'successor_wait_completed')-or
   [bool](Need $envelope 'successor_wait_timeout')-or$successor-lt$frozenEnd){
    Fail 'frozen measurement end以後のphysical successorが確認されていません'
}
if(-not[bool](Need $envelope 'upper_closed_after_successor')-or$close-lt$successor){
    Fail 'native capture envelopeがsuccessorより先に閉じています'
}
if(-not[bool](Need $shadow 'shadow_authority_valid')-or
   -not[bool](Need $shadow 'predecessor_valid')-or-not[bool](Need $shadow 'successor_valid')-or
   [int64](Need $shadow 'successor_qpc')-ne$successor){Fail 'physical boundary shadowがC0.1 closureと一致しません'}
foreach($field in @('overflow_count','duplicate_token_count','stale_token_count')){
    if([int64](Need $envelope $field)-ne0){Fail "capture envelope $field が0ではありません"}
}
if([int](Need $envelope 'record_count')-ne$records.Count-or$records.Count-lt$b1Records.Count){
    Fail 'capture envelope superset件数が不正です'
}
foreach($record in $b1Records){
    if([int64](Need $record 'present_enter_qpc')-lt$start-or
       [int64](Need $record 'present_return_qpc')-ge$frozenEnd){Fail 'B1 projectionへenvelope境界recordが混入しています'}
}
if(-not[bool](Need $envelope 'authority_pass')){Fail 'capture envelope authorityが不成立です'}
$result=[ordered]@{
    schema='mvm-p2-d5-2-w2-c01-capture-envelope-check-1'
    stage='P2-D5-2-W2-C0.1';pass=$true
    capture_envelope_record_count=$records.Count;b1_projection_record_count=$b1Records.Count
    begin_qpc=$begin;measurement_arm_qpc=$arm;measurement_start_qpc=$start
    frozen_measurement_end_qpc=$frozenEnd;successor_qpc=$successor;close_qpc=$close
    verdict='CAPTURE_ENVELOPE_CLOSED_EXACT'
}
if($Output){$result|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $Output -Encoding utf8}
Write-Output "P2-D5-2-W2-C0.1 capture envelope: PASS"
