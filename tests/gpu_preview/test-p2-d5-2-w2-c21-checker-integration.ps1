[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$C13Fixture,
    [Parameter(Mandatory=$true)][string]$C13Core,
    [Parameter(Mandatory=$true)][string]$C1Checker,
    [Parameter(Mandatory=$true)][string]$Inventory,
    [Parameter(Mandatory=$true)][string]$C21Checker,
    [Parameter(Mandatory=$true)][string]$Directory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if(-not(Test-Path -LiteralPath $Directory)){New-Item -ItemType Directory -Path $Directory|Out-Null}
# S2-h: PID だけでは isolation key にならない。Windows は PID を再利用するため、
# 過去 run の process-<PID> directory と衝突して「既存artifactを上書きしません」で
# 失敗する。S2-f2 と同じく invocation ごとに一意な suffix を付ける。
$caseDirectory=Join-Path $Directory ("process-$PID-" + [guid]::NewGuid().ToString('N').Substring(0,12))
New-Item -ItemType Directory -Path $caseDirectory|Out-Null
& $C13Fixture -Case Good -Core $C13Core -Checker $C1Checker -Directory $caseDirectory *> $null
$c1Path=Join-Path $caseDirectory 'c13-good.json'
$c1=Get-Content -LiteralPath $c1Path -Raw -Encoding utf8|ConvertFrom-Json
$appPath=Join-Path $c1.source_c011_directory 'run-1\traced-app.json'
$app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
$app|Add-Member -NotePropertyName required_measurement_frame_count -NotePropertyValue 1
$scope=[pscustomobject][ordered]@{authority_pass=$true;required_intent_set_exact=$true
    required_intent_ordinals=@('0');records=@([pscustomobject][ordered]@{
        token_serial='102';intent_ordinal='0';intent_scope='CURRENT_MEASUREMENT'
        decision_qpc=200;decision_qpc_exact=$true;required_current_membership=$true
        required_current_membership_exact=$true;measurement_boundary_relation='WITHIN_CURRENT_MEASUREMENT'})}
$native=[pscustomobject][ordered]@{composition_token=[pscustomobject]@{token_serial='102'}
    intent_ordinal='0';intent_ordinal_valid=$true;present_serial='12';present_enter_qpc=205}
$hook=[pscustomobject][ordered]@{intent_scope_provenance=$scope;records=@($native)
    capture_envelope=[pscustomobject]@{measurement_arm_qpc=100;measurement_start_qpc=150;frozen_measurement_end_qpc=300}}
$app|Add-Member -NotePropertyName native_present_hook -NotePropertyValue $hook
$app|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $appPath -Encoding utf8
$c1.runs[0].sealed_input_sha256.traced_app=(Get-FileHash -LiteralPath $appPath -Algorithm SHA256).Hash.ToLowerInvariant()
$c1|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $c1Path -Encoding utf8
$proofPath=Join-Path $caseDirectory 'c21-good.json'
& $Inventory -C1Proof $c1Path -Output $proofPath -C1Checker $C1Checker *> $null
& $C21Checker -Proof $proofPath -C1Checker $C1Checker *> $null
$proof=Get-Content -LiteralPath $proofPath -Raw -Encoding utf8|ConvertFrom-Json
if(-not[bool]$proof.authority_exact-or-not[bool]$proof.required_count_set_identity_exact){
    throw '正当なC2.1 integration authorityが閉じていません'
}
$proof.runs[0].decisions[0].opportunity_ordinal='99'
$mutatedPath=Join-Path $caseDirectory 'c21-mutated.json'
$proof|ConvertTo-Json -Depth 16|Set-Content -LiteralPath $mutatedPath -Encoding utf8
$failed=$false;try{& $C21Checker -Proof $mutatedPath -C1Checker $C1Checker *> $null}catch{$failed=$true}
if(-not$failed){throw '改変C2.1 decision ledgerをcheckerが受理しました'}
Write-Output 'W2-C2.1 checker integration: PASS'
