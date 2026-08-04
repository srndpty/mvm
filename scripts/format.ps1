<#
.SYNOPSIS
    C/C++ ソースを clang-format で整形する。

.PARAMETER Check
    整形せず、差分があるかだけを検査する (lint / CI 用)。
    差分があれば exit 1。

.EXAMPLE
    pwsh scripts/format.ps1
    pwsh scripts/format.ps1 -Check
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [string]$Ucrt64 = 'C:\msys64\ucrt64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot     = Split-Path -Parent $PSScriptRoot
$ClangFormat  = Join-Path $Ucrt64 'bin\clang-format.exe'

if (-not (Test-Path $ClangFormat)) {
    throw @"
clang-format が見つかりません: $ClangFormat

    pwsh scripts/bootstrap-msys2.ps1
"@
}

# 対象は自分たちが書いたコードだけ。build と third_party は含めない。
$targets = @('src', 'tests') | ForEach-Object {
    $dir = Join-Path $RepoRoot $_
    if (Test-Path $dir) {
        Get-ChildItem -Path $dir -Recurse -Include '*.c', '*.h', '*.cpp', '*.hpp' -File
    }
} | Where-Object { $_.FullName -notmatch '\\build\\' }

if (-not $targets) {
    Write-Host '対象ファイルがありません。'
    exit 0
}

Write-Host "clang-format: $((& $ClangFormat --version))"
Write-Host "対象: $($targets.Count) ファイル"

if ($Check) {
    # clang-format の出力を PowerShell の文字列として受け取って比較しない。
    # stdout は現在のコンソール符号化で復号されるため、日本語コメントを含む
    # ファイルが全件「差分あり」になる。実際に起きた
    # (23 ファイル中 23 ファイルが未整形と誤検出された)。
    # 終了コードだけで判定すればテキストの往復が発生しない。
    $bad = @()
    foreach ($f in $targets) {
        & $ClangFormat --style=file --dry-run -Werror $f.FullName 2>$null
        if ($LASTEXITCODE -ne 0) {
            $bad += $f.FullName.Substring($RepoRoot.Length + 1)
        }
    }
    if ($bad.Count -gt 0) {
        Write-Host "`n整形されていないファイル: $($bad.Count) 件" -ForegroundColor Red
        $bad | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
        Write-Host "`n  pwsh scripts/format.ps1" -ForegroundColor Yellow
        exit 1
    }
    Write-Host '整形済みです。' -ForegroundColor Green
    exit 0
}

& $ClangFormat --style=file -i @($targets.FullName)
Write-Host '整形しました。' -ForegroundColor Green
