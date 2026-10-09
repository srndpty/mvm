# 新規の P4-5 証拠だけを集計し、同一 source と gate の完了を確認する。
[CmdletBinding()]
param([string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $repoRoot ('build/math-p45-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-Closure')
}
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '既存の証拠を上書きしません' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$records = [System.Collections.Generic.List[object]]::new()
$sourceSha = ''
$required = @{
    Focused = @('diagnostic', 'oracle', 'differential', 'focused', 'encode')
    Real = @('product')
    AlphaDomain = @('alpha-domain', 'nearest-binary', 'constants')
    RealPng = @('real-png')
    Regressions = @('inventory', 'regressions')
    BuildIndependent = @('independent')
    Lint = @('lint')
    Release = @('release')
    Review = @('product-symbols', 'diagnostic-symbols')
}
foreach ($stage in @('Focused', 'Real', 'AlphaDomain', 'RealPng', 'Regressions', 'BuildIndependent', 'Lint', 'Release', 'Review')) {
    $directory = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build') -Directory -Filter ('math-p45-*-' + $stage) |
        Sort-Object Name | Select-Object -Last 1
    if (-not $directory) { throw "証拠がありません: $stage" }
    $state = Get-Content -LiteralPath (Join-Path $directory.FullName 'source-state.json') -Raw | ConvertFrom-Json
    if (-not $sourceSha) { $sourceSha = $state.sourceSha256 }
    if ($sourceSha -ne $state.sourceSha256) { throw "source の集合が一致しません: $stage" }
    foreach ($source in $state.hashes) {
        if ((Get-FileHash -LiteralPath (Join-Path $repoRoot $source.path)).Hash -ne $source.sha256) {
            throw "検証後に source が変わっています: $($source.path)"
        }
    }
    $commands = @(Get-Content -LiteralPath (Join-Path $directory.FullName 'commands.json') -Raw | ConvertFrom-Json)
    if ($commands.Count -eq 0 -or @($commands | Where-Object { $_.exit -ne 0 }).Count -ne 0) { throw "gate に失敗があります: $stage" }
    foreach ($name in $required[$stage]) {
        if (@($commands | Where-Object { $_.name -eq $name }).Count -ne 1) { throw "必要な実行がありません: $stage/$name" }
    }
    $counts = @()
    foreach ($command in $commands) {
        $body = Get-Content -LiteralPath (Join-Path $directory.FullName ($command.name + '.log')) -Raw
        foreach ($match in [regex]::Matches($body, '100% tests passed(?:, 0 tests failed)? out of ([0-9]+)')) {
            if ([int]$match.Groups[1].Value -eq 0) { throw 'CTest が空振りです' }
            $counts += @{ name = $command.name; checks = [int]$match.Groups[1].Value }
        }
        foreach ($match in [regex]::Matches($body, '検査 ([0-9]+) 件、失敗 ([0-9]+) 件')) {
            if ([int]$match.Groups[1].Value -eq 0 -or [int]$match.Groups[2].Value -ne 0) { throw '画素検査が空振りまたは失敗です' }
            $counts += @{ name = $command.name; checks = [int]$match.Groups[1].Value }
        }
    }
    if ($stage -eq 'Real') {
        $product = Get-Content -LiteralPath (Join-Path $directory.FullName 'product/results.json') -Raw | ConvertFrom-Json
        if ($product.checks -lt 100 -or $product.failures -ne 0) { throw '製品 UI の受入が未完了です' }
        $counts += @{ name = 'product'; checks = $product.checks }
    }
    $records.Add(@{ stage = $stage; directory = $directory.FullName; counts = $counts; commands = $commands.Count })
}
$mutationDirectory = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build') -Directory -Filter 'math-p45-*-Mutations' |
    Sort-Object Name | Select-Object -Last 1
if (-not $mutationDirectory) { throw '変異証拠がありません' }
$mutationBaseline = Get-Content -LiteralPath (Join-Path $mutationDirectory.FullName 'baseline.json') -Raw | ConvertFrom-Json
$mutation = Get-Content -LiteralPath (Join-Path $mutationDirectory.FullName 'result.json') -Raw | ConvertFrom-Json
if ($mutationBaseline.sourceSha256 -ne $sourceSha -or $mutation.count -ne 10 -or $mutation.detected -ne 10 -or $mutation.restored -ne 10) {
    throw '同一 source の変異検出と復元が完了していません'
}
@{ sourceSha256 = $sourceSha; gates = $records; mutations = @{ directory = $mutationDirectory.FullName; result = $mutation }; acceptanceGatesComplete = $true } |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'results.json') -Encoding utf8NoBOM
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# P4-5 再開後の機械集計')
$lines.Add('')
$lines.Add("source SHA256: ``$sourceSha``")
$lines.Add('')
$lines.Add('| gate | 検査数 | 証拠 |')
$lines.Add('| --- | --- | --- |')
foreach ($record in $records) {
    $countsText = ($record.counts | ForEach-Object { $_.name + ': ' + $_.checks }) -join '、'
    $lines.Add('| ' + $record.stage + ' | ' + $countsText + ' | ' + $record.directory + ' |')
}
$lines.Add('| Mutations | 検出 10/10、byte 復元 10/10 | ' + $mutationDirectory.FullName + ' |')
$lines | Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'results.md') -Encoding utf8NoBOM
Write-Host "受入 gate の機械集計を保存しました: $EvidenceDirectory"
