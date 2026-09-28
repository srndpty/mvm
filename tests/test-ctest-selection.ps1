[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Library,
    [Parameter(Mandatory)][string]$Fixture,
    [Parameter(Mandatory)][string]$OutputDir,
    [Parameter(Mandatory)][string]$CMake,
    [Parameter(Mandatory)][string]$CTest,
    [Parameter(Mandatory)][string]$Ninja,
    [Parameter(Mandatory)][string]$Pwsh,
    [string]$ActualBuildDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $Library

function RequireNames {
    param([string[]]$Actual, [string[]]$Expected, [string]$Case)
    $actualText = @($Actual | Sort-Object) -join ','
    $expectedText = @($Expected | Sort-Object) -join ','
    if ($actualText -ne $expectedText) {
        throw "${Case}: 対象が不正です。期待=$expectedText 実際=$actualText"
    }
}

function GetSelectedNames {
    param([string]$BuildDir, [switch]$Portable, [switch]$Fast)
    $exclude = Get-MvmNormalExcludePattern -Portable:$Portable -Fast:$Fast
    $json = & $CTest --test-dir $BuildDir --show-only=json-v1 -LE $exclude
    if ($LASTEXITCODE -ne 0) { throw "CTest JSON を取得できません: $BuildDir" }
    $tests = @((($json -join "`n") | ConvertFrom-Json).tests)
    $independent = @(Get-MvmBuildIndependentTestNames -Tests $tests -BuildDir $BuildDir)
    $selected = @($tests | Where-Object name -NotIn $independent | ForEach-Object name)
    return $selected
}

RequireNames -Actual @(Get-MvmDailyTestArguments) -Expected @(
    '-Preset', 'ucrt64-release', '-Group', 'BuildDependent', '-Portable', '-Fast'
) -Case 'dev test の引数'
RequireNames -Actual @(Get-MvmFullTestArguments) -Expected @(
    '-Preset', 'both', '-Group', 'All'
) -Case 'dev test-full の引数'

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
& $CMake -S $Fixture -B $OutputDir -G Ninja "-DCMAKE_MAKE_PROGRAM:FILEPATH=$Ninja" `
    "-DMVM_PWSH:FILEPATH=$Pwsh" | Out-Null
if ($LASTEXITCODE -ne 0) { throw '選定 fixture の configure に失敗しました' }

RequireNames -Actual @(GetSelectedNames -BuildDir $OutputDir -Portable -Fast) `
    -Expected @('normal_build_dependent', 'dependent_pwsh', 'linked_pwsh') -Case '日常'
RequireNames -Actual @(GetSelectedNames -BuildDir $OutputDir -Portable) `
    -Expected @('normal_build_dependent', 'dependent_pwsh', 'linked_pwsh', 'extended_case') `
    -Case 'Fast なし'
RequireNames -Actual @(GetSelectedNames -BuildDir $OutputDir -Fast) `
    -Expected @('normal_build_dependent', 'dependent_pwsh', 'linked_pwsh', 'workstation_case') `
    -Case 'Portable なし'
RequireNames -Actual @(GetSelectedNames -BuildDir $OutputDir) `
    -Expected @('normal_build_dependent', 'dependent_pwsh', 'linked_pwsh',
        'extended_case', 'workstation_case') -Case '全件'

$zeroOutput = & $CTest --test-dir $OutputDir -N -R '^存在しないテスト$'
if ($LASTEXITCODE -ne 0 -or -not (@($zeroOutput) -match '^Total Tests: 0$')) {
    throw '0 件 fixture を取得できませんでした'
}
$zeroRejected = $false
try { Assert-MvmRequiredCTestCount -Total 0 -Required } catch {
    $zeroRejected = "$($_.Exception.Message)" -match '0 件'
}
if (-not $zeroRejected) { throw 'Required の 0 件を拒否できませんでした' }
Assert-MvmRequiredCTestCount -Total 1 -Required

if (-not $ActualBuildDir) {
    $ActualBuildDir = Split-Path -Parent (Split-Path -Parent $OutputDir)
}
$full = @(GetSelectedNames -BuildDir $ActualBuildDir -Portable)
$daily = @(GetSelectedNames -BuildDir $ActualBuildDir -Portable -Fast)
$ordinary = @(GetSelectedNames -BuildDir $ActualBuildDir)
if ('ownership_soak_100' -notin $ordinary) {
    throw 'ownership_soak_100 が通常全件から外れました'
}
if ('ownership_soak_100' -in $daily) {
    throw 'ownership_soak_100 が日常対象に混入しました'
}
if ($full.Count -lt 50 -or $daily.Count -lt 50 -or $daily.Count / $full.Count -lt 0.75) {
    throw "実スイートの日常対象が想定以上に減りました: 日常=$($daily.Count) 全件=$($full.Count)"
}
Write-Host "CTest 選定の正例・除外・0件拒否を確認しました (日常 $($daily.Count) / 全件 $($full.Count))"
