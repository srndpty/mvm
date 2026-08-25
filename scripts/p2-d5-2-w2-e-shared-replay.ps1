Set-StrictMode -Version Latest

# W2-E runner / checker が共有する canonical cutover 手順。
# 2 箇所に書くと片方だけ通るという最悪の形で食い違うため一本化する。
#
#   1. W2-D checker を再実行する (W2-D が再実行する upstream checker 群もここで走る)
#   2. legacy authority retirement inventory を再実行する
#   3. W2-D artifact の boolean を反転させるのではなく、同じ sealed authority から
#      formal-v2 integration を独立再構築する
#   4. canonical statement を組む

. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c1-mapping-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-c13-formal-population-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-d-formal-v2-shadow-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-d-from-authorities-core.ps1')
. (Join-Path $PSScriptRoot 'p2-d5-2-w2-e-canonical-authority-core.ps1')

function Invoke-MvmECanonicalAuthorityFromW2D {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][string]$W2DProofPath,
        [Parameter(Mandatory=$true)][string]$SourceRoot,
        [Parameter(Mandatory=$true)][string]$W2DChecker,
        [Parameter(Mandatory=$true)][string]$RetirementInventory,
        [Parameter(Mandatory=$true)][string]$C1Checker,
        [Parameter(Mandatory=$true)][string]$C21Checker,
        [Parameter(Mandatory=$true)][string]$C2Checker,
        [Parameter(Mandatory=$true)][string]$C24Checker,
        [Parameter(Mandatory=$true)][string]$W2AChecker,
        [Parameter(Mandatory=$true)][string]$B2Checker,
        [Parameter(Mandatory=$true)][string]$WorkDirectory
    )
    foreach($path in @($W2DProofPath,$W2DChecker,$RetirementInventory,$SourceRoot)){
        if(-not(Test-Path -LiteralPath $path)){throw "W2-E upstream authorityがありません: $path"}
    }
    if(-not(Test-Path -LiteralPath $WorkDirectory)){New-Item -ItemType Directory -Path $WorkDirectory|Out-Null}

    # 1. W2-D shadow authority の再実行。
    & pwsh -NoProfile -File $W2DChecker -Proof $W2DProofPath -SourceRoot $SourceRoot `
        -C1Checker $C1Checker -C21Checker $C21Checker -C2Checker $C2Checker `
        -C24Checker $C24Checker -W2AChecker $W2AChecker -B2Checker $B2Checker `
        -WorkDirectory (Join-Path $WorkDirectory 'w2d-check') *> $null
    if($LASTEXITCODE-ne0){throw 'W2-EがconsumeするW2-D formal-v2 shadow authorityが不成立です'}

    # 2. legacy presentation authority の retirement completeness。
    $retirementPath=Join-Path $WorkDirectory 'w2-e-legacy-authority-retirement.json'
    & pwsh -NoProfile -File $RetirementInventory -SourceRoot $SourceRoot -Output $retirementPath *> $null
    if($LASTEXITCODE-ne0){throw 'W2-E legacy authority retirementが不成立です'}
    $retirement=Get-Content -LiteralPath $retirementPath -Raw -Encoding utf8|ConvertFrom-Json

    # 3. 同じ sealed authority から formal-v2 integration を独立再構築する。
    $w2d=Get-Content -LiteralPath $W2DProofPath -Raw -Encoding utf8|ConvertFrom-Json
    foreach($sourceField in @('source_c1_proof','source_c21_proof','source_c2_proof')){
        if($w2d.PSObject.Properties.Name-notcontains$sourceField){throw "W2-D provenanceがありません: $sourceField"}
    }
    $c1Path=[string]$w2d.source_c1_proof
    $c21Path=[string]$w2d.source_c21_proof
    $c2Path=[string]$w2d.source_c2_proof
    $c1=Get-Content -LiteralPath $c1Path -Raw -Encoding utf8|ConvertFrom-Json
    $c21=Get-Content -LiteralPath $c21Path -Raw -Encoding utf8|ConvertFrom-Json
    $c2=Get-Content -LiteralPath $c2Path -Raw -Encoding utf8|ConvertFrom-Json
    $replay=Invoke-MvmDUpstreamAuthorityReplay -C1ProofObject $c1 -C1ProofPath $c1Path `
        -C21ProofPath $c21Path -C2ProofPath $c2Path -C1Checker $C1Checker -C21Checker $C21Checker `
        -C2Checker $C2Checker -C24Checker $C24Checker -W2AChecker $W2AChecker -B2Checker $B2Checker `
        -SourceRoot $SourceRoot -WorkDirectory (Join-Path $WorkDirectory 'w2e-replay')
    $integration=Invoke-MvmDFormalV2ProofFromSealedAuthorities -C1ProofObject $c1 -C1ProofPath $c1Path `
        -C21ProofObject $c21 -C21ProofPath $c21Path -C2ProofObject $c2 -C2ProofPath $c2Path `
        -UpstreamReplay $replay -C1CheckpointSha '5034bfcd41dd9f5c860827a9594b604be5db7446'

    # 4. canonical statement。W2-D artifact の verdict はそのまま運び、書き換えない。
    return Invoke-MvmECanonicalAuthority -FormalV2Integration $integration `
        -W2DProofPath (Resolve-Path -LiteralPath $W2DProofPath).Path `
        -W2DProofSha256 ((Get-FileHash -LiteralPath $W2DProofPath -Algorithm SHA256).Hash.ToLowerInvariant()) `
        -W2DVerdict ([string]$w2d.verdict) -RetirementInventory $retirement
}
