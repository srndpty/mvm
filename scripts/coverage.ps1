<#
.SYNOPSIS
    カバレッジ付きでビルドし、CTest を実行して gcovr でレポートを出す。

.DESCRIPTION
    専用の build ディレクトリ (build/ucrt64-coverage) を使う。
    通常ビルドに --coverage を混ぜると、計測用の副産物が
    通常の成果物に混入するため分離している。

    Phase 0 ではカバレッジを合格条件にしない。
    「検証コードのどこが一度も実行されていないか」を知るために使う。
    実際、到達しない防御的検査があることが分かっている
    (docs/phase0-findings.md の破損素材の節)。

.EXAMPLE
    pwsh scripts/coverage.ps1
#>
[CmdletBinding()]
param(
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$CMake    = Join-Path $Ucrt64 'bin\cmake.exe'
$CTest    = Join-Path $Ucrt64 'bin\ctest.exe'
$Gcovr    = Join-Path $Ucrt64 'bin\gcovr.exe'
$BuildDir = Join-Path $RepoRoot 'build\ucrt64-coverage'
$OutDir   = Join-Path $RepoRoot 'build\coverage-report'

if (-not (Test-Path $Gcovr)) {
    throw @"
gcovr が見つかりません: $Gcovr

    pwsh scripts/bootstrap-msys2.ps1
"@
}

$env:PATH = "$Ucrt64\bin;$env:PATH"
$env:QTDIR = ''
$env:Qt6_DIR = ''
$env:CMAKE_PREFIX_PATH = ''

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

Push-Location $RepoRoot
try {
    Write-Host '=== カバレッジビルド ===' -ForegroundColor Cyan
    & $CMake --preset ucrt64-debug -B $BuildDir -DMVM_ENABLE_COVERAGE=ON
    if ($LASTEXITCODE -ne 0) { throw 'configure に失敗しました' }

    & $CMake --build $BuildDir
    if ($LASTEXITCODE -ne 0) { throw 'build に失敗しました' }

    Write-Host "`n=== テスト実行 ===" -ForegroundColor Cyan
    Push-Location $BuildDir
    try {
        # カバレッジ収集が目的なので、失敗しても集計まで進む
        & $CTest --output-on-failure -LE performance
        $testExit = $LASTEXITCODE
    } finally {
        Pop-Location
    }

    Write-Host "`n=== gcovr ===" -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

    & $Gcovr --root $RepoRoot `
             --filter "$($RepoRoot -replace '\\','/')/src/" `
             --exclude '.*/build/.*' `
             --print-summary `
             --html-details (Join-Path $OutDir 'index.html') `
             --xml (Join-Path $OutDir 'coverage.xml')
    if ($LASTEXITCODE -ne 0) { throw 'gcovr に失敗しました' }

    Write-Host "`nレポート: $(Join-Path $OutDir 'index.html')" -ForegroundColor Green
    if ($testExit -ne 0) {
        Write-Host 'テストに失敗がありました (カバレッジは集計済み)' -ForegroundColor Yellow
        exit 1
    }
} finally {
    Pop-Location
}
