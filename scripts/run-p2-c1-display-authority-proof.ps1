[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SourceDirectory,
    [Parameter(Mandatory=$true)][string]$OracleDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$Checker=(Join-Path $PSScriptRoot 'check-p2-c1-display-authority.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Fail([string]$Message){throw $Message}
function Verify-Artifact([string]$Directory){
    if(-not(Test-Path -LiteralPath $Directory)){Fail "artifactがありません: $Directory"}
    $resolved=(Resolve-Path -LiteralPath $Directory).Path
    $manifest=Join-Path $resolved 'manifest.sha256'
    if(-not(Test-Path -LiteralPath $manifest)){Fail "manifestがありません: $manifest"}
    foreach($line in Get-Content -LiteralPath $manifest -Encoding ascii){
        if($line-notmatch'^([0-9a-fA-F]{64})  (.+)$'){Fail "manifest行が不正です: $line"}
        $path=Join-Path $resolved $Matches[2]
        if(-not(Test-Path -LiteralPath $path)-or(Hash $path)-ne$Matches[1].ToLowerInvariant()){
            Fail "artifact hash検証に失敗しました: $path"
        }
    }
    return $resolved
}
$SourceDirectory=Verify-Artifact $SourceDirectory
$OracleDirectory=Verify-Artifact $OracleDirectory
if(Test-Path -LiteralPath $OutputDirectory){Fail "既存F3-C1 artifactを上書きしません: $OutputDirectory"}
if(-not(Test-Path -LiteralPath $Checker)){Fail "F3-C1 checkerがありません: $Checker"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$appPath=Join-Path $SourceDirectory 'traced-app.json'
$oraclePath=Join-Path $OracleDirectory 'oracle.json'
$proofPath=Join-Path $OutputDirectory 'proof.json'
& pwsh -NoProfile -File $Checker -AppJson $appPath -OracleJson $oraclePath -Output $proofPath
if($LASTEXITCODE-ne0){Fail "F3-C1 display authority proofが失敗しました: $LASTEXITCODE"}
$proof=Get-Content -LiteralPath $proofPath -Raw -Encoding utf8|ConvertFrom-Json
[ordered]@{
    schema='mvm-p2-c1-display-authority-run-1';status=[string]$proof.proof_status
    authority='diagnostic_offline_proof';formal_counter_authority_changed=$false
    source_domain_size=[int64]$proof.source_domain_size
    displayed_unique_source_frames=[int64]$proof.displayed_unique_source_frames
    formal_source_frame_drops=[int64]$proof.formal_source_frame_drops
    source_frame_accounting_exact=[bool]$proof.source_frame_accounting_exact
    source_identities=[ordered]@{
        app_sha256=Hash $appPath;oracle_sha256=Hash $oraclePath
        source_manifest_sha256=Hash (Join-Path $SourceDirectory 'manifest.sha256')
        oracle_manifest_sha256=Hash (Join-Path $OracleDirectory 'manifest.sha256')
        checker_sha256=Hash $Checker
    }
}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
$manifestPath=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$manifestPath}|Sort-Object Name|ForEach-Object{
    "$(Hash $_.FullName)  $($_.Name)"
}|Set-Content -LiteralPath $manifestPath -Encoding ascii
Write-Host "F3-C1 display authority run: PASS ($OutputDirectory)"
