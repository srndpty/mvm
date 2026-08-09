[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Case,
    [Parameter(Mandatory)][string]$Checker,
    [Parameter(Mandatory)][string]$LegacyChecker,
    [Parameter(Mandatory)][string]$LegacyFixtureScript,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$legacyDir = Join-Path $OutputDir 'legacy-good'
& pwsh -NoProfile -File $LegacyFixtureScript -Case Good -Checker $LegacyChecker -OutputDir $legacyDir
if ($LASTEXITCODE -ne 0) { throw 'P3-C-1 positive fixtureを生成できません' }
$record = Get-Content -Raw -LiteralPath (Join-Path $legacyDir 'Good.json') | ConvertFrom-Json
$record.schema = 'mvm-p3-formal-2'
$record.contract_version = 'P3-C-2'
$record | Add-Member -NotePropertyName requested_output_width -NotePropertyValue 1920
$record | Add-Member -NotePropertyName requested_output_height -NotePropertyValue 1080
$record | Add-Member -NotePropertyName display_target_preflight_pass -NotePropertyValue $true
$record | Add-Member -NotePropertyName formal_workload_started -NotePropertyValue $true

function Environment([string]$Orientation = 'landscape') {
    return [ordered]@{
        screen_name='test-screen'; screen_orientation=$Orientation
        screen_geometry_width=1920; screen_geometry_height=1200
        available_geometry_width=1920; available_geometry_height=1152
        device_pixel_ratio=1.0
        window_logical_width=1920; window_logical_height=1080
        compositor_surface_logical_width=1920; compositor_surface_logical_height=1080
        rhi_target_pixel_width=1920; rhi_target_pixel_height=1080
        native_window_outer_width=1936; native_window_outer_height=1119
        native_window_client_width=1920; native_window_client_height=1080
    }
}
$record | Add-Member -NotePropertyName display_environment_start -NotePropertyValue ([pscustomobject](Environment))
$record | Add-Member -NotePropertyName display_environment_end -NotePropertyValue ([pscustomobject](Environment))
$reference = $record | ConvertTo-Json -Depth 100 | ConvertFrom-Json
$expectPass = $Case -eq 'GoodC2'
$nanToken = $false

switch ($Case) {
    'GoodC2' {}
    'NegativePreflightFalse' {$record.display_target_preflight_pass=$false}
    'NegativePreflightType' {$record.display_target_preflight_pass='false'}
    'NegativeWorkloadFalse' {$record.formal_workload_started=$false}
    'NegativeWorkloadType' {$record.formal_workload_started='false'}
    'NegativeMissingDisplayTelemetry' {$record.PSObject.Properties.Remove('display_environment_start')}
    'NegativeRequestedWidth' {$record.requested_output_width=1919}
    'NegativeWindowWidth' {$record.display_environment_start.window_logical_width=1919}
    'NegativeWindowHeight' {$record.display_environment_start.window_logical_height=1079}
    'NegativeSurfaceWidth' {$record.display_environment_start.compositor_surface_logical_width=1919}
    'NegativeSurfaceHeight' {$record.display_environment_start.compositor_surface_logical_height=1079}
    'NegativeRhiWidth' {$record.display_environment_start.rhi_target_pixel_width=1204}
    'NegativeRhiHeight' {$record.display_environment_start.rhi_target_pixel_height=1079}
    'NegativeDpr' {$record.display_environment_start.device_pixel_ratio=1.25}
    'NegativeDisplayStartEndOrientation' {$record.display_environment_end.screen_orientation='portrait'}
    'NegativeDisplayStartEndGeometry' {$record.display_environment_end.screen_geometry_width=1200}
    'NegativeDisplayStartEndDpr' {$record.display_environment_end.device_pixel_ratio=1.25}
    'NegativeDisplayStartEndWindow' {$record.display_environment_end.window_logical_width=1919}
    'NegativeDisplayStartEndSurface' {$record.display_environment_end.compositor_surface_logical_width=1919}
    'NegativeDisplayStartEndRhiTarget' {$record.display_environment_end.rhi_target_pixel_width=1204}
    'NegativeDisplayEnvironmentRunMismatch' {
        $record.display_environment_start.screen_name='other-screen'
        $record.display_environment_end.screen_name='other-screen'
    }
    'NegativeDisplayNull' {$record.display_environment_start.screen_name=$null}
    'NegativeDisplayNaN' {
        $record.display_environment_start.device_pixel_ratio='__NAN__'
        $nanToken=$true
    }
    'NegativeDisplayType' {$record.display_environment_start.window_logical_width='1920'}
    default {throw "未知のcase: $Case"}
}

$json = Join-Path $OutputDir "$Case.json"
$text = $record | ConvertTo-Json -Depth 100
if ($nanToken) { $text=$text.Replace('"__NAN__"','NaN') }
Set-Content -LiteralPath $json -Value $text -Encoding utf8
$referenceJson = Join-Path $OutputDir 'Reference.json'
$reference | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $referenceJson -Encoding utf8
$arguments=@('-NoProfile','-File',$Checker,'-Json',$json,'-Mode','playback','-DryRun')
if ($Case -eq 'NegativeDisplayEnvironmentRunMismatch') {
    $arguments += @('-ReferenceJson',$referenceJson)
}
& pwsh @arguments
$actualPass = $LASTEXITCODE -eq 0
if ($actualPass -ne $expectPass) {
    Write-Error "P3-C-2 checker caseが期待と不一致です: $Case (pass=$actualPass)"
}
