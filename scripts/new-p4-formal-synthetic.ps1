[CmdletBinding()]
param([Parameter(Mandatory)][string]$Output, [string]$Case = 'GoodFormal')
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
$e0=7L
function State([long]$f){if($f-lt600){'S0'}elseif($f-lt1200){'S1'}elseif($f-lt1800){'S2'}elseif($f-lt2400){'S3'}elseif($f-lt3000){'S0'}else{'S1'}}
function Epoch([long]$f){[long]($e0+[Math]::Floor($f/600))}
function Source([long]$id,[long]$f){[ordered]@{source_id=$id;frame=$f;source_generation=$(if($id-eq1){11}else{12});resource_epoch=$(if($id-eq1){21}else{22})}}
function Record([long]$f){[ordered]@{output_frame=$f;composition_state=(State $f);composition_epoch=(Epoch $f);application_av_projection_valid=$true;application_av_delta_ms=0.0;sources=@((Source 1 $f),(Source 2 $f))}}
$ledger=@(0..3599|ForEach-Object{Record $_})
function Transition([long]$b){[ordered]@{boundary=$b;expected_state=(State $b);first_displayed_output_frame=$b;activation_lag_frames=0;first_display_state=(State $b);first_display_composition_epoch=(Epoch $b);first_display_sources=@((Source 1 $b),(Source 2 $b))}}
function Probe([long]$b,[string]$name){$rgba=if($name-eq'TL'){@(80,90,100,255)}else{@(110,120,130,255)};[ordered]@{boundary=$b;actual_output_frame=$b;composition_state=(State $b);cpu_reference_state=(State $b);composition_epoch=(Epoch $b);probe=$name;x=$(if($name-eq'TL'){480}else{1440});y=$(if($name-eq'TL'){270}else{810});actual_rgba=@($rgba);cpu_expected_rgba=@($rgba);gpu_ticket=$b;gpu_completion_serial=$b;completion_observed=$true;blocking_wait_count=0;sources=@((Source 1 $b),(Source 2 $b))}}
$bounds=@(600L,1200L,1800L,2400L,3000L)
$probes=@();foreach($b in $bounds){$probes+=(Probe $b 'TL');$probes+=(Probe $b 'BR')}
$env=[ordered]@{screen_name='test';screen_orientation='landscape';screen_geometry_width=1920;screen_geometry_height=1080;available_geometry_width=1920;available_geometry_height=1040;device_pixel_ratio=1.0;window_logical_width=1920;window_logical_height=1080;compositor_surface_logical_width=1920;compositor_surface_logical_height=1080;rhi_target_pixel_width=1920;rhi_target_pixel_height=1080}
$raw=[ordered]@{
 schema='mvm-p4-formal-1';schema_version=1;contract_version='P4-formal-frozen';phase='P4-D';schedule_kind='formal';formal_verdict='NOT_RUN';process_exit_code=0
 canonical_schedule='0:S0;600:S1;1200:S2;1800:S3;2400:S0;3000:S1';canonical_schedule_sha256='5b66543f43f98ad261a5a96e961332ef4a3d5b21f8f30b1713b4ff420a855f79'
 schedule=@(@{boundary=0;state='S0'},@{boundary=600;state='S1'},@{boundary=1200;state='S2'},@{boundary=1800;state='S3'},@{boundary=2400;state='S0'},@{boundary=3000;state='S1'})
 fixture_a_sha256='d398114c38806f39670df51dfabb0095d462cbc35286ea1467901d4007cf0308';fixture_b_sha256='fe7cd1a45d101d363cb3930497601efd41dd55fd36c194acf8f24e3e4728b479'
 warmup_seconds=5;measurement_seconds=60;required_video_frames=3600;measurement_audio_start_sample=0;measurement_audio_end_sample=2880000;audio_master_only=$true
 cpu_reference_pixel_status='PRECOMPUTED';cpu_reference_candidate_frame_count=15;cpu_reference_candidate_probe_count=30
 measurement_baseline_composition_epoch=$e0;baseline_source_generation_a=11;baseline_source_generation_b=12;baseline_resource_epoch_a=21;baseline_resource_epoch_b=22
 measurement_display_ledger=$ledger;measurement_display_ledger_count=3600;measurement_video_displayed_unique_count=3600;measurement_video_skipped_frame_count=0;measurement_non_increasing_display_count=0
 effective_video_fps=60.0;drop_rate=0.0;application_av_delta_ms=@{count=3600;p50=0.0;p95=0.0;min=0.0;max=0.0};application_av_delta_abs_ms=@{count=3600;p50=0.0;p95=0.0;p99=0.0;min=0.0;max=0.0};application_av_projection_failure_count=0
 composition_state_resolve_count=3600;composition_state_adoption_count=5;composition_state_noop_count=3595;composition_state_reject_count=0;composition_state_unresolved_count=0;composition_epoch_increment_count=5
 transition_activation_lag_frames=@(0,0,0,0,0);transition_boundaries=@($bounds|ForEach-Object{Transition $_});old_state_after_boundary_count=0
 transition_pixel_probe_status='COMPLETE';transition_probe_records=$probes;transition_probe_checked_count=10;transition_probe_mismatch_count=0;transition_probe_render_thread_blocking_wait_count=0;transition_probe_untracked_submission_count=0;transition_probe_completion_failure_count=0;transition_probe_retirement_timeout_count=0;transition_probe_pending_after_drain_count=0;transition_probe_issue_failure_count=0
 composition_state_display_mismatch_count=0;composition_pair_identity_violation_count=0;composition_layer_generation_mismatch_count=0;source_generation_change_due_to_layout_count=0;phase4_adoption_failure_count=0
 measurement_audio_underflow_count=0;measurement_audio_overflow_count=0;measurement_marker_mismatch_count=0;measurement_mixed_pair_count=0;measurement_mixed_generation_count=0;measurement_stale_composition_epoch_count=0;measurement_video_ahead_violation_count=0;measurement_clock_regression_count=0;measurement_video_qpc_master_fallback_count=0;measurement_audio_clock_query_failure_count=0;measurement_audio_clock_catchup_skip_count=0;measurement_scheduler_deadline_drop_count=0;measurement_render_failure_count=0
 cpu_full_frame_readback_count=0;full_frame_gpu_copy_count=0;software_video_fallback_count=0;untracked_submission_count=0;completion_poll_failure_count=0;retirement_depth_after_drain=0;payloads_released_before_completion=0;retirement_timeout_count=0;partial_gpu_issue_failure_count=0;device_lost_count=0;lifecycle_violation_count=0;audio_render_thread_join_leak=0;audio_decode_thread_join_leak=0
 video_worker_a_joined=$true;video_worker_b_joined=$true;teardown_success=$true;final_report_after_teardown=$true;shutdown_workers_joined_before_teardown=$true;shutdown_render_teardown_requested=$true;shutdown_order_violation_count=0
 shutdown_sequence=@('DisableSchedulers','StopAudioSink','StopAudioDecodeWorker','StopVideoWorkerA','StopVideoWorkerB','DetachSharedWorkerRefs','RequestRenderTeardown')
 display_target_preflight_pass=$true;requested_output_width=1920;requested_output_height=1080;display_environment_start=$env;display_environment_end=([ordered]@{}+$env);adapter='synthetic-adapter';audio_endpoint_sample_rate=48000;audio_endpoint_channels=2;audio_endpoint_sample_format='flt'
}
function Remove-Frames([long]$First,[long]$Last){
 $raw.measurement_display_ledger=@($raw.measurement_display_ledger|Where-Object{$_.output_frame-lt$First-or$_.output_frame-gt$Last})
 $skipped=$Last-$First+1;$unique=3600-$skipped
 $raw.measurement_display_ledger_count=$unique;$raw.measurement_video_displayed_unique_count=$unique;$raw.measurement_video_skipped_frame_count=$skipped
 $raw.effective_video_fps=$unique/60.0;$raw.drop_rate=$skipped/3600.0
 $raw.application_av_delta_ms.count=$unique;$raw.application_av_delta_abs_ms.count=$unique
 $raw.measurement_audio_clock_catchup_skip_count=$skipped
}
switch($Case){
 'GoodFormal'{}
 'WrongFormalSchedule'{$raw.canonical_schedule='0:S0;600:S2'}
 'WrongFormalHash'{$raw.canonical_schedule_sha256='00'*32}
 'SmokeScheduleInFormal'{$raw.canonical_schedule='0:S0;200:S1;400:S2'}
 'WrongWarmup4'{$raw.warmup_seconds=4}
 'WrongDuration59'{$raw.measurement_seconds=59}
 'WrongRequiredFrames'{$raw.required_video_frames=3599}
 'WrongAudioEndSample'{$raw.measurement_audio_end_sample=2879999}
 'Adoption4'{$raw.composition_state_adoption_count=4}
 'Adoption6'{$raw.composition_state_adoption_count=6}
 'EpochIncrement4'{$raw.composition_epoch_increment_count=4}
 'Reject1'{$raw.composition_state_reject_count=1}
 'WrongState'{$raw.measurement_display_ledger[1200].composition_state='S1'}
 'StaleEpoch'{$raw.measurement_display_ledger[1200].composition_epoch=$e0+1}
 'WrongReturnToS0EpochAt2400'{$raw.measurement_display_ledger[2400].composition_epoch=$e0}
 'WrongReturnToS1EpochAt3000'{$raw.measurement_display_ledger[3000].composition_epoch=$e0+1}
 'Lag3'{$raw.measurement_display_ledger=@($raw.measurement_display_ledger|Where-Object{$_.output_frame-notin@(600,601,602)});$raw.measurement_display_ledger_count=3597;$raw.measurement_video_displayed_unique_count=3597;$raw.measurement_video_skipped_frame_count=3;$raw.effective_video_fps=59.95;$raw.drop_rate=3/3600;$raw.application_av_delta_abs_ms.count=3597;$raw.transition_activation_lag_frames[0]=3;$raw.transition_boundaries[0].first_displayed_output_frame=603;$raw.transition_boundaries[0].activation_lag_frames=3}
 'MissingLag'{$raw.transition_activation_lag_frames=@(0,0,0,0)}
 'LagStringZero'{$raw.transition_activation_lag_frames[0]='0'}
 'LagDoubleZero'{$raw.transition_activation_lag_frames[0]=[double]0.0}
 'LagBoolean'{$raw.transition_activation_lag_frames[0]=$true}
 'OldStateAfterBoundary'{$raw.old_state_after_boundary_count=1}
 'ProbeCount9'{$raw.transition_probe_records=@($raw.transition_probe_records|Select-Object -First 9);$raw.transition_probe_checked_count=9}
 'ProbeCount11'{$raw.transition_probe_records+=Probe 3000 'BR';$raw.transition_probe_checked_count=11}
 'ProbeMissing'{$raw.transition_probe_records=@($raw.transition_probe_records|Select-Object -Skip 1);$raw.transition_probe_checked_count=9}
 'ProbeDuplicate'{$raw.transition_probe_records[1]=$raw.transition_probe_records[0]}
 'ProbeWrongFrame'{$raw.transition_probe_records[0].actual_output_frame=601}
 'ProbeWrongState'{$raw.transition_probe_records[0].composition_state='S0'}
 'ProbeWrongEpoch'{$raw.transition_probe_records[0].composition_epoch=$e0}
 'CpuReferenceWrongState'{$raw.transition_probe_records[0].cpu_reference_state='S0'}
 'WrongProbeRgb'{$raw.transition_probe_records[0].actual_rgba[0]+=4}
 'WrongProbeAlpha'{$raw.transition_probe_records[0].actual_rgba[3]=254}
 'ProbeActualRgbString'{$raw.transition_probe_records[0].actual_rgba[0]='80'}
 'ProbeExpectedRgbString'{$raw.transition_probe_records[0].cpu_expected_rgba[0]='80'}
 'ProbeRgbaNull'{$raw.transition_probe_records[0].actual_rgba[0]=$null}
 'ProbeRgbaOutOfRange'{$raw.transition_probe_records[0].actual_rgba[0]=256}
 'ProbeBlockingWait'{$raw.transition_probe_render_thread_blocking_wait_count=1}
 'ProbePending'{$raw.transition_probe_pending_after_drain_count=1}
 'ProbeCompletionFailure'{$raw.transition_probe_completion_failure_count=1}
 'GenerationChanged'{$raw.measurement_display_ledger[100].sources[0].source_generation=13}
 'ResourceEpochChanged'{$raw.measurement_display_ledger[100].sources[0].resource_epoch=23}
 'FpsBelow55'{Remove-Frames 3003 3303}
 'DropOver2Percent'{Remove-Frames 3003 3075}
 'AvP95_20_001'{for($i=0;$i-lt216;$i++){$raw.measurement_display_ledger[$i].application_av_delta_ms=20.001};$raw.application_av_delta_ms.p95=20.001;$raw.application_av_delta_ms.max=20.001;$raw.application_av_delta_abs_ms.p95=20.001;$raw.application_av_delta_abs_ms.p99=20.001;$raw.application_av_delta_abs_ms.max=20.001}
 'AvMax_33_335'{$raw.measurement_display_ledger[0].application_av_delta_ms=33.335;$raw.application_av_delta_ms.max=33.335;$raw.application_av_delta_abs_ms.p99=0.0;$raw.application_av_delta_abs_ms.max=33.335}
 'AvProjectionFalse'{$raw.measurement_display_ledger[0].application_av_projection_valid=$false}
 'AvMissingRaw'{$raw.measurement_display_ledger[0].Remove('application_av_delta_ms')}
 'AvNaN'{$raw.measurement_display_ledger[0].application_av_delta_ms='NaN'}
 'AvSummaryDoesNotMatchRaw'{$raw.application_av_delta_abs_ms.p95=1.0}
 'ShutdownOrder'{$raw.shutdown_sequence[0]='StopAudioSink'}
 'TeardownBeforeJoin'{$raw.shutdown_workers_joined_before_teardown=$false}
 'MissingBoolean'{$raw.Remove('teardown_success')}
 'StringFalseBoolean'{$raw.teardown_success='false'}
 'ProcessExitNonzero'{$raw.process_exit_code=3}
 'WrongAudioSampleRate'{$raw.audio_endpoint_sample_rate=44100}
 'WrongAudioChannels'{$raw.audio_endpoint_channels=1}
 'WrongAudioSampleFormat'{$raw.audio_endpoint_sample_format='float'}
 default{throw "未知caseです: $Case"}
}
$parent=Split-Path -Parent $Output;if($parent){New-Item -ItemType Directory -Force -Path $parent|Out-Null}
$raw|ConvertTo-Json -Depth 12|Set-Content -LiteralPath $Output -Encoding utf8
