[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$JsonDirectory,
    [Parameter(Mandatory)][int]$ExpectedSamples
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Fail([string]$Message) { throw "ATTR-Q2B display preflight: $Message" }
function Property([object]$Object, [string]$Name) {
    if ($null -eq $Object -or $Object.PSObject.Properties.Name -notcontains $Name) {
        Fail "必須fieldがありません: $Name"
    }
    return $Object.$Name
}

$fields = @(
    'screen_name','screen_orientation','screen_geometry_width','screen_geometry_height',
    'available_geometry_width','available_geometry_height','device_pixel_ratio',
    'window_logical_width','window_logical_height','compositor_surface_logical_width',
    'compositor_surface_logical_height','rhi_target_pixel_width','rhi_target_pixel_height'
)
if ($ExpectedSamples -lt 2) { Fail 'ExpectedSamplesは2以上でなければなりません' }
if (-not (Test-Path -LiteralPath $JsonDirectory -PathType Container)) {
    Fail "probe JSON directoryがありません: $JsonDirectory"
}
$jsonPaths = @(Get-ChildItem -LiteralPath $JsonDirectory -Filter 'probe-*.json' -File |
    Sort-Object Name | Select-Object -ExpandProperty FullName)
if ($jsonPaths.Count -ne $ExpectedSamples) {
    Fail "probe JSON数が一致しません: expected=$ExpectedSamples actual=$($jsonPaths.Count)"
}

$reference = $null
foreach ($path in $jsonPaths) {
    if (-not (Test-Path -LiteralPath $path)) { Fail "probe JSONがありません: $path" }
    $root = Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
    if ((Property $root 'schema') -ne 'mvm-display-preflight-probe-1') {
        Fail "schemaが一致しません: $path"
    }
    $passed = Property $root 'preflight_pass'
    if ($passed -isnot [bool] -or -not $passed) { Fail "probeがpreflight PASSではありません: $path" }
    $environment = Property $root 'display_environment'
    foreach ($field in $fields) { $null = Property $environment $field }
    if ($environment.window_logical_width -ne 1920 -or $environment.window_logical_height -ne 1080 -or
        $environment.compositor_surface_logical_width -ne 1920 -or
        $environment.compositor_surface_logical_height -ne 1080 -or
        $environment.rhi_target_pixel_width -ne 1920 -or
        $environment.rhi_target_pixel_height -ne 1080 -or
        [math]::Abs([double]$environment.device_pixel_ratio - 1.0) -gt 0.000001) {
        Fail "probeがP3-C-2 exact display targetではありません: $path"
    }
    if ($null -eq $reference) {
        $reference = $environment
        continue
    }
    foreach ($field in $fields) {
        if ($field -eq 'device_pixel_ratio') {
            if ([math]::Abs([double]$reference.$field - [double]$environment.$field) -gt 0.000001) {
                Fail "probe間で$field が変化しました: $path"
            }
        } elseif ($reference.$field -ne $environment.$field) {
            Fail "probe間で$field が変化しました: $path"
        }
    }
}

Write-Host "PASS: ATTR-Q2B display provenance preflight ($ExpectedSamples samples)"
