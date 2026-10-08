# 生の終了コード・CTest 集計・UI assertion を結果文書へ再計算する。
[CmdletBinding()]
param([string]$OutputPath = 'docs/math-graph-p44-results.md')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$gitTool = Get-Command git -ErrorAction SilentlyContinue
$gitExe = if ($gitTool) { $gitTool.Source } else { 'C:\Program Files\Git\cmd\git.exe' }
if (-not (Test-Path -LiteralPath $gitExe)) { throw 'git がありません' }
$head = (& $gitExe -C $repoRoot rev-parse HEAD).Trim()
$originalController = (& $gitExe -C $repoRoot show HEAD:apps/mvm/mvm_controller.cpp) -join "`n"
$currentController = [IO.File]::ReadAllText((Join-Path $repoRoot 'apps/mvm/mvm_controller.cpp')).Replace("`r`n", "`n")
$reusePattern = '(?s)QString graphMemo;.*?auto composition = std::make_shared<preview::CompositionSnapshot>\(\);'
$originalReuse = [regex]::Match($originalController, $reusePattern)
$currentReuse = [regex]::Match($currentController, $reusePattern)
if (-not $originalReuse.Success -or -not $currentReuse.Success -or $originalReuse.Value -cne $currentReuse.Value) {
    throw '既存の Graph composition 再利用契約が一致しません'
}
& $gitExe -C $repoRoot diff --exit-code -- 'docs/math-graph-p*.md' 'docs/math-equation-sequence-p*.md' | Out-Null
if ($LASTEXITCODE -ne 0) { throw '歴史的な結果文書が変更されています' }
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# P4-4 の検証結果')
$lines.Add('')
$lines.Add("基準 HEAD: ``$head``。`stableGraph` / `graphMemo` の契約範囲は HEAD と文字完全一致。追跡済みの歴史的 Graph / EquationSequence 文書に差分はない。")
$lines.Add('')
$lines.Add('P4-4 の判定はこの生結果と設計文書で確認する。P4-5 は開始せず、コミット・push は行わない。')
$lines.Add('')
$lines.Add('初回の Flow と fixture、native style 警告、同値編集と削除後選択の試験期待の誤り、診断の型誤り、途中で停止した重複 build はすべて保存する。')
$lines.Add('成立後に追加のイベントを処理して待機条件を再評価する helper の不具合は、queued event の決定的な負例を添えて修正した。')
$lines.Add('初回の再起動失敗がこの同じ原因だったかは過去 run の診断が不足しており未確定。現在の修正済み待機と native 全画素比較を独立の根拠とする。')
$lines.Add('Final-Focused と Final-Regressions の `graph_editor_controller` 失敗は、その後の試験修正より前の source である。通常 release は修正後の試験をビルドして通過した。')
$lines.Add('最初の BuildIndependent は Escape で拒否 draft を現在の確定値へ戻す修正より前の QML である。BuildIndependent-02 は最終 QML で 1084 件を通過した。')
$lines.Add('Final-Mutations は CP932 コンソールのリダイレクトが UTF-8 の失敗文を崩し、assertion 文字列の照合だけが不成立だった。試験は終了コード 8 で該当拒否が失敗している。Final-Mutations-02 は UTF-8 で捕捉し、6 件すべてを検出して source を復元した。')
$lines.Add('')
$lines.Add('|取得|終了コード|生結果の集計|証拠|')
$lines.Add('|---|---:|---|---|')
foreach ($directory in Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build') -Directory -Filter 'math-p44-*' | Sort-Object Name) {
    $resultPath = Join-Path $directory.FullName 'result.json'
    $commandPath = Join-Path $directory.FullName 'commands.json'
    $mutationPath = Join-Path $directory.FullName 'mutations.json'
    $result = if (Test-Path -LiteralPath $resultPath) { Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json } else { $null }
    $commands = @()
    if (Test-Path -LiteralPath $commandPath) { $commands = @(Get-Content -LiteralPath $commandPath -Raw | ConvertFrom-Json) }
    $exitText = if ($result) { [string]$result.exit } elseif ($commands.Count) { ($commands | ForEach-Object { $_.exit }) -join ',' } else { '未記録' }
    $summary = [System.Collections.Generic.List[string]]::new()
    $uiPath = Join-Path $directory.FullName 'product/results.json'
    if (Test-Path -LiteralPath $uiPath) {
        $ui = Get-Content -LiteralPath $uiPath -Raw | ConvertFrom-Json
        $assertions = @($ui.results)
        if ($assertions.Count -ne $ui.checks -or @($assertions | Where-Object { -not $_.ok }).Count -ne $ui.failures) {
            throw 'UI の集計と assertion が一致しません'
        }
        $summary.Add("UI $($ui.checks) 検査、失敗 $($ui.failures)")
        $pixels = @($assertions | Where-Object { $_.check -match 'native 全画素の独立 RGBA 比較' })
        if ($pixels.Count) { $summary.Add("原寸全画素比較 $($pixels.Count) frame、失敗 $(@($pixels | Where-Object { -not $_.ok }).Count)") }
    }
    if (Test-Path -LiteralPath $mutationPath) {
        $mutations = @(Get-Content -LiteralPath $mutationPath -Raw | ConvertFrom-Json)
        $summary.Add("変異 $(@($mutations | Where-Object detected).Count)/$($mutations.Count) 検出")
    }
    foreach ($log in Get-ChildItem -LiteralPath $directory.FullName -File -Filter '*.log') {
        $body = Get-Content -LiteralPath $log.FullName -Raw
        foreach ($matched in [regex]::Matches($body, '(\d+)% tests passed(?:, (\d+) tests failed)? out of (\d+)')) {
            $failed = if ($matched.Groups[2].Success) { [int]$matched.Groups[2].Value } else { 0 }
            $summary.Add("$($log.Name): $($matched.Groups[3].Value) 件中 $failed 失敗")
        }
    }
    if ($summary.Count -eq 0) { $summary.Add('build・初期化・終了のログを参照') }
    $lines.Add("|$($directory.Name)|$exitText|$($summary -join ' / ')|[生証拠](../build/$($directory.Name)/)|")
}
$lines.Add('')
$lines.Add('各 gate の `source-state.json` は HEAD とソース SHA256、`sources/` は当該取得の実ソース。変異は original SHA256 の完全一致復元と復元後の正常試験を要求する。')
$lines.Add('件数は上の生 JSON と CTest log から生成し、性能値を手で転記していない。')
[IO.File]::WriteAllLines((Join-Path $repoRoot $OutputPath), $lines, [Text.UTF8Encoding]::new($false))
