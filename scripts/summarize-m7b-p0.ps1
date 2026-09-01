param(
    [Parameter(Mandatory = $true)][string]$RawJson,
    [Parameter(Mandatory = $true)][string]$OutJson
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    throw "M7b-P0集計失敗: $Message"
}

if (-not (Test-Path -LiteralPath $RawJson -PathType Leaf)) {
    Fail "生JSONがありません: $RawJson"
}
$rawData = Get-Content -LiteralPath $RawJson -Raw -Encoding utf8 | ConvertFrom-Json
if ([string]$rawData.schema -ne 'm7b-p0-v1') { Fail 'schemaが不正です' }
if ([int]$rawData.failure_count -ne 0) { Fail "harness failure_count=$($rawData.failure_count)" }
if ([int]$rawData.evaluated_case_count -lt 10) { Fail '実画素caseが10件未満です' }
if ([string]$rawData.domains.transition_in_out -ne 'timeline_absolute') {
    Fail 'transition in/out authorityを確定できていません'
}
if ([string]$rawData.domains.animation_keyframes -ne 'transition_local') {
    Fail 'animation keyframe authorityを確定できていません'
}
if ([bool]$rawData.v2_affine_filter) { Fail 'V2がaffine filterを使用しています' }
if ([string]$rawData.affine_properties.rotation_property -ne 'fix_rotate_x' -or
    [int]$rawData.affine_properties.keyed -ne 0 -or
    [int]$rawData.affine_properties.b_alpha -ne 0 -or
    [string]$rawData.affine_properties.opacity_property -ne 'rect.o') {
    Fail '実画素で成立したaffine property契約と一致しません'
}

$requiredCases = @(
    'v1_only_baseline',
    'centered_scaled_v2',
    'opacity_50_reveals_v1',
    'asymmetric_crop_reveals_v1',
    'position_rotation',
    'fade_reveals_v1',
    'nonzero_source_in',
    'nonzero_timeline_start_and_blanks',
    'two_nonoverlapping_v2_clips'
)
$caseNames = @($rawData.cases | ForEach-Object { [string]$_.name })
foreach ($required in $requiredCases) {
    if ($required -notin $caseNames) { Fail "必須caseがありません: $required" }
}
foreach ($case in @($rawData.cases)) {
    if ([int64]$case.output_frames -lt 90) { Fail "$($case.name)の出力frameが不足しています" }
    if ([int]$case.playlist_count -ne 2) { Fail "$($case.name)が2 playlistではありません" }
    if ([int]$case.opaque_black_affine_filter_count -ne 0) {
        Fail "$($case.name)がopaque-black affine filterを使用しています"
    }
    if ([string]$case.name -ne 'v1_only_baseline' -and @($case.clips).Count -eq 0) {
        Fail "$($case.name)の入力clip propertyが生JSONにありません"
    }
}

$summary = [ordered]@{
    schema = 'm7b-p0-summary-v1'
    verdict = 'PASS'
    graph = [string]$rawData.graph
    transition_in_out_authority = [string]$rawData.domains.transition_in_out
    animation_keyframe_authority = [string]$rawData.domains.animation_keyframes
    affine_properties = $rawData.affine_properties
    crop_properties = $rawData.crop_properties
    evaluated_case_count = [int]$rawData.evaluated_case_count
    opacity_blend_error = [double]$rawData.metrics.opacity_blend_error
    crop_outside_difference = [double]$rawData.metrics.crop_outside_difference
    fade_start_bottom_difference = [double]$rawData.metrics.fade_start_bottom_difference
    fade_end_bottom_difference = [double]$rawData.metrics.fade_end_bottom_difference
    blank_max_difference = [double]$rawData.metrics.blank_max_difference
    two_clip_gap_max_difference = [double]$rawData.metrics.two_clip_gap_max_difference
    local_key_error = [double]$rawData.domains.local_key_error
    absolute_key_error = [double]$rawData.domains.absolute_key_error
}
$outDirectory = Split-Path -Parent $OutJson
if ($outDirectory) { New-Item -ItemType Directory -Force -Path $outDirectory | Out-Null }
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $OutJson -Encoding utf8
Write-Host "M7b-P0集計: PASS ($($summary.evaluated_case_count) cases)"
