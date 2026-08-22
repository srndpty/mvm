[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateRange(1,300)][int]$MeasureSeconds=15,
    [ValidateRange(30,600)][int]$TimeoutSeconds=180,
    [string]$Decoder=(Join-Path (Split-Path -Parent $PSScriptRoot) 'build\p2-etw-decoder\mvm_present_history_decoder.exe')
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
$repo=Split-Path -Parent $PSScriptRoot
$runner=Join-Path $PSScriptRoot 'p2-c0-native-etw.ps1';$checker=Join-Path $PSScriptRoot 'check-p2-c2-discard-reasons.ps1'
$provenance=Join-Path $repo 'build\p2-etw-decoder\provenance.json';$patch=Join-Path $repo 'presentmon-patches\2.3.1\0001-mvm-discard-reason-diagnostic.patch'
foreach($path in @($runner,$checker,$Decoder,$provenance,$patch)){if(-not(Test-Path -LiteralPath $path)){throw "F3-C2必須pathがありません: $path"}}
$decoderIdentity=Get-Content -LiteralPath $provenance -Raw -Encoding utf8|ConvertFrom-Json
if([string]$decoderIdentity.schema-ne'mvm-p2-etw-decoder-build-2'){throw 'F3-C2診断decoderがbuildされていません'}
$patchHash=Hash $patch
if([string]$decoderIdentity.discard_reason_patch_sha256-ne$patchHash){throw 'F3-C2診断patchとdecoder provenanceが一致しません'}
if(Test-Path -LiteralPath $OutputDirectory){throw "既存F3-C2 artifactを上書きしません: $OutputDirectory"}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$canonical=Join-Path $OutputDirectory 'canonical'
& pwsh -NoProfile -File $runner -OutputDirectory $canonical -Decoder $Decoder -MeasureSeconds $MeasureSeconds -TimeoutSeconds $TimeoutSeconds
if($LASTEXITCODE-ne0){throw "F3-C2 canonical runが失敗しました: $LASTEXITCODE"}
$proof=Join-Path $OutputDirectory 'discard-reason-proof.json'
& pwsh -NoProfile -File $checker -OracleJson (Join-Path $canonical 'oracle.json') -Output $proof
if($LASTEXITCODE-ne0){throw "F3-C2 discard reason closureが失敗しました: $LASTEXITCODE"}
$result=Get-Content -LiteralPath $proof -Raw -Encoding utf8|ConvertFrom-Json
[ordered]@{
    schema='mvm-p2-c2-discard-attribution-run-1';status='PASS';authority='diagnostic_only'
    formal_counter_authority_changed=$false;measure_seconds=$MeasureSeconds
    present_record_count=[int64]$result.present_record_count;presented_count=[int64]$result.presented_count
    discarded_count=[int64]$result.discarded_count;discard_reason_count=[int64]$result.discard_reason_count
    unknown_discard_reason_count=[int64]$result.unknown_discard_reason_count
    reason_histogram=$result.reason_histogram
    identities=[ordered]@{decoder_sha256=Hash $Decoder;discard_reason_patch_sha256=$patchHash;checker_sha256=Hash $checker;canonical_manifest_sha256=Hash (Join-Path $canonical 'manifest.sha256')}
}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Encoding utf8
$manifest=Join-Path $OutputDirectory 'manifest.sha256'
Get-ChildItem -LiteralPath $OutputDirectory -File|Where-Object{$_.FullName-ne$manifest}|Sort-Object Name|ForEach-Object{"$(Hash $_.FullName)  $($_.Name)"}|Set-Content -LiteralPath $manifest -Encoding ascii
Write-Host "F3-C2 discard attribution: PASS ($OutputDirectory)"
