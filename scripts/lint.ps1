<#
.SYNOPSIS
    静的検査をまとめて実行する。

.DESCRIPTION
    以下を順に実行する。1 つでも失敗すれば exit 1。

      1. clang-format の差分検査
      2. アーキテクチャ検査 (MLT ヘッダの漏れ出し)
      3. PSScriptAnalyzer (導入されている場合のみ)

    2 は Phase 0 の中核的な制約であり、失敗させる価値が最も高い。
    「MLT のヘッダを include してよいのは src/media/mlt/ だけ」という規約は、
    人間のレビューでは必ず漏れるため機械的に強制する。

.EXAMPLE
    pwsh scripts/lint.ps1
#>
[CmdletBinding()]
param([string]$Ucrt64 = 'C:\msys64\ucrt64')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$failed = @()

function Write-Section([string]$Name) {
    Write-Host "`n=== $Name ===" -ForegroundColor Cyan
}

# --- 1. 整形 ---------------------------------------------------------------
Write-Section 'clang-format'
& (Join-Path $PSScriptRoot 'format.ps1') -Check -Ucrt64 $Ucrt64
if ($LASTEXITCODE -ne 0) { $failed += 'clang-format' }

# --- 2. アーキテクチャ検査 --------------------------------------------------
Write-Section 'アーキテクチャ検査'

# MLT のヘッダを include してよいのは src/media/mlt/ だけ。
# Mlt++ は使わない (C API のみ)。
$mltAllowed = Join-Path $RepoRoot 'src\media\mlt'
$sources = @('src', 'tests') | ForEach-Object {
    $dir = Join-Path $RepoRoot $_
    if (Test-Path $dir) {
        Get-ChildItem -Path $dir -Recurse -Include '*.c', '*.h', '*.cpp', '*.hpp' -File
    }
} | Where-Object { $_.FullName -notmatch '\\build\\' }

$violations = @()
foreach ($f in $sources) {
    $isAllowed = $f.FullName.StartsWith($mltAllowed, [StringComparison]::OrdinalIgnoreCase)
    $text = Get-Content -Raw -LiteralPath $f.FullName
    $rel = $f.FullName.Substring($RepoRoot.Length + 1)

    if (-not $isAllowed) {
        if ($text -match '#\s*include\s*<(framework|mlt\+\+)/') {
            $violations += "$rel : MLT のヘッダを include している (許可: src\media\mlt\ のみ)"
        }
    }
    # Mlt++ はどこでも禁止
    if ($text -match '#\s*include\s*<mlt\+\+/') {
        $violations += "$rel : Mlt++ を include している (C API のみを使う方針)"
    }
    if ($text -match '\bMlt::') {
        $violations += "$rel : Mlt:: を使っている (C API のみを使う方針)"
    }
}

if ($violations.Count -gt 0) {
    Write-Host "違反 $($violations.Count) 件" -ForegroundColor Red
    $violations | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    $failed += 'アーキテクチャ検査'
} else {
    Write-Host "OK ($($sources.Count) ファイル)" -ForegroundColor Green
}

# --- 3. PSScriptAnalyzer ----------------------------------------------------
Write-Section 'PSScriptAnalyzer'
if (Get-Module -ListAvailable PSScriptAnalyzer) {
    Import-Module PSScriptAnalyzer
    $issues = Invoke-ScriptAnalyzer -Path (Join-Path $RepoRoot 'scripts') -Severity Error, Warning
    if ($issues) {
        $issues | ForEach-Object {
            Write-Host ("  {0}:{1} [{2}] {3}" -f (Split-Path $_.ScriptPath -Leaf), $_.Line,
                                                 $_.RuleName, $_.Message) -ForegroundColor Red
        }
        $failed += 'PSScriptAnalyzer'
    } else {
        Write-Host 'OK' -ForegroundColor Green
    }
} else {
    # 開発中なのでフォールバックは最小。導入方法だけ示して先へ進む。
    Write-Host '未導入のためスキップ: Install-Module PSScriptAnalyzer -Scope CurrentUser' -ForegroundColor Yellow
}

# --- 結果 -------------------------------------------------------------------
Write-Host ''
if ($failed.Count -gt 0) {
    Write-Host "lint 失敗: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host 'lint 通過' -ForegroundColor Green
