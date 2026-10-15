# scripts/lib/crash-archive.ps1 の契約。
# 実際の archive (%LOCALAPPDATA%\mvm\crash-archive) には触れず、一時 directory の archive で確かめる。
# 偽の dump (中身を決めた小さな file) を使い、WER は呼ばない。
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Library)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $Library

$failures = 0
function Check([bool]$Condition, [string]$Message) {
    if ($Condition) { Write-Host "PASS: $Message" }
    else { Write-Host "FAIL: $Message" -ForegroundColor Red; $script:failures++ }
}
function Throws([scriptblock]$Block, [string]$Pattern) {
    try { & $Block; return $false } catch { return $_.Exception.Message -match $Pattern }
}
function Write-Dump([string]$Path, [string]$Text) {
    New-Item -ItemType Directory -Force (Split-Path $Path) | Out-Null
    [IO.File]::WriteAllText($Path, $Text)
}
function Set-Limits([string]$Root, [long]$Max, [long]$MinFree) {
    @{ max_bytes = $Max; min_free_bytes = $MinFree } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Root 'config.json') -Encoding utf8NoBOM
}
function Records([string]$Root) {
    @(Get-Content -LiteralPath (Join-Path $Root 'index.jsonl') | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
}

$temp = Join-Path ([IO.Path]::GetTempPath()) "mvm-crash-archive-test-$PID-$([Environment]::TickCount64)"
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $root = Join-Path $temp 'archive'
    $src1 = Join-Path $temp 'CrashDumps1'
    $src2 = Join-Path $temp 'CrashDumps2'

    # 1. 置き場所の検査
    $repo = (Resolve-Path (Join-Path (Split-Path $Library) '..\..')).Path
    Check (Throws { Get-MvmCrashArchiveRoot (Join-Path $repo 'build\crash') } 'repo の中') 'repo の中 (build/) の archive を拒否する'
    $file = Join-Path $temp 'not-a-directory'
    Set-Content -LiteralPath $file -Value 'x'
    Check (Throws { Open-MvmCrashArchive -Root $file } 'directory ではありません') 'archive の場所が file なら失敗する'

    # 2. 保全・同じ名前の別の dump・再実行
    Write-Dump (Join-Path $src1 'app.exe.100.dmp') 'dump-A'
    Write-Dump (Join-Path $src2 'app.exe.100.dmp') 'dump-B-different'
    Write-Dump (Join-Path $src2 'copy-of-a.dmp') 'dump-A'
    $a = Open-MvmCrashArchive -Root $root
    try {
        $r = Save-MvmCrashDumps -Archive $a -Directory @($src1, $src2) -Context 'contract'
        $byStatus = $r | Group-Object status -AsHashTable
        Check ($r.Count -eq 3) '3 件を数える'
        Check (@(Get-ChildItem (Join-Path $root 'objects')).Count -eq 2) '同じ名前の別内容は 2 つの object になり、同じ内容は 1 つにまとまる'
        Check ($byStatus['archived'].Count -eq 2 -and $byStatus['already_object'].Count -eq 1) '状態: archived 2、already_object 1'
        $sha = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes('dump-A')))
        Check (Test-Path (Join-Path $root "objects\$sha.dmp")) 'object の名前は内容の SHA-256'
        $records = Records $root
        Check (@($records | Where-Object { $_.kind -eq 'dump' }).Count -eq 3) 'index に 3 件の元 path を記録する'
        Check (@($records | Where-Object { $_.original_path -eq (Join-Path $src2 'app.exe.100.dmp') }).Count -eq 1) '元の path を記録する'
        $again = Save-MvmCrashDumps -Archive $a -Directory @($src1, $src2)
        Check (@($again | Where-Object { $_.status -ne 'already_indexed' }).Count -eq 0) '再実行では何も増やさない'
        Check ((Records $root).Count -eq $records.Count) '再実行で index を増やさない'
        Check (@(Get-ChildItem (Join-Path $root 'incoming')).Count -eq 0) '一時 file を残さない'

        # 3. 容量の上限・空きの下限: 1 件も書かずに失敗する
        Write-Dump (Join-Path $src1 'big.dmp') ('x' * 4096)
        $before = @(Get-ChildItem (Join-Path $root 'objects')).Count
        $indexBefore = (Records $root).Count
        $a.MaxBytes = 100
        Check (Throws { Save-MvmCrashDumps -Archive $a -Directory $src1 } '上限を超えます') '上限を超える保全は失敗する'
        $a.MaxBytes = 64GB
        $a.MinFreeBytes = [long]::MaxValue / 2
        Check (Throws { Save-MvmCrashDumps -Archive $a -Directory $src1 } '空きが足りません') 'volume の空きが下限を割る保全は失敗する'
        $a.MinFreeBytes = 0
        Check (@(Get-ChildItem (Join-Path $root 'objects')).Count -eq $before -and (Records $root).Count -eq $indexBefore) '容量の失敗では object も index も増えない'

        # 4. 読めない dump (WER が書いている途中など) は保全の失敗
        $lock = [IO.File]::Open((Join-Path $src1 'big.dmp'), 'Open', 'ReadWrite', 'None')
        try {
            Check (Throws { Save-MvmCrashDumps -Archive $a -Directory $src1 } '.') '開けない dump があれば保全は失敗する'
        } finally { $lock.Dispose() }

        # 5. 保全に失敗したら Action (probe の起動) を実行しない
        $ran = $false
        $a.MaxBytes = 100
        Check (Throws { Invoke-MvmAfterCrashArchive -Archive $a -Directory $src1 -Context 't' -Action { $script:ran = $true } } '上限') '保全の失敗を例外にする'
        Check (-not $ran) '保全に失敗したら probe を起動しない'
        $a.MaxBytes = 64GB
        Invoke-MvmAfterCrashArchive -Archive $a -Directory $src1 -Context 't' -Action { $script:ran = $true }
        Check $ran '保全に成功したら probe を起動する'

        # 6. run の directory と小さな記録の複製は上書きしない
        $run = New-MvmCrashArchiveRun -Archive $a -Name 'contract'
        Check (Throws { New-MvmCrashArchiveRun -Archive $a -Name 'contract' } '既にあります') '同じ run の directory を作り直さない'
        Check (Throws { New-MvmCrashArchiveRun -Archive $a -Name 'bad/name' } '使えない文字') 'run の名前に path の区切りを許さない'
        $log = Join-Path $temp 'ctest.log'
        Set-Content -LiteralPath $log -Value 'log'
        Copy-MvmCrashArchiveFile -Archive $a -Source $log -Destination (Join-Path $run 'ctest.log')
        Check (Throws { Copy-MvmCrashArchiveFile -Archive $a -Source $log -Destination (Join-Path $run 'ctest.log') } '.') '同じ名前の記録を上書きしない'
        Check (Throws { Copy-MvmCrashArchiveFile -Archive $a -Source $log -Destination (Join-Path $temp 'outside.log') } 'runs/ の外') 'runs/ の外へ書かない'

        # 7. 消すのは明示したときだけ
        $preview = Remove-MvmCrashArchiveObject -Archive $a -Sha256 $sha -Reason 'contract test の確認'
        Check ($preview.Count -eq 1 -and (Test-Path (Join-Path $root "objects\$sha.dmp"))) '-Apply が無ければ消さない'
        Check (Throws { Remove-MvmCrashArchiveObject -Archive $a -Sha256 $sha -Reason 'x' -Apply } '理由') '理由の無い削除を拒否する'
        Remove-MvmCrashArchiveObject -Archive $a -Sha256 $sha -Reason 'contract test の確認' -Apply | Out-Null
        Check (-not (Test-Path (Join-Path $root "objects\$sha.dmp"))) '-Apply で消す'
        Check (@(Records $root | Where-Object { $_.kind -eq 'removed' -and $_.sha256 -eq $sha }).Count -eq 1) '削除を index に記録する'

        # 8. 書き手は 1 つ
        Check (Throws { Open-MvmCrashArchive -Root $root -LockTimeoutSeconds 1 } 'lock') '別の書き手が開いている間は開けない'
        $u = Get-MvmCrashArchiveUsage $a
        Check ($u.bytes -gt 0 -and $u.objects -ge 2 -and $u.runs -eq 1) '使用量を報告する'
    } finally { Close-MvmCrashArchive $a }

    # 9. 設定の不正
    Set-Limits $root -1 0
    Check (Throws { Open-MvmCrashArchive -Root $root } 'max_bytes') 'config.json の不正な上限を拒否する'
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures -gt 0) { Write-Host "$failures 件失敗"; exit 1 }
Write-Host '全件通過'
