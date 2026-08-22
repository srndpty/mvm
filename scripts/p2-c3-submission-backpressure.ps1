[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,300)][int]$MeasureSeconds=15,
    [ValidateRange(30,600)][int]$TimeoutSeconds=180,
    [ValidateSet('CONTROL_FRAME_DWM','FRAME_DWM_CONTROL','DWM_CONTROL_FRAME')]
    [string]$Order='CONTROL_FRAME_DWM',
    [string]$Decoder=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\p2-etw-decoder\mvm_present_history_decoder.exe')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$runner=Join-Path $PSScriptRoot 'p2-c0-native-etw.ps1'
$checker=Join-Path $PSScriptRoot 'check-p2-c3-submission-backpressure.ps1'
foreach($path in @($runner,$checker,$Decoder)){if(-not(Test-Path -LiteralPath $path)){throw "F3-C3-A必須pathがありません: $path"}}
$Modes=switch($Order){
    'CONTROL_FRAME_DWM'{@('CONTROL','FRAME_LATENCY_1','DWM_FLUSH_AFTER_PRESENT')}
    'FRAME_DWM_CONTROL'{@('FRAME_LATENCY_1','DWM_FLUSH_AFTER_PRESENT','CONTROL')}
    'DWM_CONTROL_FRAME'{@('DWM_FLUSH_AFTER_PRESENT','CONTROL','FRAME_LATENCY_1')}
}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存F3-C3-A artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$results=@()
foreach($mode in $Modes){
    $canonical=Join-Path $OutputDirectory $mode.ToLowerInvariant()
    & pwsh -NoProfile -File $runner -OutputDirectory $canonical -Decoder $Decoder `
        -MeasureSeconds $MeasureSeconds -TimeoutSeconds $TimeoutSeconds -SubmissionMode $mode
    if($LASTEXITCODE-ne0){throw "F3-C3-A $mode canonical runが失敗しました: $LASTEXITCODE"}
    $proof=Join-Path $OutputDirectory ("{0}-causal-proof.json" -f $mode.ToLowerInvariant())
    & pwsh -NoProfile -File $checker -OracleJson (Join-Path $canonical 'oracle.json') `
        -AppJson (Join-Path $canonical 'traced-app.json') -SubmissionMode $mode -Output $proof
    if($LASTEXITCODE-ne0){throw "F3-C3-A $mode causal checkerが失敗しました: $LASTEXITCODE"}
    $results+=Get-Content -LiteralPath $proof -Raw -Encoding utf8|ConvertFrom-Json
}
[ordered]@{
    schema='mvm-p2-c3-submission-backpressure-run-2';status='PASS';authority='diagnostic_only'
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false
    measure_seconds=$MeasureSeconds;execution_order=@($Modes);counterbalancing='ORDER_EXPLICIT'
    results=$results
    identities=[ordered]@{decoder_sha256=Hash $Decoder;runner_sha256=Hash $runner;checker_sha256=Hash $checker}
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
$manifest=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$manifest}|Sort-Object Name|ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}|Set-Content -LiteralPath $manifest -Encoding ascii
Write-Host "F3-C3-A Submission Backpressure Causal Proof: PASS ($OutputDirectory)"
