[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OracleDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$Checker=(Join-Path $PSScriptRoot 'check-p2-c2-display-discard-attribution.ps1')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Fail([string]$Message){throw $Message}
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
if(-not(Test-Path -LiteralPath $OracleDirectory)){Fail "oracle artifactがありません: $OracleDirectory"}
$OracleDirectory=(Resolve-Path -LiteralPath $OracleDirectory).Path
$manifest=Join-Path $OracleDirectory 'manifest.sha256'
if(-not(Test-Path -LiteralPath $manifest)){Fail "oracle manifestがありません: $manifest"}
foreach($line in Get-Content -LiteralPath $manifest -Encoding ascii){
    if($line-notmatch'^([0-9a-fA-F]{64})  (.+)$'){Fail "manifest行が不正です: $line"}
    $path=Join-Path $OracleDirectory $Matches[2]
    if(-not(Test-Path -LiteralPath $path)-or(Hash $path)-ne$Matches[1].ToLowerInvariant()){Fail "artifact hash検証に失敗しました: $path"}
}
if(Test-Path -LiteralPath $OutputDirectory){Fail "既存F3-C2 artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$oracle=Join-Path $OracleDirectory 'oracle.json';$proof=Join-Path $OutputDirectory 'proof.json'
& pwsh -NoProfile -File $Checker -OracleJson $oracle -Output $proof
if($LASTEXITCODE-ne0){Fail "F3-C2 offline attributionが失敗しました: $LASTEXITCODE"}
$result=Get-Content -LiteralPath $proof -Raw -Encoding utf8|ConvertFrom-Json
[ordered]@{
    schema='mvm-p2-c2-display-discard-attribution-run-1';status=[string]$result.proof_status
    authority='diagnostic_offline_proof';formal_counter_authority_changed=$false
    source_artifact_reacquired=$false;present_record_count=[int64]$result.present_record_count
    presented_count=[int64]$result.presented_count;discarded_count=[int64]$result.discarded_count
    physical_gap_accounting_exact=[bool]$result.physical_gap_accounting_exact
    source_frame_is_exact_physical_timeline=[bool]$result.source_frame_is_exact_physical_timeline
    source_vblank_delta_mismatch_count=[int64]$result.source_vblank_delta_mismatch_count
    source_identities=[ordered]@{oracle_sha256=Hash $oracle;oracle_manifest_sha256=Hash $manifest;checker_sha256=Hash $Checker}
}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
$outputManifest=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$outputManifest}|Sort-Object Name|ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}|Set-Content -LiteralPath $outputManifest -Encoding ascii
Write-Host "F3-C2 offline attribution run: PASS ($OutputDirectory)"
