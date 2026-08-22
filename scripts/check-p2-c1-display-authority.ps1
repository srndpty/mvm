[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AppJson,
    [Parameter(Mandatory=$true)][string]$OracleJson,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Equal($Actual,$Expected,[string]$Name){if($Actual-ne$Expected){Fail "$Name が一致しません (expected=$Expected actual=$Actual)"}}
$app=Get-Content -LiteralPath $AppJson -Raw -Encoding utf8|ConvertFrom-Json
$oracle=Get-Content -LiteralPath $OracleJson -Raw -Encoding utf8|ConvertFrom-Json
Equal ([string]$oracle.schema) 'mvm-p2-c0-native-etw-oracle-1' 'oracle schema'
Equal ([string]$oracle.oracle_status) 'ORACLE_VALID' 'oracle status'
Equal ([string]$oracle.display_completion_status) 'CLOSED' 'display completion status'
Equal ([int64]$oracle.incomplete_unknown_count) 0 'incomplete unknown count'
Equal ([int64]$oracle.lost_count) 0 'lost count'
$required=[int64]$app.required_measurement_frame_count
if($required-le0){Fail 'required measurement frame countが不正です'}
$records=@($oracle.records)
Equal $records.Count ([int64]$oracle.native_present_count) 'oracle record count'
$presentedFrames=@();$presentedIdentities=@();$previousOpportunity=$null
foreach($record in $records){
    $classification=[string]$record.completion_class
    $frame=[int64]$record.output_frame
    if($frame-lt0-or$frame-ge$required){Fail "output frameが3600-domain外です: $frame"}
    if([string]::IsNullOrWhiteSpace([string]$record.present_serial)-or
       [string]::IsNullOrWhiteSpace([string]$record.composition_token_serial)){
        Fail 'native/composition identityがありません'
    }
    $displayed=@($record.displayed_qpc);$opportunities=@($record.actual_opportunity_ordinals)
    if($classification-eq'PRESENTED'){
        if($displayed.Count-eq0-or$displayed.Count-ne$opportunities.Count){
            Fail "Presented recordのDisplayedQPC/opportunityが不正です: frame=$frame"
        }
        $firstOpportunity=[int64]$opportunities[0]
        if($null-ne$previousOpportunity-and$firstOpportunity-le[long]$previousOpportunity){
            Fail "Presented opportunityがstrict monotoneではありません: frame=$frame"
        }
        $previousOpportunity=$firstOpportunity
        $presentedFrames+=$frame
        $presentedIdentities+=[ordered]@{
            output_frame=$frame;present_serial=[string]$record.present_serial
            composition_token_serial=[string]$record.composition_token_serial
            displayed_qpc=@($displayed|ForEach-Object{[int64]$_})
            physical_opportunity_ordinals=@($opportunities|ForEach-Object{[int64]$_})
        }
    }elseif($classification-eq'DISCARDED'){
        if($displayed.Count-ne0-or$opportunities.Count-ne0){
            Fail "Discarded recordにdisplay authority payloadがあります: frame=$frame"
        }
    }else{
        Fail "closure後のrecord分類が不正です: $classification"
    }
}
$uniqueFrames=@($presentedFrames|Sort-Object -Unique)
$gapDrops=0L;$nextFrame=0L
foreach($frame in $uniqueFrames){
    if($frame-ge$nextFrame){$gapDrops+=$frame-$nextFrame;$nextFrame=$frame+1}
}
$tailDrops=[math]::Max(0L,$required-$nextFrame)
$formalDrops=$gapDrops+$tailDrops
Equal ($uniqueFrames.Count+$formalDrops) $required 'source frame domain accounting'
$result=[ordered]@{
    schema='mvm-p2-c1-display-authority-proof-1';authority='diagnostic_offline_proof'
    proof_status='PASS';formal_counter_authority_changed=$false
    source_domain_size=$required
    native_present_count=[int64]$oracle.native_present_count
    presented_record_count=$presentedFrames.Count
    discarded_present_count=[int64]$oracle.discarded_count
    displayed_unique_source_frames=$uniqueFrames.Count
    repeated_displayed_source_frames=$presentedFrames.Count-$uniqueFrames.Count
    source_frame_gap_drops=$gapDrops;tail_source_frame_drops=$tailDrops
    formal_source_frame_drops=$formalDrops
    source_frame_accounting_exact=$true
    discarded_present_count_added_to_formal_drops=$false
    presented_identities=$presentedIdentities
}
$result|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C1 display authority proof: PASS unique=$($uniqueFrames.Count) drops=$formalDrops domain=$required"
