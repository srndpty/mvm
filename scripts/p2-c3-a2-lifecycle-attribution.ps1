[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CanonicalDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$DecoderProvenance=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\p2-etw-decoder\provenance.json')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$repo=Split-Path -Parent $PSScriptRoot
$checker=Join-Path $PSScriptRoot 'check-p2-c3-a2-lifecycle.ps1'
$app=Join-Path $CanonicalDirectory 'traced-app.json';$oracle=Join-Path $CanonicalDirectory 'oracle.json'
$canonicalSummary=Join-Path $CanonicalDirectory 'summary.json';$canonicalManifest=Join-Path $CanonicalDirectory 'manifest.sha256'
$discardPatch=Join-Path $repo 'presentmon-patches\2.3.1\0001-mvm-discard-reason-diagnostic.patch'
$lifecyclePatch=Join-Path $repo 'presentmon-patches\2.3.1\0002-mvm-dependency-lifecycle-diagnostic.patch'
foreach($path in @($checker,$app,$oracle,$canonicalSummary,$canonicalManifest,$DecoderProvenance,$discardPatch,$lifecyclePatch)){
    if(-not(Test-Path -LiteralPath $path)){throw "F3-C3-A2必須pathがありません: $path"}
}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存F3-C3-A2 attributionを上書きしません: $OutputDirectory"}
$run=Get-Content -LiteralPath $canonicalSummary -Raw -Encoding utf8|ConvertFrom-Json
if([string]$run.c0_r2_status-ne'PASS'-or[string]$run.oracle_status-ne'ORACLE_VALID'-or[string]$run.display_completion_status-ne'CLOSED'){throw 'canonical runが閉じていません'}
if([string]$run.acquisition_mode-ne'CanonicalPresentMonLive'-or[string]$run.submission_mode-ne'CONTROL'){throw 'CONTROL-only canonical runではありません'}
$provenance=Get-Content -LiteralPath $DecoderProvenance -Raw -Encoding utf8|ConvertFrom-Json
if([string]$provenance.schema-ne'mvm-p2-etw-decoder-build-3'){throw 'lifecycle decoder provenanceではありません'}
if([string]$provenance.discard_reason_patch_sha256-ne$(Hash $discardPatch)-or
   [string]$provenance.dependency_lifecycle_patch_sha256-ne$(Hash $lifecyclePatch)){throw 'PresentMon diagnostic patch provenanceが一致しません'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$proofPath=Join-Path $OutputDirectory 'lifecycle-proof.json'
& pwsh -NoProfile -File $checker -AppJson $app -OracleJson $oracle -Output $proofPath
if($LASTEXITCODE-ne0){throw 'F3-C3-A2 lifecycle checkerが失敗しました'}
$proof=Get-Content -LiteralPath $proofPath -Raw -Encoding utf8|ConvertFrom-Json
$appRaw=Get-Content -LiteralPath $app -Raw -Encoding utf8|ConvertFrom-Json
$elapsed=([double][int64]$appRaw.presentation_opportunity.measurement_end_qpc_exclusive-[double][int64]$appRaw.presentation_opportunity.measurement_start_qpc)/[double][int64]$appRaw.presentation_opportunity.qpc_frequency
[ordered]@{
    schema='mvm-p2-c3-a2-lifecycle-summary-1';status='PASS';authority='diagnostic_only'
    acquisition_mode='CanonicalPresentMonLive';submission_mode='CONTROL';measurement_seconds=$elapsed
    formal_counter_authority_changed=$false;formal_drop_threshold_changed=$false;production_scheduler_changed=$false
    present_modes=@($proof.present_modes);present_mode_transition_count=@($proof.present_mode_transitions).Count
    presented_count=[int64]$proof.presented_count;discarded_count=[int64]$proof.discarded_count
    dependency_batch_count=[int64]$proof.dependency_batch_count;max_dependency_batch_size=[int64]$proof.max_dependency_batch_size
    displayed_parent_count=[int64]$proof.displayed_parent_count;large_parent_gap_proof_count=[int64]$proof.large_parent_gap_proof_count
    legacy_dependency_identity_overwrite_count=[int64]$proof.legacy_dependency_identity_overwrite_count
    source_physical_gap_pair_count=[int64]$proof.source_physical_gap_pair_count
    source_physical_gap_exact_count=[int64]$proof.source_physical_gap_exact_count
    source_physical_gap_mismatch_count=[int64]$proof.source_physical_gap_mismatch_count
    source_physical_gap_max_abs_difference=[int64]$proof.source_physical_gap_max_abs_difference
    displayed_source_span=[int64]$proof.displayed_source_span;displayed_physical_span=[int64]$proof.displayed_physical_span
    displayed_source_minus_physical_span=[int64]$proof.displayed_source_minus_physical_span
    branch=[string]$proof.branch;next='F3-C3-A3_DWM_CONSUMPTION_STALL_ATTRIBUTION'
    identities=[ordered]@{
        canonical_summary_sha256=Hash $canonicalSummary;canonical_manifest_sha256=Hash $canonicalManifest
        app_json_sha256=Hash $app;oracle_json_sha256=Hash $oracle;lifecycle_proof_sha256=Hash $proofPath
        decoder_provenance_sha256=Hash $DecoderProvenance;discard_reason_patch_sha256=Hash $discardPatch
        dependency_lifecycle_patch_sha256=Hash $lifecyclePatch;checker_sha256=Hash $checker
    }
}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
$manifest=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$manifest}|Sort-Object Name|ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}|Set-Content -LiteralPath $manifest -Encoding ascii
Write-Host "F3-C3-A2 attribution: PASS branch=$($proof.branch) ($OutputDirectory)"
