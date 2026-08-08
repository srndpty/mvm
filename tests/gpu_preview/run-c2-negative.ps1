param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$SourceA,
    [Parameter(Mandatory)][string]$SourceB,
    [Parameter(Mandatory)][string]$Metrics,
    [Parameter(Mandatory)][ValidateSet('device_change','completion_fatal')][string]$Fault
)
$ErrorActionPreference = 'Stop'
& $Exe --source-a $SourceA --source-b $SourceB `
    --warmup-seconds 0 --measure-seconds 1 --seed 20260808 --seek-count 1 `
    --metrics $Metrics --gpu-completion fence --display-timeout-ms 2000 `
    --mode playback --test-fault $Fault
$actual = $LASTEXITCODE
if ($actual -ne 3) { throw "C2 negative testの終了コードが不正です: actual=$actual expected=3" }
if (-not (Test-Path -LiteralPath $Metrics)) { throw "fatal後のmetricsがありません: $Metrics" }
$m = Get-Content -Raw -LiteralPath $Metrics | ConvertFrom-Json
if (-not $m.teardown_success -or -not $m.final_report_written_after_teardown) {
    throw 'fatal後のshutdown/report契約が不成立です'
}
