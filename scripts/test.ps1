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

# fail-closed。「測れなかった」を「通った」と報告しない。
#
#   - ctest -N 自体が失敗したら失敗
#   - Total Tests 行が取れなければ失敗 (件数不明を成功にしない)
#   - 対象が 0 件なら失敗 (ラベル指定の誤りを緑にしない)
#   - 実行数と件数が食い違ったら失敗
#
# $Required は「この種別は必ず 1 件以上あるはず」を表す。
function Invoke-CTestGroup {
    param([string]$Preset, [string]$Kind, [string[]]$CTestArgs, [switch]$Required)

    # 対象件数は -N (dry run) で先に数える。
    $listed = & $script:CTest -N @CTestArgs 2>&1
    $listExit = $LASTEXITCODE
    $total = -1
    foreach ($line in $listed) {
        if ("$line" -match '^Total Tests:\s*(\d+)') { $total = [int]$Matches[1] }
    }

    $fatal = @()
    if ($listExit -ne 0) {
        $fatal += "ctest -N が exit $listExit で失敗しました"
    }
    if ($total -lt 0) {
        $fatal += "ctest -N の出力から 'Total Tests' 行を取得できませんでした"
    }
    if ($fatal.Count -gt 0) {
        foreach ($m in $fatal) { Write-Host "${Kind}: $m" -ForegroundColor Red }
        $script:summary += [pscustomobject]@{
            Preset = $Preset; Kind = $Kind; Total = $total; Ran = 0
            Failed = -1; Passed = -1; Exit = 1; Note = ($fatal -join ' / ')
        }
        $script:lastGroupExit = 1
        return
    }

    if ($total -eq 0) {
        $msg = if ($Required) {
            '対象テストが 0 件です。この種別は 1 件以上あるはずなので失敗にします。'
        } else {
            '対象テストが 0 件です。ラベル指定を確認してください。'
        }
        Write-Host "${Kind}: $msg" -ForegroundColor Red
        $script:summary += [pscustomobject]@{
            Preset = $Preset; Kind = $Kind; Total = 0; Ran = 0
            Failed = 0; Passed = 0; Exit = 1; Note = '0 件'
        }
        $script:lastGroupExit = 1
        return
    }

    $failedLog = Join-Path (Get-Location) 'Testing\Temporary\LastTestsFailed.log'
    Remove-Item $failedLog -ErrorAction SilentlyContinue

    # ctest の標準出力は関数の戻り値に混ざる。戻り値で終了コードを
    # 返すと、呼び出し側は「出力行の配列」と 0 を比較することになり、
    # 全テストが通っていても失敗と判定される。実際に一度そうなった。
    # 終了コードは script スコープの変数で受け渡す。
    # Tee-Object -Variable は 2 回目以降の呼び出しで空になることがあった。
    # 一旦すべて受け取ってから自分で表示する。
    $outLines = & $script:CTest --output-on-failure @CTestArgs 2>&1
    $code = $LASTEXITCODE
    $outLines | ForEach-Object { Write-Host "$_" }

    # 実際に何件走ったかを ctest の要約行から取る。
    # 失敗が 0 件のときは "100% tests passed out of 88" となり
    # "tests failed" を含まない。両方の書式に当たる必要がある。
    #   失敗あり: "95% tests passed, 4 tests failed out of 88"
    #   失敗なし: "100% tests passed out of 88"
    $ran = -1
    foreach ($line in $outLines) {
        if ("$line" -match 'tests passed.*out of\s+(\d+)') { $ran = [int]$Matches[1] }
    }

    $failed = @(Get-Content $failedLog -ErrorAction SilentlyContinue).Count
    $note = ''
    if ($ran -lt 0) {
        $note = 'ctest の要約行を取得できませんでした'
        Write-Host "${Kind}: $note" -ForegroundColor Red
        $code = 1
    } elseif ($ran -ne $total) {
        $note = "件数 $total に対し実行 $ran 件。数が合いません"
        Write-Host "${Kind}: $note" -ForegroundColor Red
        $code = 1
    }

    $script:summary += [pscustomobject]@{
        Preset = $Preset; Kind = $Kind; Total = $total; Ran = $ran
        Failed = $failed; Passed = $ran - $failed; Exit = $code; Note = $note
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
        Invoke-CTestGroup -Preset $p -Kind '通常' -Required `
            -CTestArgs @('-LE', 'performance|stability')
        if ($lastGroupExit -ne 0) { $anyFailed = $true }

        if ($Performance) {
            if ($p -ne 'ucrt64-release') {
                Write-Host "性能計測は release でのみ実行します ($p はスキップ)" -ForegroundColor Yellow
            } else {
                Write-Host "`n--- 性能計測 ---" -ForegroundColor Cyan
                # -Performance を指定したのに 0 件なら失敗にする。
                Invoke-CTestGroup -Preset $p -Kind '性能' -Required -CTestArgs @('-L', 'performance')
                if ($lastGroupExit -ne 0) { $anyFailed = $true }
            }
        }

        if ($Stability) {
            Write-Host "`n--- 安定性・診断 ---" -ForegroundColor Cyan
            # 診断であって合否ではない。失敗しても全体の判定には含めない。
            # -Stability を指定したのに 0 件なら失敗にする。
            # テスト自体の失敗は診断扱いだが、「対象が無い」は別の問題である。
            Invoke-CTestGroup -Preset $p -Kind '安定性(診断)' -Required -CTestArgs @('-L', 'stability')
            $row = $summary[-1]
            if ($row.Total -eq 0 -or $row.Ran -lt 0) {
                $anyFailed = $true
            } elseif ($lastGroupExit -ne 0) {
                Write-Host "安定性テストに失敗がありますが、診断扱いなので全体判定には含めません。" `
                    -ForegroundColor Yellow
            }
        }
    } finally {
        Pop-Location
    }
}

Write-Host "`n=== テスト種別ごとの結果 ===" -ForegroundColor Cyan
$summary | Format-Table Preset, Kind, Total, Ran, Passed, Failed, Exit, Note -AutoSize

if ($anyFailed) {
    Write-Host "`nテストに失敗があります。" -ForegroundColor Red
    exit 1
}
Write-Host "`n全テスト通過" -ForegroundColor Green
