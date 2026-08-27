function Get-P5E4T1HangClassification {
    param(
        [Parameter(Mandatory)][object[]]$Entries,
        [Parameter(Mandatory)][double]$LastWriteAgeSeconds
    )
    $events = @($Entries | ForEach-Object {$_.event})
    if ('tick.first.enter' -notin $events) {
        return [ordered]@{id='timer_not_fired';label='timer未発火';basis='tick.first.enterがない'}
    }
    $pipelineEnter = @($Entries | Where-Object {$_.event -eq 'open_pipelines.enter'}).Count
    $pipelineExit = @($Entries | Where-Object {$_.event -eq 'open_pipelines.exit'}).Count
    if ($pipelineEnter -gt $pipelineExit) {
        return [ordered]@{id='pipeline_blocking';label='pipeline blocking'
            basis='open_pipelines.enterに対応するexitがない'}
    }
    $starts = @($Entries | Where-Object {$_.event -like 'start_at_frame.*'})
    $startEnter = @($starts | Where-Object {$_.event -eq 'start_at_frame.enter'}).Count
    $startExit = @($starts | Where-Object {$_.event -eq 'start_at_frame.exit'}).Count
    if ($startEnter -gt $startExit) {
        $lastStart = @($starts | Where-Object {$_.event -eq 'start_at_frame.enter'})[-1]
        return [ordered]@{id='initial_start_blocking';label='initial-start blocking'
            basis="start_at_frame.exitがない (measurement_start=$($lastStart.measurement_start))"}
    }
    $heartbeats = @($Entries | Where-Object {$_.event -eq 'heartbeat'})
    if ($LastWriteAgeSeconds -le 7.5 -and $heartbeats.Count -ge 5) {
        $tail = @($heartbeats | Select-Object -Last 5)
        $phases = @($tail | Select-Object -ExpandProperty phase -Unique)
        $samples = @($tail | Select-Object -ExpandProperty audio_clock_media_sample -Unique)
        if ($phases.Count -eq 1 -and $samples.Count -eq 1 -and $phases[0] -eq 'Warmup') {
            return [ordered]@{id='warmup_clock_stall';label='Warmup clock stall'
                basis='直近5 heartbeatでWarmupのaudio clock sampleが不変'}
        }
        if ($phases.Count -eq 1 -and $samples.Count -eq 1 -and $phases[0] -eq 'Playback') {
            return [ordered]@{id='playback_clock_stall';label='Playback clock stall'
                basis='直近5 heartbeatでPlaybackのaudio clock sampleが不変'}
        }
    }
    return [ordered]@{id='event_loop_starvation';label='event-loop starvation'
        basis="pipeline/start boundaryは完了し、durable heartbeatが停止またはclock stall条件外 (last_write_age_seconds=$LastWriteAgeSeconds)"}
}
