<#
.SYNOPSIS
    実行 file を debugger (gdb) の下で起動し、不正な memory の参照で止まったら失敗にする。

.DESCRIPTION
    Windows は、debugger の下で起動した process に debug heap を使わせる。debug heap は解放した
    memory を 0xFEEEFEEE で埋めるので、通常の起動では落ちない「解放後の参照」(解放した object の中の
    pointer や mutex を使う) が、決まった場所で access violation になる。UCRT64 の toolchain には
    AddressSanitizer が無いので、その代わりに使う (2026-10-13、PreviewEngine::removeSource の
    解放後の参照をこの方法で再現した。docs/preview-audio-teardown-p35a1.md)。

    検出できるのは「解放後に読んだ値を pointer や handle として使う」形だけである。解放後の memory を
    読むだけ・書くだけで落ちない誤りは、この方法では見つからない。

    終了コード:
      0  正常に終了した (子の終了コードが 0)
      1  signal で止まった (SIGSEGV など)。backtrace を出力する
      2  子が 0 以外の終了コードで終わった
      3  引数・環境の誤り (gdb が無い、実行 file が無い、gdb の出力を解釈できない)
    -ExpectSignal を付けると判定を反転する (対照用): signal で止まれば 0、止まらなければ 1。
    -SignalOnly を付けると、子の終了コードを見ない (signal で止まらなければ 0)。実時間の再生を
    検査する試験は、debugger の下では遅くなって自分の検査に落ちることがある。その検査は通常の
    起動の試験が受け持ち、ここでは memory の誤りだけを見る。

.EXAMPLE
    pwsh scripts/run-under-debug-heap.ps1 -Exe build/ucrt64-release/bin/mvm_p5e_preview_smoke.exe `
        -ChildArgs 'a.mp4|b.mp4|--remove-audio-source'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Exe,
    # 子へ渡す引数を | で区切る (CTest から配列を渡せないため)。
    [string]$ChildArgs = '',
    [string]$Gdb = 'C:\msys64\ucrt64\bin\gdb.exe',
    # backtrace を含む gdb の出力を、この file にも残す (省略可)。
    [string]$LogFile = '',
    [switch]$ExpectSignal,
    [switch]$SignalOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $Gdb -PathType Leaf)) {
    Write-Host "gdb がありません: $Gdb" -ForegroundColor Red
    exit 3
}
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    Write-Host "実行ファイルがありません: $Exe" -ForegroundColor Red
    exit 3
}
# debug heap を止める設定が残っていると、この検査は何も検出しない。
if ($env:_NO_DEBUG_HEAP) {
    Write-Host '_NO_DEBUG_HEAP が設定されています (debug heap にならないので検査できません)' -ForegroundColor Red
    exit 3
}

$childArguments = @($ChildArgs -split '\|' | Where-Object { $_ -ne '' })
$gdbArguments = @('-batch', '-ex', 'set pagination off', '-ex', 'set confirm off', '-ex', 'run',
    '-ex', 'bt 40', '-ex', 'info registers', '--args', $Exe) + $childArguments
$output = & $Gdb @gdbArguments 2>&1 | ForEach-Object { "$_" }
$text = $output -join "`n"
if ($LogFile) { $text | Set-Content -LiteralPath $LogFile -Encoding utf8 }

$signal = [regex]::Match($text, 'received signal (\S+),')
$normal = $text -match '\[Inferior 1 \(process \d+\) exited normally\]'
$coded = [regex]::Match($text, '\[Inferior 1 \(process \d+\) exited with code (\d+)\]')

if ($signal.Success) {
    Write-Host "debugger の下で signal により停止: $($signal.Groups[1].Value)"
    # thread の出入りの行を除いて、止まった場所を出力する。
    $output | Where-Object { $_ -notmatch '^\[(New )?Thread ' } | Select-Object -Last 70 | ForEach-Object { Write-Host $_ }
    if ($ExpectSignal) { exit 0 }
    exit 1
}
if ($normal) {
    Write-Host 'debugger の下で正常に終了'
    if ($ExpectSignal) { Write-Host '対照: signal で止まるはずが、止まりませんでした' -ForegroundColor Red; exit 1 }
    exit 0
}
if ($coded.Success) {
    Write-Host "子が終了コード $([Convert]::ToInt32($coded.Groups[1].Value, 8)) で終了"
    $output | Where-Object { $_ -notmatch '^\[(New )?Thread ' } | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
    if ($ExpectSignal) { exit 1 }
    if ($SignalOnly) { Write-Host '終了コードは見ない (-SignalOnly)。signal による停止は無し'; exit 0 }
    exit 2
}
Write-Host 'gdb の出力から結果を判定できません' -ForegroundColor Red
$output | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
exit 3
