[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Good','NegativeScreenName')]
    [string]$Case,
    [Parameter(Mandatory)][string]$Checker,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (Test-Path -LiteralPath $OutputDir) {
    Remove-Item -LiteralPath $OutputDir -Recurse -Force
}
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$paths = [System.Collections.Generic.List[string]]::new()
foreach ($index in 1..5) {
    $screenName = if ($Case -eq 'NegativeScreenName' -and $index -eq 4) {
        'DELL U2412M'
    } else {
        '\\.\DISPLAY1'
    }
    $root = [ordered]@{
        schema='mvm-display-preflight-probe-1'
        preflight_pass=$true
        error=''
        display_environment=[ordered]@{
            screen_name=$screenName;screen_orientation='landscape'
            screen_geometry_width=1920;screen_geometry_height=1200
            available_geometry_width=1920;available_geometry_height=1152
            device_pixel_ratio=1.0;window_logical_width=1920;window_logical_height=1080
            compositor_surface_logical_width=1920;compositor_surface_logical_height=1080
            rhi_target_pixel_width=1920;rhi_target_pixel_height=1080
        }
    }
    $path = Join-Path $OutputDir "probe-$index.json"
    $root | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $path -Encoding utf8
    $paths.Add($path)
}

& pwsh -NoProfile -File $Checker -JsonDirectory $OutputDir -ExpectedSamples 5 2>$null
$exitCode = $LASTEXITCODE
if ($Case -eq 'Good' -and $exitCode -ne 0) {
    throw "正常なdisplay preflight fixtureが拒否されました: exit=$exitCode"
}
if ($Case -eq 'NegativeScreenName' -and $exitCode -eq 0) {
    throw 'screen_nameが揺らぐnegative fixtureを受理しました'
}
Write-Host "PASS: ATTR-Q2B display preflight test ($Case)"
