Set-StrictMode -Version Latest

# W3 runner / checker が共有する評価手順。2 箇所に書かない。
#
#   1. W2-E canonical authority checker を再実行する
#      (その中で W2-D / W2-A / B1 / B2 / C1 / C2.1 / C2 / C2.4 も再実行される)
#   2. canonical artifact と acquisition provenance から measurement window を集める
#   3. staged evaluation (acquisition -> authority -> accounting -> metric -> threshold -> verdict)

. (Join-Path $PSScriptRoot 'p2-d5-2-w3-canonical-performance-core.ps1')

function Invoke-MvmW3CanonicalPerformanceFromCanonical {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][string]$AcquisitionProvenancePath,
        [Parameter(Mandatory=$true)][string]$CanonicalProofPath,
        [string]$ExpectedCheckpointSha,
        [Parameter(Mandatory=$true)][string]$SourceRoot,
        [Parameter(Mandatory=$true)][string]$W2EChecker,
        [Parameter(Mandatory=$true)][string]$WorkDirectory
    )
    foreach($path in @($AcquisitionProvenancePath,$CanonicalProofPath,$W2EChecker,$SourceRoot)){
        if(-not(Test-Path -LiteralPath $path)){throw "W3 upstream inputがありません: $path"}
    }
    if(-not(Test-Path -LiteralPath $WorkDirectory)){New-Item -ItemType Directory -Path $WorkDirectory|Out-Null}

    $provenance=Get-Content -LiteralPath $AcquisitionProvenancePath -Raw -Encoding utf8|ConvertFrom-Json
    if([string]$provenance.schema-ne'mvm-p2-d5-2-w3-acquisition-provenance-1'){
        throw 'W3 acquisition provenance schemaが不正です'
    }
    # 明示が無ければ acquisition が記録した checkpoint を期待値にする。
    # この場合でも stage 1 は clean worktree / fresh / binary provenance を検査する。
    if([string]::IsNullOrWhiteSpace($ExpectedCheckpointSha)){
        $ExpectedCheckpointSha=[string]$provenance.checkpoint_sha
    }

    # 1. canonical authority を再実行する。W2-E artifact の値をそのまま信じない。
    & pwsh -NoProfile -File $W2EChecker -Proof $CanonicalProofPath -SourceRoot $SourceRoot `
        -WorkDirectory (Join-Path $WorkDirectory 'w2e-check') *> $null
    if($LASTEXITCODE-ne0){throw 'W3がconsumeするW2-E canonical authorityが不成立です'}
    $canonical=Get-Content -LiteralPath $CanonicalProofPath -Raw -Encoding utf8|ConvertFrom-Json

    # 2. measurement window。fps の母数は W2-A の physical domain 窓から取る。
    #    legacy ledger の経過秒や configured measure seconds は使わない。
    $cohortDirectory=[string]$provenance.cohort_directory
    $windows=@()
    for($run=1;$run-le[int]$provenance.run_count;++$run){
        $appPath=Join-Path $cohortDirectory "run-$run\traced-app.json"
        if(-not(Test-Path -LiteralPath $appPath)){throw "run $run のtraced-appがありません: $appPath"}
        $app=Get-Content -LiteralPath $appPath -Raw -Encoding utf8|ConvertFrom-Json
        $shadow=$app.presentation_opportunity.physical_vblank_domain_shadow
        $windows+=,[ordered]@{
            run=$run
            measurement_start_qpc=[int64]$shadow.measurement_start_qpc
            measurement_end_qpc_exclusive=[int64]$shadow.measurement_end_qpc_exclusive
            qpc_frequency=[int64]$app.formal_qpc_frequency
        }
    }

    # 3. staged evaluation。
    $result=Invoke-MvmW3CanonicalPerformance -AcquisitionProvenance $provenance `
        -CanonicalAuthority $canonical -MeasurementWindows $windows `
        -ExpectedCheckpointSha $ExpectedCheckpointSha
    $result.source_acquisition_provenance=(Resolve-Path -LiteralPath $AcquisitionProvenancePath).Path
    $result.source_acquisition_provenance_sha256=(Get-FileHash -LiteralPath $AcquisitionProvenancePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $result.source_canonical_proof=(Resolve-Path -LiteralPath $CanonicalProofPath).Path
    $result.source_canonical_proof_sha256=(Get-FileHash -LiteralPath $CanonicalProofPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $result.measurement_windows=$windows
    return $result
}

function Assert-MvmW3Proof {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)]$Expected,[Parameter(Mandatory=$true)]$Actual)
    $expectedJson=$Expected|ConvertTo-Json -Depth 16 -Compress
    $actualJson=$Actual|ConvertTo-Json -Depth 16 -Compress
    if($expectedJson-ne$actualJson){throw 'W3 artifactがcanonical authorityからの再評価結果と一致しません'}
}
