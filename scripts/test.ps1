<#
.SYNOPSIS
    ビルドして CTest を実行する。

.DESCRIPTION
    既定では release と debug の両方をビルドし、通常テストを実行する。
    通常テストは Smoke 素材を使い短時間で終わる。

    性能計測 (LABELS performance) は既定では実行しない。
    debug ビルドの性能値を判定に使わないため、-Performance は
    release でのみ意味を持つ。

.PARAMETER Preset
    ucrt64-release / ucrt64-debug / both (既定)

.PARAMETER Performance
    performance ラベルのテストも実行する。release のみ。

.EXAMPLE
    pwsh scripts/test.ps1
    pwsh scripts/test.ps1 -Preset ucrt64-release -Performance
#>
[CmdletBinding()]
param(
    [ValidateSet('ucrt64-release', 'ucrt64-debug', 'both')]
    [string]$Preset = 'both',

    [switch]$Performance,
    [string]$Ucrt64 = 'C:\msys64\ucrt64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$CTest    = Join-Path $Ucrt64 'bin\ctest.exe'

$presets = if ($Preset -eq 'both') { @('ucrt64-release', 'ucrt64-debug') } else { @($Preset) }

$anyFailed = $false

foreach ($p in $presets) {
    Write-Host "`n=== $p ===" -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'build.ps1') -Preset $p -Ucrt64 $Ucrt64
    if ($LASTEXITCODE -ne 0) { throw "ビルドに失敗しました: $p" }

    $buildDir = Join-Path $RepoRoot "build\$p"
    $env:PATH = "$Ucrt64\bin;$env:PATH"

    Push-Location $buildDir
    try {
        # 通常テスト: performance ラベルを除外する
        & $CTest --output-on-failure -LE performance
        if ($LASTEXITCODE -ne 0) { $anyFailed = $true }

        if ($Performance) {
            if ($p -ne 'ucrt64-release') {
                Write-Host "性能計測は release でのみ実行します ($p はスキップ)" -ForegroundColor Yellow
            } else {
                Write-Host "`n--- 性能計測 ---" -ForegroundColor Cyan
                & $CTest --output-on-failure -L performance
                if ($LASTEXITCODE -ne 0) { $anyFailed = $true }
            }
        }
    } finally {
        Pop-Location
    }
}

if ($anyFailed) {
    Write-Host "`nテストに失敗があります。" -ForegroundColor Red
    exit 1
}
Write-Host "`n全テスト通過" -ForegroundColor Green
