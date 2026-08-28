param([Parameter(Mandatory=$true)][string]$RepoRoot)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest

function Read-Source([string]$RelativePath){Get-Content -Raw -LiteralPath (Join-Path $RepoRoot $RelativePath)}
function Require([string]$Text,[string]$Pattern,[string]$Message){if($Text-notmatch$Pattern){throw $Message}}
function Reject([string]$Text,[string]$Pattern,[string]$Message){if($Text-match$Pattern){throw $Message}}

$join=Read-Source 'src/media/gpu_preview/qualified_present_commit_join.cpp'
$renderer=Read-Source 'src/app/preview/compositor_rhi_item.cpp'
$controller=Read-Source 'apps/compositor_spike/compositor_spike_controller.cpp'

foreach($predicate in @(
    'BindTokenPresent','BindIntentOrdinalExact','BindTokenSerialEqualsReservation',
    'CommitTokenSerialEqualsReservation')){
    Require $join ([regex]::Escape("QualifiedCommitFailurePredicate::$predicate")) `
        "COMPOSITION_TOKEN_MISMATCH predicate inventoryが不足しています: $predicate"
}
Require $join 'BindTokenPresent,[\s\S]+CompositionTokenMismatch[\s\S]+BindIntentOrdinalExact,[\s\S]+CompositionTokenMismatch[\s\S]+BindTokenSerialEqualsReservation,[\s\S]+CompositionTokenMismatch' 'BIND_NATIVE_PRESENTのmismatch predicateが個別記録されません'
Require $join 'CommitTokenSerialEqualsReservation,[\s\S]+CompositionTokenMismatch' 'COMMIT_FRAME_SWAPPEDのmismatch predicateが個別記録されません'
Require $renderer 'failure\.join = joinAttribution[\s\S]+failure\.nativeRecord = nativeRecord[\s\S]+failure\.receipt = receipt' 'fatal時のraw join evidenceを保存していません'
Require $renderer 'captureJoinFailureAttribution\([\s\S]+runtimeAttribution\(\)' 'join runtime attributionをraw snapshotへ接続していません'
Require $renderer 'PreJoinBoundarySwap[\s\S]+BoundarySwapRequiresNoActiveReservation[\s\S]+captureJoinFailureAttribution' 'pre-join boundary fatalのraw evidenceを保存していません'
Require $controller 'failure_phase[\s\S]+failure_predicate[\s\S]+reservation[\s\S]+native_present_record[\s\S]+frame_swapped_receipt[\s\S]+latest_set_token_publication[\s\S]+lifetime_checks' 'B3-I2 attribution payloadが不足しています'
Require $controller 'composition_token_runtime_attribution", compositionTokenRuntimeAttribution' 'artifactへB3-I2 attributionを出力していません'
Require $controller 'diagnostic_only", true[\s\S]+identity_authority", false[\s\S]+nearest_latest_fallback_used", false[\s\S]+serial_inference_used", false' 'B3-I2 attributionがdiagnostic-onlyとして固定されていません'
Require $renderer 'failQualifiedJoin\([\s\S]+QualifiedCommitResult::QualifiedCommit' 'mismatchのFATAL経路を保持していません'
Reject $join '(?i)((nearest|latest)(Record|Present)\s*\(|expectedPresentSerial_\s*=\s*[^;]+\+\s*1|qpcProximity\s*\()' 'joinがnearest/latest/fallback/serial推定を使用しています'

Write-Output 'P2-D5-2 B3-I2 composition token attribution architecture: PASS'
