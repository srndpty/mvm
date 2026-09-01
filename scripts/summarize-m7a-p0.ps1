<#
.SYNOPSIS
    M7a-P0のMLT/GPU生JSONを検査し、backend primitiveの集計JSONを生成する。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$MltJson,
    [Parameter(Mandatory)][string]$GpuJson,
    [Parameter(Mandatory)][string]$OutJson
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-Case([object[]]$Cases, [string]$Name) {
    $found = @($Cases | Where-Object { $_.name -eq $Name })
    if ($found.Count -ne 1) { throw "M7a-P0 case '$Name' が1件ではありません" }
    return $found[0]
}

$mltData = Get-Content -LiteralPath $MltJson -Raw | ConvertFrom-Json
$gpuData = Get-Content -LiteralPath $GpuJson -Raw | ConvertFrom-Json
if ($mltData.schema -ne 'm7a-p0-v1' -or $gpuData.schema -ne 'm7a-p0-v1') {
    throw 'M7a-P0 raw JSON schemaが一致しません'
}
if ($mltData.fade_authority -ne 'clip_local_frame') {
    throw 'MLT fade authorityがclip-localではありません'
}

$baseline = Require-Case $mltData.cases 'baseline'
$crop = Require-Case $mltData.cases 'crop'
$positionScale = Require-Case $mltData.cases 'position_scale'
$rotation = Require-Case $mltData.cases 'transform'
$opacity = Require-Case $mltData.cases 'opacity'
$fade = Require-Case $mltData.cases 'fade'
$cropFirst = Require-Case $mltData.cases 'combined_crop_then_affine'
$affineFirst = Require-Case $mltData.cases 'combined_affine_then_crop'

$baselineSample = $baseline.samples[0]
$cropSample = $crop.samples[0]
$positionSample = $positionScale.samples[0]
$rotationSample = $rotation.samples[0]
$opacitySample = $opacity.samples[0]
if ($baseline.output_frames -le 0 -or $gpuData.samples.Count -eq 0) {
    throw '対象frameが0件です'
}

$opacityRatio = [double]$opacitySample.mean_rgb / [double]$positionSample.mean_rgb
$fadeErrors = @($fade.samples | ForEach-Object {
    [math]::Abs([double]$_.mean_rgb - [double]$baselineSample.mean_rgb * [double]$_.expected_opacity)
})
$fadeMaxError = ($fadeErrors | Measure-Object -Maximum).Maximum
$pivotDx = [math]::Abs([double]$positionSample.magenta_centroid[0] -
                       [double]$rotationSample.magenta_centroid[0])
$pivotDy = [math]::Abs([double]$positionSample.magenta_centroid[1] -
                       [double]$rotationSample.magenta_centroid[1])
$orderEquivalent = (($cropFirst.samples[0].bbox -join ',') -eq
                    ($affineFirst.samples[0].bbox -join ',')) -and
                   ([math]::Abs([double]$cropFirst.samples[0].mean_rgb -
                                [double]$affineFirst.samples[0].mean_rgb) -lt 0.01)
$cropRemovedEdges = $cropSample.marker_pixels.red -eq 0 -and
                    $cropSample.marker_pixels.green -eq 0 -and
                    $cropSample.marker_pixels.blue -eq 0
$cropFilledRect = [double]$cropSample.marker_pixels.magenta -gt
                  [double]$baselineSample.marker_pixels.magenta * 2.0

if ([math]::Abs($opacityRatio - 0.5) -gt 0.08) { throw 'MLT opacity比が期待範囲外です' }
if ($fadeMaxError -gt 5.0) { throw 'MLT fade実画素がhelper期待値から外れています' }
if (-not $cropRemovedEdges -or -not $cropFilledRect) { throw 'MLT crop semanticsを確定できません' }
if ($pivotDx -ge 2.0 -or $pivotDy -ge 2.0) { throw 'MLT rotation pivotを確定できません' }
if (-not $orderEquivalent) { throw 'MLT filter attachment順の比較が一致しません' }
if ($gpuData.rotation.supported_by_current_primitive -ne $false) {
    throw 'GPU rotation capability結果が想定外です'
}

$summary = [ordered]@{
    schema = 'm7a-p0-summary-v1'
    gate = [ordered]@{
        low_cost_path_available = $true
        project_semantics_frozen = $false
        full_m7a_may_start = $false
        reason = 'P0結果レビュー後にcrop/pivot semanticsをProject契約へ固定する'
    }
    fade = [ordered]@{
        authority = 'clip_local_frame'
        mlt_filter_range = 'source_in_frame..source_in_frame+duration-1'
        maximum_mean_rgb_error = $fadeMaxError
    }
    mlt = [ordered]@{
        crop_semantics = 'crop_then_fill_affine_rect'
        crop_mask_requires = 'cropped visible rect compensation'
        rotation_pivot = 'affine_rect_center'
        pivot_marker_delta = @($pivotDx, $pivotDy)
        attachment_order_equivalent_for_probe = $orderEquivalent
        opacity_ratio = $opacityRatio
        required_services = @('crop', 'affine', 'avformat')
    }
    gpu = [ordered]@{
        source_uv_destination_opacity_supported = $true
        rotation_supported_by_current_primitive = $false
        required_local_extension = $gpuData.rotation.required_local_extension
        additional_render_pass_required = $gpuData.rotation.additional_render_pass_required
        cpu_readback_required = $gpuData.rotation.cpu_readback_required
    }
}

$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutJson -Encoding utf8
Write-Host "M7a-P0 summary: PASS"
