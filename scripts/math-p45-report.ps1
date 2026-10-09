# 生ログと source snapshot から P4-5 の状態を再計算する。失敗を閉鎖へ読み替えない。
[CmdletBinding()]
param([string]$OutputPath = 'docs/math-graph-p45-results.md')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$lines = [Collections.Generic.List[string]]::new()
$lines.Add('# P4-5 の検証結果')
$lines.Add('')
$lines.Add('状態: **HOLD**。encoder 直前の独立 RGBA oracle が未通過。P4 と P4-5 を PASS/CLOSED にしていない。')
$lines.Add('')
$lines.Add('以下は新規の生ログから再計算した件数。通常 release、BuildIndependent、実 Manim と製品 UI、変異の閉鎖 gate は未実施。過去の P3/P4 の証拠は変更していない。')
$lines.Add('')
$lines.Add('|取得|処理|終了コード|生ログの検査件数|証拠|')
$lines.Add('|---|---|---:|---|---|')
$directories = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build') -Directory -Filter 'math-p45-*' | Sort-Object Name)
foreach ($directory in $directories) {
    $commandsPath = Join-Path $directory.FullName 'commands.json'
    if (-not (Test-Path -LiteralPath $commandsPath)) { continue }
    $commands = @(Get-Content -LiteralPath $commandsPath -Raw | ConvertFrom-Json)
    foreach ($command in $commands) {
        $logPath = Join-Path $directory.FullName ($command.name + '.log')
        $summary = '該当する集計なし'
        if (Test-Path -LiteralPath $logPath) {
            $log = Get-Content -LiteralPath $logPath -Raw
            $found = [regex]::Match($log, '検査 (\d+) 件、失敗 (\d+) 件')
            if ($found.Success) { $summary = $found.Groups[1].Value + ' 件、失敗 ' + $found.Groups[2].Value + ' 件' }
            elseif ([regex]::IsMatch($log, '100% tests passed out of ([1-9][0-9]*)')) {
                $passed = [regex]::Match($log, '100% tests passed out of ([1-9][0-9]*)')
                $summary = $passed.Groups[1].Value + ' 件、失敗 0 件'
            }
            elseif ($log -match 'lint 通過') { $summary = '静的検査通過' }
        }
        $name = $directory.Name
        $lines.Add("|$name|$($command.name)|$($command.exit)|$summary|[生証拠](../build/$name/)|")
    }
}
$lines.Add('')
$lines.Add('## 画素の失敗と独立対照')
$lines.Add('')
$lines.Add('Graph の静止 endpoint の straight RGBA は `[200,99,31,128]`。黒い不透明背景との最近接整数 source-over の期待値と、実際の MLT 合成後／YUV 変換前の byte を比較する。')
$lines.Add('')
$lines.Add('|取得|期待 RGBA|実測 RGBA|')
$lines.Add('|---|---|---|')
foreach ($directory in $directories) {
    $rawPath = Join-Path $directory.FullName 'encode/encoder-10.rgba'
    if (-not (Test-Path -LiteralPath $rawPath)) { continue }
    $raw = [IO.File]::ReadAllBytes($rawPath)
    if ($raw.Length -ne 64 * 36 * 4) { throw '保存した canonical RGBA の byte 数が不正です' }
    $actual = @($raw[0..3]) -join ','
    $expected = @([int][Math]::Floor((200 * 128 + 127) / 255), [int][Math]::Floor((99 * 128 + 127) / 255), [int][Math]::Floor((31 * 128 + 127) / 255), 255) -join ','
    $lines.Add("|$($directory.Name)|``[$expected]``|``[$actual]``|")
}
$lines.Add('')
$lines.Add('通常 Image の対照は同じ straight PNG を使い、Graph の compile／成果物 loader／source timing を通さない。対照の全 frame が Graph endpoint と全画素一致した assertion は encode.log に残す。Graph の画素問題を「無関係」として除外する根拠には使わない。')
$lines.Add('')
$lines.Add('既存 MLT 7.36.1 の [interp.h](https://github.com/mltframework/mlt/blob/v7.36.1/src/modules/plus/interp.h) は float の source-over を uint8_t へ切り捨てる。Graph artifact 内の凍結した最近接整数丸めと、外側の既存 MLT 合成は異なる。Graph だけの別合成や許容差を追加せず、既存の丸めを保存したため、この oracle は未通過である。')
$lines.Add('')
$lines.Add('## ソースの来歴')
$lines.Add('')
foreach ($directory in $directories) {
    $statePath = Join-Path $directory.FullName 'source-state.json'
    if (-not (Test-Path -LiteralPath $statePath)) { continue }
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
    $shaProperty = $state.PSObject.Properties['sourceSha256']
    $sha = if ($shaProperty) { $shaProperty.Value } else { (Get-FileHash -LiteralPath $statePath -Algorithm SHA256).Hash }
    $lines.Add("- $($directory.Name): HEAD ``$($state.revision)``、source snapshot SHA256 ``$sha``。各ファイルは [source-state.json](../build/$($directory.Name)/source-state.json) と ``sources/`` に固定。")
}
$lines.Add('')
$lines.Add('## 初期失敗と未達条件')
$lines.Add('')
$lines.Add('- 最初のビルドは試験の Track.visible が存在しないため失敗。製品の既存 Track.muted に試験を修正した。sandbox 内ビルドの停止は公式 build の sandbox 外実行で回避し、source／Ninja metadata の修復はしていない。')
$lines.Add('- 通常 Image の最初の対照は、Image に Graph data を残した試験 fixture の構造不正で失敗。Graph data を空にして新規取得し、過去の失敗 directory は保存した。')
$lines.Add('- 既存 controller 試験の sandbox 内実行は QTemporaryDir の初期保存で失敗。build/math-p45-editor-sandbox-failure に LastTest と失敗一覧を退避した。同じ source の公式 build に変更対象の再コンパイルは無く、sandbox 外では一件通過した。制限外でも同じ失敗が出ることは確認されていない。')
$lines.Add('- 共通 key の静止所有者と Draw 所有者は先に集約して、一つの Draw package だけを準備する。台帳と一枚の const RGBA owner は preview residency に依存しない。')
$lines.Add('- 最終 oracle が通過する合成 authority は未確定。実 Manim → 製品 QML → H.264、全取消境界、普通の音声／字幕との組合せ、全変異、BuildIndependent、通常 release は閉鎖していない。要求 output 範囲を持つ planner は実装したが、製品 exporter は従来どおり timeline 全体を出力する。')
$lines.Add('- コミット・push は行っていない。P4-5 の実装・閉鎖は未完了。')
[IO.File]::WriteAllLines((Join-Path $repoRoot $OutputPath), $lines, [Text.UTF8Encoding]::new($false))
