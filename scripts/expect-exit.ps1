<#
.SYNOPSIS
    コマンドを実行し、終了コードが期待値と厳密に一致することを検査する。

.DESCRIPTION
    CTest の WILL_FAIL は「0 以外なら合格」でしかない。
    使い方エラー (2) を期待しているのに、クラッシュ (0xC0000374) や
    DLL ロード失敗でも合格になってしまう。
    「失敗したこと」ではなく「意図した理由で失敗したこと」を検査する。

.PARAMETER ExpectExit
    期待する終了コード。

.PARAMETER Exe
    実行するファイル。

.PARAMETER ChildArgs
    子プロセスへ渡す引数。**';' 区切りの 1 つの文字列**で渡す。

    PowerShell は素の '--' を「あいまいなパラメータ名」として弾き、
    '--case' のような値もパラメータ名として束縛しようとする。
    区切り文字列で渡せばパラメータ束縛を一切通らない。

.EXAMPLE
    pwsh scripts/expect-exit.ps1 -ExpectExit 2 -Exe mvm_bench.exe `
        -ChildArgs "memory-probe;x.json;--case;D"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][int]$ExpectExit,
    [Parameter(Mandatory)][string]$Exe,
    [string]$ChildArgs = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path $Exe)) {
    Write-Host "実行ファイルがありません: $Exe" -ForegroundColor Red
    exit 1
}

$argv = @($ChildArgs -split ';' | Where-Object { $_ -ne '' })

$out = New-TemporaryFile
$errf = New-TemporaryFile
try {
    if ($argv.Count -gt 0) {
        $p = Start-Process -FilePath $Exe -NoNewWindow -Wait -PassThru -ArgumentList $argv `
            -RedirectStandardOutput $out -RedirectStandardError $errf
    } else {
        $p = Start-Process -FilePath $Exe -NoNewWindow -Wait -PassThru `
            -RedirectStandardOutput $out -RedirectStandardError $errf
    }
    $code = $p.ExitCode

    if ($code -eq $ExpectExit) {
        Write-Host "期待どおり exit $code で終了しました。"
        exit 0
    }

    Write-Host "終了コードが違います: 期待 $ExpectExit / 実際 $code" -ForegroundColor Red
    # 0x8 桁の負値はクラッシュであることが多い。取り違えを防ぐため明示する。
    if ($code -lt 0) {
        Write-Host ("実際の終了コードは 0x{0:X8} です。異常終了の可能性があります。" -f $code) `
            -ForegroundColor Red
    }
    Write-Host '--- stdout ---'
    Get-Content $out -TotalCount 40 -ErrorAction SilentlyContinue
    Write-Host '--- stderr ---'
    Get-Content $errf -TotalCount 40 -ErrorAction SilentlyContinue
    exit 1
} finally {
    Remove-Item $out, $errf -ErrorAction SilentlyContinue
}
