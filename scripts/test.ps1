<#
.SYNOPSIS
    ビルドして CTest を実行する。

.DESCRIPTION
    既定では release と debug の両方をビルドし、通常テストを実行する。
    通常テストは Smoke 素材を使い短時間で終わる。

    性能計測 (LABELS performance) と安定性・診断 (LABELS stability) は
    既定では実行しない。両方を除外しないと、通常テストの所要時間に
    長時間の診断が混ざり、「通常テストが何件通ったか」が分からなくなる。

    debug ビルドの性能値を判定に使わないため、-Performance は
    release でのみ意味を持つ。-Stability は診断が目的なので preset を問わない。

.PARAMETER Preset
    ucrt64-release / ucrt64-debug / both (既定)

.PARAMETER Performance
    performance ラベルのテストも実行する。release のみ。

.PARAMETER Stability
    stability ラベル (長時間のメモリ診断など) も実行する。
    現時点では合否判定に使えないため、既定では実行しない。

.EXAMPLE
    pwsh scripts/test.ps1
    pwsh scripts/test.ps1 -Preset ucrt64-release -Performance
    pwsh scripts/test.ps1 -Preset ucrt64-release -Stability
#>
[CmdletBinding()]
param(
    [ValidateSet('ucrt64-release', 'ucrt64-debug', 'both')]
    [string]$Preset = 'both',

    [switch]$Performance,
    [switch]$Stability,
    [string]$Ucrt64 = 'C:\msys64\ucrt64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$CTest    = Join-Path $Ucrt64 'bin\ctest.exe'

$presets = if ($Preset -eq 'both') { @('ucrt64-release', 'ucrt64-debug') } else { @($Preset) }

$anyFailed = $false
# 種別ごとに件数を分けて報告する。合計だけだと
# 「通常テストが減っている」ことに気づけない。
$summary = @()
$lastGroupExit = 0

function Invoke-CTestGroup {
    param([string]$Preset, [string]$Kind, [string[]]$CTestArgs)

    # 対象件数は -N (dry run) で先に数える。0 件を「通った」と report しない。
    $listed = & $script:CTest -N @CTestArgs
    $total = 0
    foreach ($line in $listed) {
        if ($line -match '^Total Tests:\s*(\d+)') { $total = [int]$Matches[1] }
    }

    $failedLog = Join-Path (Get-Location) 'Testing\Temporary\LastTestsFailed.log'
    Remove-Item $failedLog -ErrorAction SilentlyContinue

    # ctest の標準出力は関数の戻り値に混ざる。戻り値で終了コードを
    # 返すと、呼び出し側は「出力行の配列」と 0 を比較することになり、
    # 全テストが通っていても失敗と判定される。実際に一度そうなった。
    # 終了コードは script スコープの変数で受け渡す。
    & $script:CTest --output-on-failure @CTestArgs
    $code = $LASTEXITCODE

    $failed = @(Get-Content $failedLog -ErrorAction SilentlyContinue).Count
    $script:summary += [pscustomobject]@{
        Preset = $Preset; Kind = $Kind; Total = $total
        Failed = $failed; Passed = $total - $failed; Exit = $code
    }
    if ($total -eq 0) {
        Write-Host "${Kind}: 対象テストが 0 件です。ラベル指定を確認してください。" -ForegroundColor Yellow
    }
    $script:lastGroupExit = $code
}

foreach ($p in $presets) {
    Write-Host "`n=== $p ===" -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot 'build.ps1') -Preset $p -Ucrt64 $Ucrt64
    if ($LASTEXITCODE -ne 0) { throw "ビルドに失敗しました: $p" }

    $buildDir = Join-Path $RepoRoot "build\$p"
    $env:PATH = "$Ucrt64\bin;$env:PATH"

    Push-Location $buildDir
    try {
        # 通常テスト: performance と stability の両方を除外する
        Invoke-CTestGroup -Preset $p -Kind '通常' -CTestArgs @('-LE', 'performance|stability')
        if ($lastGroupExit -ne 0) { $anyFailed = $true }

        if ($Performance) {
            if ($p -ne 'ucrt64-release') {
                Write-Host "性能計測は release でのみ実行します ($p はスキップ)" -ForegroundColor Yellow
            } else {
                Write-Host "`n--- 性能計測 ---" -ForegroundColor Cyan
                Invoke-CTestGroup -Preset $p -Kind '性能' -CTestArgs @('-L', 'performance')
                if ($lastGroupExit -ne 0) { $anyFailed = $true }
            }
        }

        if ($Stability) {
            Write-Host "`n--- 安定性・診断 ---" -ForegroundColor Cyan
            # 診断であって合否ではない。失敗しても全体の判定には含めない。
            Invoke-CTestGroup -Preset $p -Kind '安定性(診断)' -CTestArgs @('-L', 'stability')
            if ($lastGroupExit -ne 0) {
                Write-Host "安定性テストに失敗がありますが、診断扱いなので全体判定には含めません。" `
                    -ForegroundColor Yellow
            }
        }
    } finally {
        Pop-Location
    }
}

Write-Host "`n=== テスト種別ごとの結果 ===" -ForegroundColor Cyan
$summary | Format-Table Preset, Kind, Total, Passed, Failed, Exit -AutoSize

if ($anyFailed) {
    Write-Host "`nテストに失敗があります。" -ForegroundColor Red
    exit 1
}
Write-Host "`n全テスト通過" -ForegroundColor Green
