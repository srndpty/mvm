[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$Executable = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ucrt64-release\bin\mvm_compositor_spike.exe'),
    [string]$Decoder = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\p2-etw-decoder\mvm_present_history_decoder.exe'),
    [ValidateRange(1, 300)][int]$WarmupSeconds = 5,
    [ValidateRange(1, 300)][int]$MeasureSeconds = 15,
    [ValidateRange(10, 600)][int]$TimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$sourceA = Join-Path $repo 'tests\assets\benchmark\v1080p60_h264.mp4'
$sourceB = Join-Path $repo 'tests\assets\benchmark\v1080p60_hevc.mp4'
$checker = Join-Path $PSScriptRoot 'check-p2-etw-present-history.ps1'
$vblankChecker = Join-Path $PSScriptRoot 'check-p2-vblank-shadow.ps1'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'ETW Present-History採取には管理者権限が必要です。昇格したPowerShellから実行してください'
}
foreach ($path in @($Executable, $Decoder, $sourceA, $sourceB, $checker, $vblankChecker)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "R2必須ファイルがありません: $path" }
}
if (Test-Path -LiteralPath $OutputDirectory) { throw "既存artifactを上書きしません: $OutputDirectory" }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$Decoder = (Resolve-Path -LiteralPath $Decoder).Path
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"

function Get-Sha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Invoke-Compositor([string]$Metrics, [string]$Stdout, [string]$Stderr,
                           [int]$TimeoutMilliseconds) {
    $arguments = @(
        '--source-a', $sourceA, '--source-b', $sourceB, '--metrics', $Metrics,
        '--warmup-seconds', [string]$WarmupSeconds, '--measure-seconds', [string]$MeasureSeconds,
        '--seed', '20260808', '--seek-count', '1000', '--display-timeout-ms', '2000',
        '--gpu-completion', 'fence', '--mode', 'playback',
        '--vblank-observer', '--presentation-opportunity-ring'
    )
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru `
        -RedirectStandardOutput $Stdout -RedirectStandardError $Stderr
    if (-not $process.WaitForExit($TimeoutMilliseconds)) {
        $process.Kill($true)
        $process.WaitForExit()
        return [ordered]@{ process=$process; exit_code=124 }
    }
    return [ordered]@{ process=$process; exit_code=$process.ExitCode }
}

function Get-SwapRate([string]$Json) {
    $raw = Get-Content -LiteralPath $Json -Raw -Encoding utf8 | ConvertFrom-Json
    $opportunity = $raw.presentation_opportunity
    $elapsed = ([double][long]$opportunity.measurement_end_qpc_exclusive -
                [double][long]$opportunity.measurement_start_qpc) / [double][long]$opportunity.qpc_frequency
    if ($elapsed -le 0) { throw "measurement elapsedが不正です: $Json" }
    return [double][long]$opportunity.swap_record_count / $elapsed
}

$baselineJson = Join-Path $OutputDirectory 'baseline-app.json'
$baselineRun = Invoke-Compositor $baselineJson (Join-Path $OutputDirectory 'baseline-stdout.txt') `
    (Join-Path $OutputDirectory 'baseline-stderr.txt') ($TimeoutSeconds * 1000)
if ($baselineRun.exit_code -ne 0 -or -not (Test-Path -LiteralPath $baselineJson)) {
    throw "未計測baseline appが失敗しました: $($baselineRun.exit_code)"
}
$baselineRate = Get-SwapRate $baselineJson

$traceJson = Join-Path $OutputDirectory 'traced-app.json'
$etlPath = Join-Path $OutputDirectory 'trace.etl'
$etwJson = Join-Path $OutputDirectory 'present-history-raw.json'
$oracleJson = Join-Path $OutputDirectory 'oracle.json'
$traceStarted = $false
$traceRun = $null
try {
    & wpr.exe -start GeneralProfile -start GPU -start DesktopComposition -filemode
    if ($LASTEXITCODE -ne 0) { throw 'WPR startに失敗しました' }
    $traceStarted = $true
    & wpr.exe -marker 'F3-B0.6-R2 compositor-start' | Out-Null
    $traceRun = Invoke-Compositor $traceJson (Join-Path $OutputDirectory 'traced-stdout.txt') `
        (Join-Path $OutputDirectory 'traced-stderr.txt') ($TimeoutSeconds * 1000)
} finally {
    if ($traceStarted) {
        & wpr.exe -marker 'F3-B0.6-R2 compositor-end' | Out-Null
        & wpr.exe -stop $etlPath 'F3-B0.6-R2 ETW Present-History Oracle'
        if ($LASTEXITCODE -ne 0) { throw 'WPR stopに失敗しました' }
    }
}
if ($null -eq $traceRun -or $traceRun.exit_code -ne 0 -or -not (Test-Path -LiteralPath $traceJson)) {
    throw "ETW計測中のappが失敗しました: $(if ($traceRun) { $traceRun.exit_code } else { '未起動' })"
}

& $Decoder --etl $etlPath --process-id $traceRun.process.Id --output $etwJson
$decoderExit = $LASTEXITCODE
if ($decoderExit -ne 0 -and $decoderExit -ne 5) { throw "raw ETL decodeに失敗しました: $decoderExit" }
if (-not (Test-Path -LiteralPath $etwJson)) { throw 'raw ETW JSONが生成されませんでした' }
$tracedRate = Get-SwapRate $traceJson
$ratio = if ($baselineRate -gt 0) { $tracedRate / $baselineRate } else { $null }
$etw = Get-Content -LiteralPath $etwJson -Raw -Encoding utf8 | ConvertFrom-Json
$etw.cadence_diagnostic.traced_swaps_per_second = $tracedRate
$etw.cadence_diagnostic.baseline_swaps_per_second = $baselineRate
$etw.cadence_diagnostic.ratio = $ratio
$etw.cadence_diagnostic.extreme_change = if ($null -eq $ratio) { $null } else { $ratio -lt 0.5 -or $ratio -gt 1.5 }
$etw | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $etwJson -Encoding utf8

& pwsh -NoProfile -File $checker -AppJson $traceJson -EtwJson $etwJson -Output $oracleJson `
    -VBlankChecker $vblankChecker -ProcessExitCode $traceRun.exit_code
$checkerExit = $LASTEXITCODE
$oracle = if ($checkerExit -eq 0 -and (Test-Path -LiteralPath $oracleJson)) {
    Get-Content -LiteralPath $oracleJson -Raw -Encoding utf8 | ConvertFrom-Json
} else { $null }

[ordered]@{
    schema = 'mvm-p2-etw-present-history-run-1'
    authority = 'diagnostic_only'
    oracle_status = if ($null -ne $oracle) { $oracle.oracle_status } else { 'ORACLE_INVALID' }
    collision_evidence_status = if ($null -ne $oracle) {
        $oracle.collision_evidence_status
    } else { 'NOT_EVALUABLE' }
    r2_exit_status = if ($null -ne $oracle) { $oracle.r2_exit_status } else { 'INVALID' }
    mapper_proof_status = 'NOT_YET_EVALUABLE'
    mapper_changed = $false
    presentmon_commit = '717c5bf14e80a4a06b70cd16415ae8d40a7ce201'
    process_id = $traceRun.process.Id
    decoder_exit_code = $decoderExit
    checker_exit_code = $checkerExit
    conditions = [ordered]@{
        warmup_seconds = $WarmupSeconds; measure_seconds = $MeasureSeconds
        seed = 20260808; sync_interval_required = 1
        wpr_profiles = @('GeneralProfile', 'GPU', 'DesktopComposition')
    }
    cadence_diagnostic = $etw.cadence_diagnostic
    identities = [ordered]@{
        executable_sha256 = Get-Sha256 $Executable
        decoder_sha256 = Get-Sha256 $Decoder
        source_a_sha256 = Get-Sha256 $sourceA
        source_b_sha256 = Get-Sha256 $sourceB
        etl_sha256 = Get-Sha256 $etlPath
        raw_etw_json_sha256 = Get-Sha256 $etwJson
    }
} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8

$manifestPath = Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File | Where-Object FullName -ne $manifestPath |
    Sort-Object Name | ForEach-Object { "$(Get-Sha256 $_.FullName)  $($_.Name)" } |
    Set-Content -LiteralPath $manifestPath -Encoding ascii
if ($checkerExit -ne 0) { throw 'ETW Present-History oracleはINVALIDです。mapperは未評価のままです' }
Write-Host "ETW Present-History oracle: ORACLE_VALID / $($oracle.collision_evidence_status) ($OutputDirectory)"
