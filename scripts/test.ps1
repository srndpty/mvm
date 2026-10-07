<#
.SYNOPSIS
    ビルドして CTest を実行する。

.DESCRIPTION
    既定では release と debug の両方をビルドし、通常テストを実行する。
    通常テストは Smoke 素材を使い短時間で終わる。
    【操作可】通常の GUI テストは作業中のアプリより背面で描画し、フォーカス・マウス入力を
    受けず、タスクバーにも表示しない。実描画と合成入力の検証は継続する。

    性能計測 (LABELS performance) と安定性・診断 (LABELS stability) は
    既定では実行しない。両方を除外しないと、通常テストの所要時間に
    長時間の診断が混ざり、「通常テストが何件通ったか」が分からなくなる。

    -Portable は、特定の開発機環境 (Meiryo / D3D11VA hardware device) が必要な
    workstation ラベルも除外する。CI と日常の短縮検査で使用する。

    debug ビルドの性能値を判定に使わないため、-Performance は
    release でのみ意味を持つ。-Stability は診断が目的なので preset を問わない。

    通常テストは「ビルド種別依存」と「ビルド種別非依存」に分けて扱う。
    非依存とは、pwsh で起動し、引数に build/<preset>/bin を含まず、
    DEPENDS 関係にも関わらないテストを指す (契約・checker スクリプトの検査)。
    これらは release と debug で結果が変わらないため、1 回の呼び出しでは
    最初の preset でだけ実行する。判定できないものは依存側に倒す。

.PARAMETER Preset
    ucrt64-release / ucrt64-debug / both (既定)

.PARAMETER Jobs
    ctest の並列実行数 (既定 8)。1 を指定すると直列に戻る。

    workstation ラベル (実 GPU / audio endpoint / display を使うテスト) は
    RESOURCE_LOCK により、並列度に関わらず 1 件ずつ実行される。
    それ以外は並列に走る。

.PARAMETER Performance
    performance ラベルのテストも実行する。release のみ。

.PARAMETER Stability
    stability ラベル (長時間のメモリ診断など) も実行する。
    現時点では合否判定に使えないため、既定では実行しない。

.PARAMETER Fast
    extended ラベルの長い統合検査を通常テストから除外する。
    既存の CMake cache があれば明示的な再 configure を省く。
    CI の全件検査では指定しない。

.PARAMETER Group
    通常テストのうちどれを実行するか。
      All (既定)       : 両方。ただし非依存テストは最初の preset でだけ実行する
      BuildDependent   : ビルド種別依存のテストのみ
      BuildIndependent : ビルド種別非依存のテストのみ。ビルドせず configure だけ行う

.PARAMETER Shard
    'k/n' 形式。BuildIndependent の対象を n 分割した k 番目だけを実行する。
    CI で複数 job に分けて並列に走らせるために使う。

.PARAMETER WhisperRoot
    固定リビジョンの Whisper の導入先。CI は MSYS2 の package の版が固定と違うため、
    scripts/build-whisper.ps1 で構築した build/whisper-install を渡す。

.EXAMPLE
    pwsh scripts/test.ps1
    pwsh scripts/test.ps1 -Preset ucrt64-release -Performance
    pwsh scripts/test.ps1 -Preset ucrt64-release -Stability
    pwsh scripts/test.ps1 -Preset ucrt64-release -Group BuildDependent -Portable -Fast
    pwsh scripts/test.ps1 -Jobs 1                            # 直列に戻す
    pwsh scripts/test.ps1 -Preset ucrt64-release -Group BuildIndependent -Shard 1/3
#>
[CmdletBinding()]
param(
    [ValidateSet('ucrt64-release', 'ucrt64-debug', 'both')]
    [string]$Preset = 'both',

    [ValidateRange(1, 256)]
    [int]$Jobs = 8,

    [switch]$Performance,
    [switch]$Stability,
    [switch]$Portable,
    [switch]$Fast,

    [ValidateSet('All', 'BuildDependent', 'BuildIndependent')]
    [string]$Group = 'All',

    [ValidatePattern('^[1-9][0-9]*/[1-9][0-9]*$')]
    [string]$Shard,

    [string]$Ucrt64 = 'C:\msys64\ucrt64',

    # 固定リビジョンの Whisper の導入先 (scripts/build-whisper.ps1 の出力)。
    # 省略時は build.ps1 の既定 (UCRT64 の package) を使う。
    [string]$WhisperRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'lib\ctest-selection.ps1')
. (Join-Path $PSScriptRoot 'lib\test-display-lease.ps1')

$shardIndex = 0
$shardCount = 1
if ($Shard) {
    if ($Group -ne 'BuildIndependent') {
        throw "-Shard は -Group BuildIndependent と組み合わせてください"
    }
    $shardIndex, $shardCount = $Shard.Split('/') | ForEach-Object { [int]$_ }
    if ($shardIndex -gt $shardCount) { throw "-Shard の k が n を超えています: $Shard" }
    $shardIndex -= 1
}
if ($Group -eq 'BuildIndependent' -and ($Performance -or $Stability)) {
    # configure だけでは性能・安定性テストの実行ファイルが無い。
    throw "-Group BuildIndependent は -Performance / -Stability と併用できません"
}

$RepoRoot = Split-Path -Parent $PSScriptRoot
$CTest    = Join-Path $Ucrt64 'bin\ctest.exe'
# 関数内から参照するため script scope へ明示的に写す。
$CTestJobs = $Jobs

$presets = if ($Preset -eq 'both') { @('ucrt64-release', 'ucrt64-debug') } else { @($Preset) }

$anyFailed = $false
# 種別ごとに件数を分けて報告する。合計だけだと
# 「通常テストが減っている」ことに気づけない。
$summary = @()
$lastGroupExit = 0
$normalExclude = Get-MvmNormalExcludePattern -Portable:$Portable -Fast:$Fast
$normalKind = if ($Fast) { '通常(短縮)' } elseif ($Portable) { '通常(portable)' } else { '通常' }

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
        try { Assert-MvmRequiredCTestCount -Total $total -Required:$Required } catch { $msg = $_.Exception.Message }
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
    # 各行を表示しながら要約照合用にも保持する。長い検査でも進捗が見える。
    $outLines = [System.Collections.Generic.List[string]]::new()
    & $script:CTest --output-on-failure --timeout 120 -j $script:CTestJobs @CTestArgs 2>&1 | ForEach-Object {
        $line = "$_"
        Write-Host $line
        [void]$outLines.Add($line)
    }
    $code = $LASTEXITCODE

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

# ビルド種別非依存のテスト名を登録順で返す。
# 判定材料は ctest --show-only=json-v1 の command と DEPENDS だけ。
# 取りこぼし (依存なのに非依存と判定) は debug での検証漏れになるため、
# 少しでも怪しいものは依存側に倒す。
function Get-BuildIndependentTestNames {
    param([string]$BuildDir, [string[]]$CTestArgs)

    $jsonLines = & $script:CTest --show-only=json-v1 @CTestArgs
    if ($LASTEXITCODE -ne 0) { throw "ctest --show-only=json-v1 が exit $LASTEXITCODE で失敗しました" }
    $tests = @((($jsonLines -join "`n") | ConvertFrom-Json).tests)

    return (Get-MvmBuildIndependentTestNames -Tests $tests -BuildDir $BuildDir)
}

# 非依存テストは 1 回の呼び出しで 1 度だけ実行する。
$independentDone = $false

$displayLease = $null
try {
if ($Group -ne 'BuildIndependent') {
    $displayLease = Start-MvmTestDisplayLease
}
foreach ($p in $presets) {
    Write-Host "`n=== $p ===" -ForegroundColor Cyan
    # 非依存テストだけなら実行ファイルは不要なので configure で止める。
    $buildArgs = @{ Preset = $p; Ucrt64 = $Ucrt64 }
    if ($WhisperRoot) { $buildArgs.WhisperRoot = $WhisperRoot }
    if ($Group -eq 'BuildIndependent') { $buildArgs.ConfigureOnly = $true }
    if ($Fast -and $Group -ne 'BuildIndependent') { $buildArgs.ReuseConfigure = $true }
    & (Join-Path $PSScriptRoot 'build.ps1') @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "ビルドに失敗しました: $p" }

    $buildDir = Join-Path $RepoRoot "build\$p"
    $env:PATH = "$Ucrt64\bin;$env:PATH"

    Push-Location $buildDir
    try {
        if ($displayLease) { $displayLease.AssertValid() }
        # 通常テスト: performance と stability の両方を除外する
        Write-Host "通常テストの除外ラベル: $normalExclude" -ForegroundColor Yellow
        $normalArgs = @('-LE', $normalExclude)
        $effectiveGroup = if ($Group -eq 'All' -and $independentDone) { 'BuildDependent' } else { $Group }

        if ($effectiveGroup -eq 'All') {
            Invoke-CTestGroup -Preset $p -Kind $normalKind -Required -CTestArgs $normalArgs
        } else {
            $independent = Get-BuildIndependentTestNames -BuildDir $buildDir -CTestArgs $normalArgs
            if ($independent.Count -eq 0) {
                # 分類が壊れて全件が依存側に倒れた可能性がある。黙って続けない。
                throw 'ビルド種別非依存のテストが 0 件です。分類処理を確認してください。'
            }
            $listDir = Join-Path $buildDir 'Testing'
            New-Item -ItemType Directory -Force $listDir | Out-Null
            $listFile = Join-Path $listDir 'mvm-build-independent-tests.txt'

            if ($effectiveGroup -eq 'BuildDependent') {
                Set-Content -LiteralPath $listFile -Value $independent -Encoding utf8NoBOM
                if ($Group -eq 'All') {
                    # 飛ばしたことを結果に残す。
                    Write-Host "ビルド種別非依存の $($independent.Count) 件は実行済みのため省略します" -ForegroundColor Yellow
                    $summary += [pscustomobject]@{
                        Preset = $p; Kind = "$normalKind 非依存"; Total = $independent.Count; Ran = 0
                        Failed = 0; Passed = 0; Exit = 0; Note = "$($presets[0]) で実行済み"
                    }
                } else {
                    Write-Host "ビルド種別非依存の $($independent.Count) 件は今回の選定から除外します" `
                        -ForegroundColor Yellow
                    $summary += [pscustomobject]@{
                        Preset = $p; Kind = "$normalKind 非依存"; Total = $independent.Count; Ran = 0
                        Failed = 0; Passed = 0; Exit = 0; Note = '指定により対象外'
                    }
                }
                Invoke-CTestGroup -Preset $p -Kind "$normalKind 依存" -Required `
                    -CTestArgs ($normalArgs + @('--exclude-from-file', $listFile))
            } else {
                $selected = @(for ($i = $shardIndex; $i -lt $independent.Count; $i += $shardCount) { $independent[$i] })
                Write-Host ("ビルド種別非依存 全 {0} 件のうち shard {1}/{2} の {3} 件を実行します" -f `
                    $independent.Count, ($shardIndex + 1), $shardCount, $selected.Count)
                # 空のリストを渡すと絞り込み無しと同じになり得るので、ここで止める。
                if ($selected.Count -eq 0) { throw "shard $Shard の対象が 0 件です" }
                Set-Content -LiteralPath $listFile -Value $selected -Encoding utf8NoBOM
                $kind = if ($Shard) { "$normalKind 非依存 $Shard" } else { "$normalKind 非依存" }
                Invoke-CTestGroup -Preset $p -Kind $kind -Required `
                    -CTestArgs ($normalArgs + @('--tests-from-file', $listFile))
            }
        }
        if ($lastGroupExit -ne 0) { $anyFailed = $true }
        if ($effectiveGroup -ne 'BuildDependent') { $independentDone = $true }

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
} finally {
    if ($displayLease) {
        try {
            $displayLease.AssertValid()
            Write-Host "描画試験の電源前提を終了: 観測=$($displayLease.Observations -join ', ')"
        } finally { $displayLease.Dispose() }
    }
}
Write-Host "`n全テスト通過" -ForegroundColor Green
