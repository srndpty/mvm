[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,10)][int]$Runs=3,
    [ValidateRange(1,60)][int]$WarmupSeconds=3,
    [ValidateRange(1,60)][int]$MeasureSeconds=5,
    [ValidateRange(10,600)][int]$TimeoutSeconds=120,
    [string]$Executable=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ucrt64-release\bin\mvm_compositor_spike.exe')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

# P2-D5-2-W2-A live shadow acquisition。
#
# 目的は performance 評価ではない。physical_vblank_domain_shadow が実 observer と
# 実 measurement lifecycle の上でも exact に閉じるかだけを確認する。
#
# formal-preflight は有効にしない。legacy formal path は RENDER_SWAP_MISMATCH 系で
# 早期 shutdown し得るため、W2-A shadow の確認に混ぜない。
# incremental mapper shadow も有効にしない (独立した fail 経路を持ち込まない)。
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$repo=Split-Path -Parent $PSScriptRoot
$sourceA=Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB=Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'
$checker=Join-Path $PSScriptRoot 'check-p2-d5-2-w2a-physical-domain.ps1'
foreach($path in @($Executable,$sourceA,$sourceB,$checker)){
    if(-not(Test-Path -LiteralPath $path)){throw "W2-A live必須fileがありません: $path"}
}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存W2-A artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$Executable=(Resolve-Path -LiteralPath $Executable).Path
$env:PATH="C:\msys64\ucrt64\bin;$env:PATH"
Remove-Item Env:QSG_NO_VSYNC -ErrorAction SilentlyContinue

$results=@()
$matrixPass=$true
$failure=''
for($run=1;$run-le$Runs;++$run){
    $metrics=Join-Path $OutputDirectory "run-$run.json"
    $stdout=Join-Path $OutputDirectory "run-$run-stdout.txt"
    $stderr=Join-Path $OutputDirectory "run-$run-stderr.txt"
    $arguments=@('--source-a',$sourceA,'--source-b',$sourceB,'--metrics',$metrics,
        '--warmup-seconds',[string]$WarmupSeconds,'--measure-seconds',[string]$MeasureSeconds,
        '--seed','20260824','--seek-count','1000','--display-timeout-ms','2000',
        '--gpu-completion','fence','--mode','playback','--vblank-observer',
        '--presentation-opportunity-ring')
    $process=Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if(-not$process.WaitForExit($TimeoutSeconds*1000)){
        $process.Kill($true);$process.WaitForExit();throw "W2-A live run $run がtimeoutしました"
    }
    if(-not(Test-Path -LiteralPath $metrics)){
        throw "W2-A live run $run がmetricsを生成しませんでした (exit=$($process.ExitCode))"
    }
    $proof=Join-Path $OutputDirectory "run-$run-w2a-proof.json"
    & pwsh -NoProfile -File $checker -Json $metrics -Output $proof
    $checkerExit=$LASTEXITCODE
    $raw=Get-Content -LiteralPath $metrics -Raw -Encoding utf8|ConvertFrom-Json
    $shadow=$raw.presentation_opportunity.physical_vblank_domain_shadow
    $results+=[ordered]@{
        run=$run;metrics="run-$run.json";metrics_sha256=Hash $metrics
        process_exit_code=$process.ExitCode;checker_exit_code=$checkerExit
        shadow_authority_valid=[bool]$shadow.shadow_authority_valid
        shadow_authority_error=[string]$shadow.shadow_authority_error
        shadow_authority_canonical_reason=[string]$shadow.shadow_authority_canonical_reason
        measurement_start_qpc=[int64]$shadow.measurement_start_qpc
        measurement_end_qpc_exclusive=[int64]$shadow.measurement_end_qpc_exclusive
        predecessor_ordinal=[int64]$shadow.predecessor_ordinal
        predecessor_qpc=[int64]$shadow.predecessor_qpc
        origin_ordinal=[int64]$shadow.origin_ordinal
        last_ordinal=[int64]$shadow.last_ordinal
        successor_ordinal=[int64]$shadow.successor_ordinal
        successor_qpc=[int64]$shadow.successor_qpc
        physical_opportunity_count=[int64]$shadow.physical_opportunity_count
        required_intent_count=[int64]$shadow.required_intent_count
        intent_overhang_count=[int64]$shadow.intent_overhang_count
        intent_surplus_count=[int64]$shadow.intent_surplus_count
        sequence_status=[string]$shadow.sequence_status
        long_interval_count=[int64]$shadow.long_interval_count
        short_interval_count=[int64]$shadow.short_interval_count
        ring_overflow_count=[int64]$shadow.ring_overflow_count
        wait_failure_count=[int64]$shadow.wait_failure_count
        cumulative_consistent=[bool]$shadow.cumulative_consistent
        output_stable=[bool]$shadow.output_stable
        boundary_bracketed=[bool]$shadow.boundary_bracketed
        prestart_vblank_preroll_completed=[bool]$shadow.prestart_vblank_preroll_completed
        prestart_vblank_preroll_timeout=[bool]$shadow.prestart_vblank_preroll_timeout
        prestart_vblank_sample_ordinal=[int64]$shadow.prestart_vblank_sample_ordinal
        prestart_vblank_sample_qpc=[int64]$shadow.prestart_vblank_sample_qpc
        prestart_wait_elapsed_qpc=[int64]$shadow.prestart_wait_elapsed_qpc
        # 確認する不変量はordinalではなくこれ。
        preroll_sample_before_measurement_start=([int64]$shadow.prestart_vblank_sample_qpc-lt[int64]$shadow.measurement_start_qpc)
    }
    if($checkerExit-ne0-or-not[bool]$shadow.shadow_authority_valid){
        $matrixPass=$false
        $failure="run $run がfail-closedしました (checker=$checkerExit error=$($shadow.shadow_authority_error))"
        break
    }
}
$summary=[ordered]@{
    schema='mvm-p2-d5-2-w2a-live-shadow-1'
    stage='P2-D5-2-W2-A'
    # shadow only。legacy formal scheduler / counters / shutdown / threshold は不変。
    shadow_only=$true
    formal_preflight_used=$false
    performance_evaluated=$false
    executable_sha256=Hash $Executable
    runs=$Runs;warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds
    matrix_pass=$matrixPass
    verdict=$(if($matrixPass){'PHYSICAL_VBLANK_DOMAIN_SHADOW_EXACT'}else{'AUTHORITY_INVALID'})
    failure=$failure
    results=$results
}
$summaryPath=Join-Path $OutputDirectory 'w2a-live-summary.json'
$summary|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $summaryPath -Encoding utf8
if(-not$matrixPass){Write-Error $failure;exit 1}
Write-Host "P2-D5-2 W2-A live shadow: PASS ($Runs/$Runs) PHYSICAL_VBLANK_DOMAIN_SHADOW_EXACT"
