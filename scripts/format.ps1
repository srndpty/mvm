<#
.SYNOPSIS
    C/C++ ソースを clang-format で整形する。

.DESCRIPTION
    clang-format は scripts/clang-format.requirements.txt で版を固定したものだけを使う。
    MSYS2 の clang-format は rolling 更新で版が変わり、CI と整形結果が食い違うため使わない。

.PARAMETER Check
    整形せず、差分があるかだけを検査する (lint / CI 用)。
    差分があれば exit 1。

.EXAMPLE
    pwsh scripts/format.ps1
    pwsh scripts/format.ps1 -Check
#>
[CmdletBinding()]
param(
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot     = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'lib\clang-format.ps1')
$tool         = Get-MvmClangFormat -RepoRoot $RepoRoot
$ClangFormat  = $tool.Exe

if (-not (Test-Path -LiteralPath $ClangFormat)) {
    throw @"
固定版の clang-format $($tool.Version) がありません: $ClangFormat

    pwsh scripts/install-clang-format.ps1
"@
}
# 固定と違う版で整形・検査しない。版ごとに結果が違い、CI と食い違う。
$actualVersion = & $ClangFormat --version
if ("$actualVersion" -notmatch "clang-format version $([regex]::Escape($tool.Version))(\s|$)") {
    throw "clang-format の版が固定と違います: $actualVersion (固定: $($tool.Version))"
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

Write-Host "clang-format: $actualVersion"
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
