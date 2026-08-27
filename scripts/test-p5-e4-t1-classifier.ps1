$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'p5-e4-t1-classifier.ps1')

function Entry([string]$EventName, [string]$Phase = '', [long]$Sample = -1,
               [bool]$MeasurementStart = $false) {
    return [pscustomobject]@{event=$EventName;phase=$Phase;audio_clock_media_sample=$Sample
        measurement_start=$MeasurementStart}
}

$base = @(
    (Entry 'tick.first.enter'),
    (Entry 'open_pipelines.enter'),
    (Entry 'open_pipelines.exit'),
    (Entry 'start_at_frame.enter'),
    (Entry 'start_at_frame.exit')
)
$cases = @(
    @{name='timer';entries=@((Entry 'controller.constructed'));age=180;expected='timer_not_fired'},
    @{name='pipeline';entries=@((Entry 'tick.first.enter'),(Entry 'open_pipelines.enter'))
        age=180;expected='pipeline_blocking'},
    @{name='initial';entries=@((Entry 'tick.first.enter'),(Entry 'open_pipelines.enter'),
            (Entry 'open_pipelines.exit'),(Entry 'start_at_frame.enter' '' -1 $true))
        age=180;expected='initial_start_blocking'},
    @{name='warmup';entries=$base + @(1..5 | ForEach-Object {(Entry 'heartbeat' 'Warmup' 48000)})
        age=1;expected='warmup_clock_stall'},
    @{name='playback';entries=$base + @(1..5 | ForEach-Object {(Entry 'heartbeat' 'Playback' 96000)})
        age=1;expected='playback_clock_stall'},
    @{name='event-loop';entries=$base + @((Entry 'heartbeat' 'Playback' 48000))
        age=30;expected='event_loop_starvation'}
)
foreach ($case in $cases) {
    $actual = (Get-P5E4T1HangClassification -Entries $case.entries `
        -LastWriteAgeSeconds $case.age).id
    if ($actual -ne $case.expected) {
        throw "ATTR-Q3-T1 classifier test失敗: $($case.name): expected=$($case.expected) actual=$actual"
    }
}
Write-Host "ATTR-Q3-T1 classifier: $($cases.Count)/$($cases.Count) PASS"
