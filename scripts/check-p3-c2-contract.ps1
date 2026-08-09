[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Json,
    [ValidateSet('playback','seek','pause-resume')][string]$Mode,
    [int]$ProcessExitCode = 0,
    [switch]$DryRun,
    [string]$ReferenceJson
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$Message) { throw "P3-C-2 contract: $Message" }
function Property([object]$Object, [string]$Name) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        Fail "必須 field がありません: $Name"
    }
    return $property.Value
}
function Integer([object]$Object, [string]$Name) {
    $value = Property $Object $Name
    if ($value -isnot [sbyte] -and $value -isnot [byte] -and
        $value -isnot [int16] -and $value -isnot [uint16] -and
        $value -isnot [int32] -and $value -isnot [uint32] -and
        $value -isnot [int64] -and $value -isnot [uint64]) {
        Fail "$Name は JSON integer ではありません"
    }
    return [long]$value
}
function Number([object]$Object, [string]$Name) {
    $value = Property $Object $Name
    if ($value -isnot [ValueType] -or $value -is [bool]) { Fail "$Name は数値ではありません" }
    $number = [double]$value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) {
        Fail "$Name は有限数ではありません"
    }
    return $number
}
function Text([object]$Object, [string]$Name) {
    $value = Property $Object $Name
    if ($value -isnot [string] -or [string]::IsNullOrWhiteSpace($value)) {
        Fail "$Name は空でない文字列ではありません"
    }
    return [string]$value
}
function Boolean([object]$Object, [string]$Name, [bool]$Expected) {
    $value = Property $Object $Name
    if ($value -isnot [bool]) { Fail "$Name はJSON booleanではありません" }
    if ($value -ne $Expected) { Fail "$Name が期待値 $Expected ではありません" }
    return [bool]$value
}

$displayFields = @(
    'screen_name','screen_orientation','screen_geometry_width','screen_geometry_height',
    'available_geometry_width','available_geometry_height','device_pixel_ratio',
    'window_logical_width','window_logical_height','compositor_surface_logical_width',
    'compositor_surface_logical_height','rhi_target_pixel_width','rhi_target_pixel_height'
)

function DisplayEnvironment([object]$Root, [string]$Name) {
    $value = Property $Root $Name
    $result = [ordered]@{
        screen_name = Text $value 'screen_name'
        screen_orientation = Text $value 'screen_orientation'
        screen_geometry_width = Integer $value 'screen_geometry_width'
        screen_geometry_height = Integer $value 'screen_geometry_height'
        available_geometry_width = Integer $value 'available_geometry_width'
        available_geometry_height = Integer $value 'available_geometry_height'
        device_pixel_ratio = Number $value 'device_pixel_ratio'
        window_logical_width = Integer $value 'window_logical_width'
        window_logical_height = Integer $value 'window_logical_height'
        compositor_surface_logical_width = Integer $value 'compositor_surface_logical_width'
        compositor_surface_logical_height = Integer $value 'compositor_surface_logical_height'
        rhi_target_pixel_width = Integer $value 'rhi_target_pixel_width'
        rhi_target_pixel_height = Integer $value 'rhi_target_pixel_height'
    }
    foreach ($field in @('screen_geometry_width','screen_geometry_height',
            'available_geometry_width','available_geometry_height')) {
        if ($result[$field] -le 0) { Fail "$Name.$field は正数ではありません" }
    }
    foreach ($field in @('native_window_outer_width','native_window_outer_height',
            'native_window_client_width','native_window_client_height')) {
        if ($value.PSObject.Properties.Name -contains $field) {
            $native = Integer $value $field
            if ($native -lt 0) { Fail "$Name.$field はnonnegativeではありません" }
        }
    }
    return $result
}

function RequireTarget([object]$Value, [string]$Name) {
    if ($Value.window_logical_width -ne 1920 -or $Value.window_logical_height -ne 1080) {
        Fail "$Name のQQuickWindow logical sizeが1920x1080ではありません"
    }
    if ($Value.compositor_surface_logical_width -ne 1920 -or
        $Value.compositor_surface_logical_height -ne 1080) {
        Fail "$Name のCompositorSurface logical sizeが1920x1080ではありません"
    }
    if ($Value.rhi_target_pixel_width -ne 1920 -or $Value.rhi_target_pixel_height -ne 1080) {
        Fail "$Name のactual RHI targetが1920x1080ではありません"
    }
    if ([math]::Abs($Value.device_pixel_ratio - 1.0) -gt 0.000001) {
        Fail "$Name のdevicePixelRatioが1.0ではありません"
    }
}

function RequireSameDisplay([object]$A, [object]$B, [string]$Name) {
    foreach ($field in $displayFields) {
        if ($field -eq 'device_pixel_ratio') {
            if ([math]::Abs([double]$A[$field] - [double]$B[$field]) -gt 0.000001) {
                Fail "$Name の$field が変化しました"
            }
        } elseif ($A[$field] -ne $B[$field]) {
            Fail "$Name の$field が変化しました"
        }
    }
}

if (-not (Test-Path -LiteralPath $Json)) { Fail "JSONがありません: $Json" }
try { $data = Get-Content -Raw -LiteralPath $Json | ConvertFrom-Json } catch { Fail "JSONを読めません: $_" }
if ((Property $data 'schema') -ne 'mvm-p3-formal-2' -or
    (Property $data 'contract_version') -ne 'P3-C-2') {
    Fail 'schema/contract versionがP3-C-2ではありません'
}
if ((Integer $data 'requested_output_width') -ne 1920 -or
    (Integer $data 'requested_output_height') -ne 1080) {
    Fail 'requested output sizeが1920x1080ではありません'
}
Boolean $data 'display_target_preflight_pass' $true | Out-Null
Boolean $data 'formal_workload_started' $true | Out-Null

$start = DisplayEnvironment $data 'display_environment_start'
$end = DisplayEnvironment $data 'display_environment_end'
RequireTarget $start 'start'
RequireTarget $end 'end'
RequireSameDisplay $start $end 'start/end display environment'

if ($ReferenceJson) {
    if (-not (Test-Path -LiteralPath $ReferenceJson)) { Fail "reference JSONがありません: $ReferenceJson" }
    try { $reference = Get-Content -Raw -LiteralPath $ReferenceJson | ConvertFrom-Json } catch {
        Fail "reference JSONを読めません: $_"
    }
    $referenceStart = DisplayEnvironment $reference 'display_environment_start'
    RequireSameDisplay $referenceStart $start 'matrix run間 display environment'
}

# P3-C-1の性能・correctness検査をそのまま再利用し、C2で弱めない。
$legacy = $data | ConvertTo-Json -Depth 100 | ConvertFrom-Json
$legacy.schema = 'mvm-p3-formal-1'
$legacy.contract_version = 'P3-C-1'
$temporary = [IO.Path]::GetTempFileName()
try {
    $legacy | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $temporary -Encoding utf8
    $checker = Join-Path $PSScriptRoot 'check-p3-c-contract.ps1'
    $arguments = @('-NoProfile','-File',$checker,'-Json',$temporary,
        '-ProcessExitCode',$ProcessExitCode)
    if ($Mode) { $arguments += @('-Mode',$Mode) }
    if ($DryRun) { $arguments += '-DryRun' }
    & pwsh @arguments
    if ($LASTEXITCODE -ne 0) { Fail '継承したP3-C-1 contractが不合格です' }
} finally {
    Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
}

$suffix = if ($Mode) { " ($Mode)" } else { '' }
Write-Host "PASS: P3-C-2 contract$suffix" -ForegroundColor Green
