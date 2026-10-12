<#
.SYNOPSIS
    P3-3A Task A: math_equation_sequence_product_ui の起動直後の SegFault を、事前に決めた回数だけ
    同じ build・同じ環境で再現させてみる。

.DESCRIPTION
    回数と内容は実行の前に固定する (結果を見て増減しない)。build はしない。
      R1  元の失敗と同じ 49 件の cohort (同じ正規表現・同じ順序) を 3 回
      R2  直前の試験との組 (math_inspector_product_ui -> math_equation_sequence_product_ui) を 5 回
      R3  試験の command を procdump の下で直接 5 回 (未処理の例外で full dump を採る)
    1 回ごとに ctest の log と終了コードを残す。既存の出力先は上書きしない。
    再現しなくても「解決」とは扱わない (docs/math-transition-p33a.md)。
#>
[CmdletBinding()]
param([string]$OutputDirectory = 'build/math-transition-p33a-20261012/task-a')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo 'build\ucrt64-release'
$exe = Join-Path $build 'bin\mvm_test_text_ui_input.exe'
$out = Join-Path (Join-Path $repo $OutputDirectory) ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-cohort')
if (Test-Path -LiteralPath $out) { throw "出力先が既にあります: $out" }
New-Item -ItemType Directory -Force $out | Out-Null
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'

$identity = [ordered]@{
    started = (Get-Date -Format o)
    exe_sha256 = (Get-FileHash -Algorithm SHA256 $exe).Hash
    exe_last_write = (Get-Item $exe).LastWriteTime.ToString('o')
    git_head = (git -C $repo rev-parse HEAD)
    git_status = @(git -C $repo status --short)
    cohort_regex = 'math|mlt_export|m7b_3_product_export|timeline_export'
    plan = 'R1 x3, R2 x5, R3 x5 (事前に固定)'
}
$identity | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $out 'identity.json') -Encoding utf8

$rows = @()
function Invoke-Ctest([string]$Name, [string]$Regex) {
    $log = Join-Path $out "$Name.log"
    $begin = Get-Date
    ctest --test-dir $build -R $Regex --output-on-failure -O $log 2>&1 | Out-Null
    $code = $LASTEXITCODE
    $text = Get-Content -Raw $log
    $target = ([regex]'Test\s+#1485: math_equation_sequence_product_ui \.+\s*(.+)').Match($text).Groups[1].Value.Trim()
    $failed = @([regex]::Matches($text, '(?m)^\s*\d+ - (\S+) \(') | ForEach-Object { $_.Groups[1].Value })
    [pscustomobject]@{ run = $Name; exit_code = $code; target = $target; failed = ($failed -join ','); began = $begin.ToString('o');
        seconds = [int]((Get-Date) - $begin).TotalSeconds }
}

for ($i = 1; $i -le 3; ++$i) {
    $row = Invoke-Ctest "R1-cohort-$i" $identity.cohort_regex
    $rows += $row; Write-Host ($row | ConvertTo-Json -Compress)
}
for ($i = 1; $i -le 5; ++$i) {
    $row = Invoke-Ctest "R2-pair-$i" '^(math_inspector_product_ui|math_equation_sequence_product_ui)$'
    $rows += $row; Write-Host ($row | ConvertTo-Json -Compress)
}
$procdump = (Get-Command procdump -ErrorAction SilentlyContinue)?.Source
for ($i = 1; $i -le 5; ++$i) {
    $name = "R3-procdump-$i"
    $begin = Get-Date
    if ($procdump) {
        $dumps = Join-Path $out "$name-dumps"
        New-Item -ItemType Directory -Force $dumps | Out-Null
        & $procdump -accepteula -e -ma -x $dumps $exe --equation-sequence-ui (Join-Path $repo 'build/p351-ui') --scratch *> (Join-Path $out "$name.log")
        $code = $LASTEXITCODE
        $count = @(Get-ChildItem $dumps -Filter *.dmp -ErrorAction SilentlyContinue).Count
        $row = [pscustomobject]@{ run = $name; exit_code = $code; target = "dumps=$count"; failed = ''; began = $begin.ToString('o');
            seconds = [int]((Get-Date) - $begin).TotalSeconds }
    } else {
        $row = [pscustomobject]@{ run = $name; exit_code = $null; target = 'procdump がありません (未実行)'; failed = ''; began = $begin.ToString('o'); seconds = 0 }
    }
    $rows += $row; Write-Host ($row | ConvertTo-Json -Compress)
}
$rows | ConvertTo-Json | Set-Content (Join-Path $out 'summary.json') -Encoding utf8
Write-Host "結果: $out"
