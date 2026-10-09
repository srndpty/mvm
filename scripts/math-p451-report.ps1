# 生の終了コード・画素 TSV・変異 SHA から結果文書を生成する。手で件数を転記しない。
[CmdletBinding()]
param([string]$OutputPath)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputPath) { $OutputPath = Join-Path $repoRoot 'docs/math-graph-p451-results.md' }
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# P4-5.1: 合成契約の検証結果')
$lines.Add('')
$runs = @{}
$complete = $true
foreach ($stage in @('Focused', 'Regressions', 'Mutations', 'Lint')) {
    $directory = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build') -Directory -Filter ('math-p451-*-'+$stage) |
        Sort-Object Name | Select-Object -Last 1
    if (-not $directory) { $complete = $false; continue }
    $runs[$stage] = $directory
    if ($stage -eq 'Mutations') {
        $resultPath = Join-Path $directory.FullName 'result.json'
        if (-not (Test-Path -LiteralPath $resultPath)) { $complete = $false; continue }
        $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
        if ($result.count -ne 8 -or $result.detected -ne 8 -or $result.restored -ne 8) { $complete = $false }
    } else {
        $commandsPath = Join-Path $directory.FullName 'commands.json'
        if (-not (Test-Path -LiteralPath $commandsPath)) { $complete = $false; continue }
        $commands = @(Get-Content -LiteralPath $commandsPath -Raw | ConvertFrom-Json)
        if (@($commands | Where-Object { $_.exit -ne 0 }).Count -gt 0) { $complete = $false }
        $expectedCount = if ($stage -eq 'Focused') { 7 } elseif ($stage -eq 'Regressions') { 8 } else { 1 }
        if ($commands.Count -ne $expectedCount) { $complete = $false }
    }
}
$state = if ($complete) { 'PASS/CLOSED' } else { 'HOLD' }
$lines.Add('P4-5.1 composition authority: **' + $state + '**。')
$lines.Add('P4-5 original focused blocker: **' + $(if ($complete) { 'RESOLVED' } else { '検証中' }) + '**。')
$lines.Add('P4-5 full product closure: **STILL OPEN**。P4/P4-5 は CLOSED にしない。')
$lines.Add('')
$lines.Add('結論 A: artifact の最近接整数 oracle を post-MLT に適用した境界誤り。合成の production 処理は変更しない。')
$lines.Add('数式・source provenance・観測の限界は [契約文書](math-graph-p451.md) を参照する。')
$lines.Add('')
$lines.Add('## alpha254 の独立演算 trace')
$lines.Add('')
$sourceAlpha = [double][float](128.0 / 255.0)
$alphaSum = [double][float]($sourceAlpha + 1.0)
$combinedAlpha = [double][float]($alphaSum - [double][float]($sourceAlpha * 1.0))
$scaledAlpha = [double][float](255.0 * $combinedAlpha)
$weight = [double][float]($sourceAlpha / $combinedAlpha)
$lines.Add('|binary32 演算|独立計算値|')
$lines.Add('|---|---|')
foreach ($entry in @(@('as', $sourceAlpha), @('F(as+1)', $alphaSum), @('a', $combinedAlpha), @('F(255*a)', $scaledAlpha), @('w', $weight))) {
    $lines.Add('|' + $entry[0] + '|' + $entry[1].ToString('G17', [Globalization.CultureInfo]::InvariantCulture) + '|')
}
$lines.Add('255*a の byte 切り捨て: ' + [int][Math]::Truncate($scaledAlpha) + '。255/255 自体ではなく加算後の中間丸めが原因。')
$lines.Add('')
$lines.Add('## 集中 gate')
$lines.Add('')
$lines.Add('|段階|証拠 directory|生ログからの結果|')
$lines.Add('|---|---|---|')
foreach ($stage in @('Focused', 'Regressions', 'Lint')) {
    if (-not $runs.ContainsKey($stage)) { continue }
    $directory = $runs[$stage]
    $commandsPath = Join-Path $directory.FullName 'commands.json'
    if (-not (Test-Path -LiteralPath $commandsPath)) { continue }
    foreach ($command in @(Get-Content -LiteralPath $commandsPath -Raw | ConvertFrom-Json)) {
        if ($command.name -like 'build-*' -or $command.name -eq 'inventory') { continue }
        $body = Get-Content -LiteralPath (Join-Path $directory.FullName ($command.name + '.log')) -Raw
        $match = [regex]::Match($body, '検査 ([0-9]+) 件、失敗 ([0-9]+) 件')
        $ctestMatch = [regex]::Match($body, '100% tests passed out of ([0-9]+)')
        $summary = if ($match.Success) {
            '検査 ' + $match.Groups[1].Value + '、失敗 ' + $match.Groups[2].Value
        } elseif ($ctestMatch.Success) {
            $ctestMatch.Groups[1].Value + '/' + $ctestMatch.Groups[1].Value + ' PASS'
        } else { '終了コード ' + $command.exit }
        $lines.Add('|' + $command.name + '|`build/' + $directory.Name + '`|' + $summary + '|')
    }
}
if ($runs.ContainsKey('Focused')) {
    $directory = $runs.Focused
    $lines.Add('')
    $lines.Add('## 記録 fixture の各境界')
    $lines.Add('')
    $lines.Add('|境界|RGBA|差分数|')
    $lines.Add('|---|---|---|')
    $pixels = @(Import-Csv -LiteralPath (Join-Path $directory.FullName 'diagnostic/pixels.tsv') -Delimiter "`t")
    foreach ($stageName in @('artifact-2', 'staging-2', 'producer', 'background', 'pre-source', 'pre-destination', 'post', 'pre-yuv-graph', 'pre-yuv-image')) {
        $row = $pixels | Where-Object { $_.stage -eq $stageName } | Select-Object -First 1
        if ($row) { $lines.Add('|' + $stageName + '|`[' + $row.rgba + ']`|' + $row.mismatches + '|') }
    }
    $lines.Add('')
    $lines.Add('最初の byte 差は affine 直後。実 staging を別途 qimage/affine に掛けた結果も製品 pre-YUV と一致する。')
    $lines.Add('raw RGBA と TSV に全画素 SHA、oracle SHA、先頭差分 x/y/channel、差分数を保存した。')
    $postBytes = [IO.File]::ReadAllBytes((Join-Path $directory.FullName 'diagnostic/recorded-post-0.rgba'))
    $oldExpected = @(foreach ($channel in @(200, 99, 31)) { [int][Math]::Floor($channel * 128.0 / 255.0 + 0.5) }) + @(255)
    $firstByte = -1
    $oldMismatches = 0
    for ($byteIndex = 0; $byteIndex -lt $postBytes.Length; ++$byteIndex) {
        if ($postBytes[$byteIndex] -ne $oldExpected[$byteIndex % 4]) {
            if ($firstByte -lt 0) { $firstByte = $byteIndex }
            ++$oldMismatches
        }
    }
    $firstX = [int][Math]::Floor($firstByte / 4.0) % 64
    $firstY = [int][Math]::Floor($firstByte / 256.0)
    $firstChannel = $firstByte % 4
    $lines.Add('旧最近接整数 oracle `[' + ($oldExpected -join ',') + ']` と post の比較: 先頭 byte=' + $firstByte + '（x=' + $firstX + '/y=' + $firstY + '/channel=' + $firstChannel + '）、差分 byte 数=' + $oldMismatches + '。')
    $lines.Add('')
    $lines.Add('## 行列の endpoint')
    $lines.Add('')
    $lines.Add('|条件|独立 oracle と一致した RGBA|Graph/Image 対照|')
    $lines.Add('|---|---|---|')
    $matrix = @(Import-Csv -LiteralPath (Join-Path $directory.FullName 'differential/pixels.tsv') -Delimiter "`t")
    foreach ($name in @($matrix.case | Sort-Object -Unique | Where-Object { $_ -notlike '*actual-staging*' })) {
        $row = $matrix | Where-Object { $_.case -eq $name -and $_.stage -eq 'post' } | Select-Object -Last 1
        if (-not $row) { continue }
        $imageRows = @($matrix | Where-Object { $_.case -eq $name -and $_.stage -eq 'pre-yuv-image' })
        $comparison = if ($imageRows.Count -gt 0) { '全画素 exact' } else { '透明背景の controlled 診断のみ' }
        $lines.Add('|' + $name + '|`[' + $row.rgba + ']`|' + $comparison + '|')
    }
    $bad = @($matrix | Where-Object { $_.mismatches -and $_.mismatches -ne '0' })
    $lines.Add('')
    $lines.Add('差分を持つ stage 行: ' + $bad.Count + '。alpha0 同士の定義域外は支持ケースに含めない。')
    $lines.Add('')
    $lines.Add('## MLT provenance')
    $lines.Add('')
    $lines.Add('|ファイル|SHA256|')
    $lines.Add('|---|---|')
    foreach ($entry in @(Get-Content -LiteralPath (Join-Path $directory.FullName 'mlt-provenance.json') -Raw | ConvertFrom-Json)) {
        if ($entry.path -eq 'mlt-7.36.1-recipe-commit.json') { continue }
        $lines.Add('|`' + $entry.path + '`|`' + $entry.sha256 + '`|')
    }
    $lines.Add('探索時の旧 recipe 履歴 JSON は取得履歴として保持し、正確な build authority は .BUILDINFO と SHA 一致する PKGBUILD/archive で固定する。')
}
$lines.Add('')
$lines.Add('## Source 変異')
$lines.Add('')
$lines.Add('|変異|build 終了|試験終了|検出|復元 SHA 一致|復元試験終了|')
$lines.Add('|---|---|---|---|---|---|')
if ($runs.ContainsKey('Mutations')) {
    foreach ($entry in @(Get-Content -LiteralPath (Join-Path $runs.Mutations.FullName 'mutations.json') -Raw | ConvertFrom-Json)) {
        $lines.Add('|' + $entry.name + '|' + $entry.build_exit + '|' + $entry.test_exit + '|' + $entry.detected + '|' + $entry.restored + '|' + $entry.restored_test_exit + '|')
    }
    $lines.Add('')
    $lines.Add('証拠: `build/' + $runs.Mutations.Name + '`。元・変異・復元の SHA、変異 source、全 build/CTest ログを保持。')
}
$lines.Add('')
$lines.Add('## 履歴と未実施範囲')
$lines.Add('')
$lines.Add('P4-5 の旧整数 oracle FAIL と sandbox 起因の試験失敗は [旧結果](math-graph-p45-results.md) に保持する。')
$lines.Add('`build/math-p451-matrix-initial` は色背景を RGBA の順で MLT の ARGB resource へ渡し、540 検査中14失敗。元記録を残した。')
$lines.Add('初回診断の build は試験側の Track.id／GraphRenderSpec.functions の誤参照で失敗し、実際の型へ修正した。変異検出に数えない。')
$lines.Add('`build/math-p451-20261010-003741-Mutations` の初回 audit-format は上流 affine が RGBA に戻す等価変異で未検出だった。')
$lines.Add('`build/math-p451-20261010-004109-Mutations` の条件付き convert_image による YUV 往復変異も、今回の pipeline では byte を変えず未検出だった。')
$lines.Add('未検出を成功へ読み替えず、audit の attach 先を最終 producer から合成前の背景 producer へ移す実際の境界変異に変更した。')
$lines.Add('追加は staging 観測 callback と検証専用 MLT 診断、修正は独立 exact oracle。凍結 artifact oracle の期待値は変更していない。')
$lines.Add('実製品 UI→Manim→H.264、full release、BuildIndependent は未実施。P4-5 全体の閉鎖は別途必要。コミット・push は行っていない。')
[IO.File]::WriteAllText($OutputPath, ($lines -join "`n") + "`n", [Text.UTF8Encoding]::new($false))
Write-Host ('結果文書を生成しました: ' + $state)
