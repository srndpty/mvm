[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AppJson,
    [Parameter(Mandatory=$true)][ValidateSet('CONTROL','TARGET_PIXEL')][string]$ExpectedMode,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# P2-D5-2-W2-E canonical authority disposition (machine-readable)。
# W2-E retirement inventory がこの宣言を読む。legacy presentation metric
# (effective_fps / drop_rate / effective_video_fps) を参照するが、
# canonical performance verdict は出さないことを宣言する。
$MvmPresentationAuthorityDisposition = [ordered]@{
    presentation_authority        = 'FORMAL_V2'
    legacy_presentation_metrics   = 'DIAGNOSTIC'
    canonical_performance_verdict = 'DEFERRED_TO_W3'
}
[void]$MvmPresentationAuthorityDisposition
function Fail([string]$Message){throw $Message}
function I64($Value){[int64][string]$Value}
$raw=Get-Content -LiteralPath $AppJson -Raw -Encoding utf8|ConvertFrom-Json
$ExpectedPixelToggle=$ExpectedMode-eq'TARGET_PIXEL'
if(-not[bool]$raw.measurement_available){Fail 'measurementが成立していません'}
if([bool]$raw.diagnostic_target_rhiitem_pixel_toggle-ne$ExpectedPixelToggle){Fail 'pixel toggle条件が一致しません'}
$preflight=$raw.t2_preflight
foreach($name in @('target_hwnd','gwl_exstyle_raw','QT_QPA_DISABLE_REDIRECTION_SURFACE',
                    'QT_D3D_NO_FLIP','QT_D3D_MAX_FRAME_LATENCY','QSG_NO_VSYNC')){
    if($preflight.PSObject.Properties.Name-notcontains$name){Fail "T2 preflight raw fieldがありません: $name"}
}
if([string]::IsNullOrWhiteSpace([string]$preflight.target_hwnd)-or
   [string]::IsNullOrWhiteSpace([string]$preflight.gwl_exstyle_raw)){Fail 'target HWND/exstyleが固定されていません'}
$hook=$raw.native_present_hook
if(-not[bool]$hook.authority_pass-or[bool]$hook.authority_failure){Fail 'native Present authorityがPASSではありません'}
$dirty=$hook.dirty_propagation
if([string]$dirty.schema-ne'mvm-p2-c3-a3-t2-dirty-propagation-1'){Fail 'dirty propagation schemaが不正です'}
if((I64 $dirty.overflow_count)-ne0-or(I64 $dirty.duplicate_stage_count)-ne0){Fail 'dirty propagation ringが完全ではありません'}
$names=@('renderer_update','node_schedule_update','window_update','node_render',
         'compositor_render','composition_token','dirty_material','texture_changed',
         'qsg_main_render','rhi_end_frame','successful_present','target_pixel_toggle')
$records=@($dirty.records)
if($records.Count-lt60){Fail "dirty propagation recordが少なすぎます: $($records.Count)"}
$nativePresentCount=@($hook.records).Count
$renderCallbackCount=I64 $raw.measurement_present_callback_count
if($nativePresentCount-ne$records.Count-or$renderCallbackCount-ne$records.Count){
    Fail "callback/propagation/Present countが一致しません: callback=$renderCallbackCount propagation=$($records.Count) Present=$nativePresentCount"
}
$closed=@();$renderCutoff=@();$scheduleCutoff=@()
for($index=0;$index-lt$records.Count;$index++){
    $record=$records[$index];$serial=I64 $record.propagation_serial
    if($serial-ne$index+1){Fail "propagation serialが不連続です: index=$index serial=$serial"}
    $q=[ordered]@{};foreach($name in $names){$q[$name]=I64 $record.stage_qpc.$name}
    if($q.renderer_update-le0-or$q.node_schedule_update-le0-or$q.window_update-le0-or
       $q.renderer_update-gt$q.node_schedule_update-or$q.node_schedule_update-gt$q.window_update){
        Fail "update/schedule/window chainが不正です: serial=$serial"
    }
    $late=@('node_render','compositor_render','composition_token','dirty_material',
            'texture_changed','qsg_main_render','rhi_end_frame','successful_present')
    $latePresent=@($late|Where-Object{$q[$_]-gt0})
    if($latePresent.Count-eq0){
        $scheduleCutoff+=$record;continue
    }
    if($latePresent.Count-eq2-and$q.node_render-gt0-and$q.compositor_render-gt0){
        $renderCutoff+=$record;continue
    }
    foreach($name in $late){if($q[$name]-le0){Fail "closed chain stageが欠落しています: serial=$serial stage=$name"}}
    $order=@('renderer_update','node_schedule_update','window_update','node_render',
             'compositor_render','composition_token','dirty_material','texture_changed',
             'qsg_main_render','rhi_end_frame','successful_present')
    for($stage=1;$stage-lt$order.Count;$stage++){
        if($q[$order[$stage-1]]-gt$q[$order[$stage]]){Fail "stage順序が逆転しています: serial=$serial stage=$($order[$stage])"}
    }
    if((I64 $record.composition_token_serial)-le0-or(I64 $record.present_serial)-le0-or
       (I64 $record.output_frame)-lt0){Fail "closed chain identityがありません: serial=$serial"}
    if($ExpectedPixelToggle){
        if($q.target_pixel_toggle-le0-or$q.target_pixel_toggle-lt$q.compositor_render-or
           $q.target_pixel_toggle-gt$q.composition_token){Fail "target pixel marker stageが不正です: serial=$serial"}
    }elseif($q.target_pixel_toggle-ne0){Fail "CONTROLにtarget pixel markerがあります: serial=$serial"}
    $closed+=$record
}
if($closed.Count-lt60){Fail "exact closureしたrecordが少なすぎます: $($closed.Count)"}
if($renderCutoff.Count-ne1-or$scheduleCutoff.Count-ne1-or
   (I64 $renderCutoff[0].propagation_serial)-ne$records.Count-1-or
   (I64 $scheduleCutoff[0].propagation_serial)-ne$records.Count){
    Fail "measurement end boundary shapeが不正です: closed=$($closed.Count) render_cutoff=$($renderCutoff.Count) schedule_cutoff=$($scheduleCutoff.Count)"
}
$expected=[ordered]@{
    renderer_update=$records.Count;node_schedule_update=$records.Count;window_update=$records.Count
    node_render=$closed.Count+1;compositor_render=$closed.Count+1
    composition_token=$closed.Count;dirty_material=$closed.Count;texture_changed=$closed.Count
    qsg_main_render=$closed.Count;rhi_end_frame=$closed.Count;successful_present=$closed.Count
    target_pixel_toggle=$(if($ExpectedPixelToggle){$closed.Count}else{0})
}
foreach($name in $names){
    $actual=I64 $dirty.stage_counts.$name
    if($actual-ne$expected[$name]){Fail "stage countがrecord再計算と一致しません: $name expected=$($expected[$name]) actual=$actual"}
}
$presentBySerial=@{};foreach($present in @($hook.records)){
    $serial=I64 $present.propagation_serial
    if($serial-gt0){if($presentBySerial.ContainsKey($serial)){Fail "Present propagation serialが重複しています: $serial"};$presentBySerial[$serial]=$present}
}
foreach($record in $closed){
    $serial=I64 $record.propagation_serial
    if(-not$presentBySerial.ContainsKey($serial)){Fail "closed chainに対応するPresentがありません: $serial"}
    $present=$presentBySerial[$serial]
    if((I64 $present.present_serial)-ne(I64 $record.present_serial)-or
       (I64 $present.composition_token.propagation_serial)-ne$serial-or
       (I64 $present.composition_token.token_serial)-ne(I64 $record.composition_token_serial)){
        Fail "Present/composition identity joinが不正です: serial=$serial"
    }
}
$result=[ordered]@{
    schema='mvm-p2-c3-a3-t2-update-chain-proof-1';status='PASS';authority='diagnostic_only'
    verdict='UPDATE_CHAIN_EXACT';target_pixel_toggle=$ExpectedPixelToggle
    propagation_record_count=$records.Count;exact_closed_count=$closed.Count
    native_present_count=$nativePresentCount;measurement_present_callback_count=$renderCallbackCount
    measurement_elapsed_seconds=[double]$raw.measurement_elapsed_seconds
    effective_fps=[double]$raw.effective_fps
    render_cutoff_count=$renderCutoff.Count;schedule_cutoff_count=$scheduleCutoff.Count
    measurement_end_boundary='RENDER_CUTOFF_THEN_SCHEDULE_CUTOFF'
    stage_counts=$dirty.stage_counts;preflight=$preflight
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    production_scheduler_changed=$false
}
$result|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $Output -Encoding utf8
Write-Host "F3-C3-A3-T2 update chain: PASS exact=$($closed.Count) pixel=$ExpectedPixelToggle"
