[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SourceDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Fail([string]$Message){throw $Message}
if(-not(Test-Path -LiteralPath $SourceDirectory)){Fail "source artifactがありません: $SourceDirectory"}
if(Test-Path -LiteralPath $OutputDirectory){Fail "既存audit artifactを上書きしません: $OutputDirectory"}
$SourceDirectory=(Resolve-Path -LiteralPath $SourceDirectory).Path
$manifestPath=Join-Path $SourceDirectory 'manifest.sha256'
foreach($name in @('traced-app.json','present-history-raw.json','summary.json','manifest.sha256')){
    if(-not(Test-Path -LiteralPath (Join-Path $SourceDirectory $name))){Fail "source artifact fieldがありません: $name"}
}
foreach($line in Get-Content -LiteralPath $manifestPath -Encoding ascii){
    if($line-notmatch'^([0-9a-fA-F]{64})  (.+)$'){Fail "source manifest行が不正です: $line"}
    $path=Join-Path $SourceDirectory $Matches[2]
    if(-not(Test-Path -LiteralPath $path)){Fail "source manifest対象がありません: $($Matches[2])"}
    if((Hash $path)-ne$Matches[1].ToLowerInvariant()){Fail "source manifest hashが一致しません: $($Matches[2])"}
}
$appPath=Join-Path $SourceDirectory 'traced-app.json'
$etwPath=Join-Path $SourceDirectory 'present-history-raw.json'
$summaryPath=Join-Path $SourceDirectory 'summary.json'
$app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
$etw=Get-Content -LiteralPath $etwPath -Raw -Encoding utf8|ConvertFrom-Json
$summary=Get-Content -LiteralPath $summaryPath -Raw -Encoding utf8|ConvertFrom-Json
$start=[int64]$app.presentation_opportunity.measurement_start_qpc
$end=[int64]$app.presentation_opportunity.measurement_end_qpc_exclusive
$targetPid=[int64]$etw.target_process_id
if($targetPid-ne[int64]$app.process_id){Fail 'app/ETW target PIDが一致しません'}
$native=@($app.native_present_hook.records)
$swapchain=[uint64]$native[0].swapchain_identity
$events=@($etw.events|Where-Object{
    $address=[string]$_.swap_chain_address
    $parsed=if($address.StartsWith('0x')){[Convert]::ToUInt64($address.Substring(2),16)}else{[uint64]$address}
    [int64]$_.process_id-eq$targetPid-and$parsed-eq$swapchain-and
    [int64]$_.present_start_qpc-ge$start-and[int64]$_.present_start_qpc-lt$end
}|Sort-Object{[int64]$_.present_start_qpc})
if($events.Count-ne$native.Count){Fail "historical native/ETW countが一致しません: native=$($native.Count) etw=$($events.Count)"}
$presented=0;$discarded=0;$incomplete=0
foreach($presentEvent in $events){
    $displayed=@($presentEvent.displayed)
    if([string]$presentEvent.final_state-eq'Presented'-and$displayed.Count-gt0){++$presented}
    elseif([string]$presentEvent.final_state-eq'Discarded'-and$displayed.Count-eq0){++$discarded}
    else{++$incomplete}
}
$completionFields=@('is_completed','is_lost','present_mode','seen_dxgk_present',
    'seen_win32k_events','seen_in_frame_event','wait_for_flip_event','wait_for_mpo_flip_event')
$completionFieldsAvailable=$true
foreach($presentEvent in $events){
    foreach($field in $completionFields){
        if($presentEvent.PSObject.Properties.Name-notcontains$field){$completionFieldsAvailable=$false;break}
    }
    if(-not$completionFieldsAvailable){break}
}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$audit=[ordered]@{
    schema='mvm-p2-c0-r2-historical-final-state-audit-1'
    source_artifact=$SourceDirectory;source_manifest_verified=$true
    historical_checker_verdict=[string]$summary.c0_status
    historical_unpresented_label_semantically_overbroad=$true
    native_etw_submission_join='PASS';measurement_record_count=$events.Count
    final_state_counts=[ordered]@{presented=$presented;discarded=$discarded;incomplete_unknown=$incomplete}
    completion_fields_available=$completionFieldsAvailable
    unavailable_completion_fields=$(if($completionFieldsAvailable){@()}else{$completionFields})
    display_completion_oracle=$(if($completionFieldsAvailable-and$incomplete-eq0){'CLOSED'}else{'NOT_YET_VALIDATED'})
    later_attribution='全empty Displayedはexplicit Discardedだが、historical rawにIsLost等が無いためfull closureは未検証'
    source_identities=[ordered]@{
        app_sha256=Hash $appPath;etw_sha256=Hash $etwPath;summary_sha256=Hash $summaryPath
        manifest_sha256=Hash $manifestPath
    }
}
$auditPath=Join-Path $OutputDirectory 'audit.json'
$audit|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $auditPath -Encoding utf8
"$(Hash $auditPath)  audit.json"|Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.sha256') -Encoding ascii
Write-Host "F3-C0-R2 historical Final-State audit: PASS ($OutputDirectory)"
