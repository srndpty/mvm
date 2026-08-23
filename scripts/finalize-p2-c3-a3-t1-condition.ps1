[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)]
    [ValidateSet('VISIBLE_UNOCCLUDED','FULLY_OCCLUDED','VISIBLE_UNOCCLUDED_FORCE_DIRTY')]
    [string]$Mode,
    [ValidateRange(12,300)][int]$WarmupSeconds,
    [ValidateRange(1,300)][int]$MeasureSeconds,
    [string]$Controller=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\ucrt64-release\bin\mvm_p2_window_state_controller.exe')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$canonicalRunner=Join-Path $PSScriptRoot 'p2-c0-native-etw.ps1'
$submissionChecker=Join-Path $PSScriptRoot 'check-p2-c3-submission-backpressure.ps1'
$conditionChecker=Join-Path $PSScriptRoot 'check-p2-c3-a3-t1-condition.ps1'
$canonical=Join-Path $OutputDirectory 'canonical';$stateJson=Join-Path $OutputDirectory 'window-state-raw.json'
$submissionProof=Join-Path $OutputDirectory 'submission-proof.json';$conditionProof=Join-Path $OutputDirectory 'condition-proof.json'
$summaryPath=Join-Path $OutputDirectory 'summary.json'
foreach($path in @($canonicalRunner,$submissionChecker,$conditionChecker,$Controller,$canonical,$stateJson)){
    if(-not(Test-Path -LiteralPath $path)){throw "T1 finalize必須pathがありません: $path"}
}
if(Test-Path -LiteralPath $summaryPath){throw "既存T1 condition summaryを上書きしません: $summaryPath"}
if(-not(Test-Path -LiteralPath $submissionProof)){
    & pwsh -NoProfile -File $submissionChecker -OracleJson (Join-Path $canonical 'oracle.json') `
        -AppJson (Join-Path $canonical 'traced-app.json') -SubmissionMode CONTROL -Output $submissionProof
    if($LASTEXITCODE-ne0){throw 'submission proofが失敗しました'}
}
if(Test-Path -LiteralPath $conditionProof){throw "既存condition proofを上書きしません: $conditionProof"}
& pwsh -NoProfile -File $conditionChecker -CanonicalDirectory $canonical -WindowStateJson $stateJson `
    -SubmissionProofJson $submissionProof -ExpectedMode $Mode -Output $conditionProof
if($LASTEXITCODE-ne0){throw 'window-state/DWM condition proofが失敗しました'}
$proof=Get-Content -LiteralPath $conditionProof -Raw -Encoding utf8|ConvertFrom-Json
[ordered]@{
    schema='mvm-p2-c3-a3-t1-condition-run-1';status='PASS';authority='diagnostic_only';mode=$Mode
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false;production_scheduler_changed=$false
    warmup_seconds=$WarmupSeconds;measure_seconds=$MeasureSeconds;proof=$proof
    identities=[ordered]@{
        controller_sha256=Hash $Controller;canonical_runner_sha256=Hash $canonicalRunner
        submission_checker_sha256=Hash $submissionChecker;condition_checker_sha256=Hash $conditionChecker
        canonical_manifest_sha256=Hash (Join-Path $canonical 'manifest.sha256')
        window_state_raw_sha256=Hash $stateJson;submission_proof_sha256=Hash $submissionProof
        condition_proof_sha256=Hash $conditionProof
    }
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath $summaryPath -Encoding utf8
$manifest=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$manifest}|Sort-Object Name|ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}|Set-Content -LiteralPath $manifest -Encoding ascii
Write-Host "F3-C3-A3-T1 condition finalize: PASS mode=$Mode ($OutputDirectory)"
