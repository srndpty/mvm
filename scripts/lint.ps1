<#
.SYNOPSIS
    静的検査をまとめて実行する。

.DESCRIPTION
    以下を順に実行する。1 つでも失敗すれば exit 1。

      1. clang-format の差分検査
      2. アーキテクチャ検査 (MLT ヘッダの漏れ出し)
      3. PSScriptAnalyzer (固定バージョンとリポジトリ設定を使用)

    2 は Phase 0 の中核的な制約であり、失敗させる価値が最も高い。
    「MLT のヘッダを include してよいのは src/media/mlt/ だけ」という規約は、
    人間のレビューでは必ず漏れるため機械的に強制する。

.EXAMPLE
    pwsh scripts/lint.ps1
#>
[CmdletBinding()]
param(
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    # 走査対象を差し替える。lint 自身の negative test で使う。
    # 指定時は clang-format と PSScriptAnalyzer をスキップし、
    # ソース検査だけを行う。
    [string]$Path,

    # -Path で与えたフィクスチャを、どの層にあるものとして検査するか。
    # 層の判定はパスで行うため、リポジトリ外のフィクスチャでは
    # そのままでは検査が発火しない。negative test を書けるようにするための指定。
    [ValidateSet('', 'gpu_preview', 'preview_qt')]
    [string]$AsLayer = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$failed = @()

function Write-Section([string]$Name) {
    Write-Host "`n=== $Name ===" -ForegroundColor Cyan
}

# --- 1. 整形 ---------------------------------------------------------------
if (-not $Path) {
    Write-Section 'clang-format'
    & (Join-Path $PSScriptRoot 'format.ps1') -Check -Ucrt64 $Ucrt64
    if ($LASTEXITCODE -ne 0) { $failed += 'clang-format' }
}

# --- 2. アーキテクチャ検査 --------------------------------------------------
Write-Section 'アーキテクチャ検査'

# MLT のヘッダを include してよいのは src/media/mlt/ だけ。
# Mlt++ は使わない (C API のみ)。
$mltAllowed = Join-Path $RepoRoot 'src\media\mlt'
if ($Path) {
    $scanRoots = @($Path)
} else {
    $scanRoots = @((Join-Path $RepoRoot 'src'), (Join-Path $RepoRoot 'tests'))
}
# @() で包む。1 件だとスカラーになり StrictMode 下で .Count が無いというエラーになる。
$sources = @($scanRoots | ForEach-Object {
    if (Test-Path $_) {
        Get-ChildItem -Path $_ -Recurse -Include '*.c', '*.h', '*.cpp', '*.hpp' -File
    }
} | Where-Object {
    # build 成果物は常に除外。
    # lint 自身の negative test 用フィクスチャは、-Path で明示された時だけ見る。
    $_.FullName -notmatch '\\build\\' -and
    ($Path -or $_.FullName -notmatch '\\lint-negative\\')
})

$violations = @()
foreach ($f in $sources) {
    # -Path 指定時 (negative test) は MLT ヘッダ検査を行わない。
    # フィクスチャは producer 検査を試すために意図的に include するため。
    $isAllowed = $Path -or
                 $f.FullName.StartsWith($mltAllowed, [StringComparison]::OrdinalIgnoreCase)
    $text = Get-Content -Raw -LiteralPath $f.FullName
    $rel = if ($f.FullName.StartsWith($RepoRoot, [StringComparison]::OrdinalIgnoreCase)) {
        $f.FullName.Substring($RepoRoot.Length + 1)
    } else { $f.FullName }

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

# --- Phase 1: 層の隔離検査 ---------------------------------------------------
Write-Section '層の隔離検査 (Qt / QRhi)'

# 規約 (docs/phase1-plan.md §7, AGENTS.md):
#
#   src/media/gpu_preview/ : Qt を一切 include しない
#   src/app/preview/       : QRhi (Qt の private API) を include してよい唯一の場所
#
# QRhi は patch release 間でも互換保証が無い。隔離しても「壊れないこと」は
# 保証できないが、**壊れる範囲を 2 ファイルに限定する**ことはできる。
# 人間のレビューでは必ず漏れるので機械的に強制する。

$gpuPreviewDir = Join-Path $RepoRoot 'src\media\gpu_preview'
$previewQtDir  = Join-Path $RepoRoot 'src\app\preview'

# Qt のヘッダ: <QObject> / <QtCore/...> / <QtQuick/...> / "qquickrhiitem.h" など
$qtIncludePattern  = '#\s*include\s*[<"](Qt[A-Za-z0-9]*/|Q[A-Z][A-Za-z0-9]*>|q[a-z0-9_]+\.h[">])'
# QRhi は private API。パスが版番号を含む形と rhi/ 直下の両方に当たるようにする。
$qrhiIncludePattern = '#\s*include\s*[<"][^">]*rhi/q(rhi|shader)'

$layerViolations = @()
foreach ($f in $sources) {
    $rel = if ($f.FullName.StartsWith($RepoRoot, [StringComparison]::OrdinalIgnoreCase)) {
        $f.FullName.Substring($RepoRoot.Length + 1)
    } else { $f.FullName }

    if ($AsLayer) {
        $layer = $AsLayer
    } elseif ($f.FullName.StartsWith($gpuPreviewDir, [StringComparison]::OrdinalIgnoreCase)) {
        $layer = 'gpu_preview'
    } elseif ($f.FullName.StartsWith($previewQtDir, [StringComparison]::OrdinalIgnoreCase)) {
        $layer = 'preview_qt'
    } else {
        $layer = 'other'
    }

    $text = Get-Content -Raw -LiteralPath $f.FullName

    if ($layer -eq 'gpu_preview' -and $text -match $qtIncludePattern) {
        $layerViolations += "$rel : src/media/gpu_preview は Qt を include してはいけない"
    }
    if ($layer -ne 'preview_qt' -and $text -match $qrhiIncludePattern) {
        $layerViolations += "$rel : QRhi (Qt の private API) は src/app/preview/ でのみ使える"
    }
}

if ($layerViolations.Count -gt 0) {
    Write-Host "違反 $($layerViolations.Count) 件" -ForegroundColor Red
    $layerViolations | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    $failed += '層の隔離検査'
} else {
    Write-Host "OK ($($sources.Count) ファイル)" -ForegroundColor Green
}

# --- Phase 1.1: gpu_preview 層の禁止事項 -------------------------------------
Write-Section 'gpu_preview の禁止事項 (CPU readback / software fallback)'

# 規約 (docs/phase1-plan.md, P1.1 §9):
#
#   gpu_preview 層は decode 結果を CPU へ戻さない。
#   例外は marker 帯と color patch の **小領域 readback だけ**で、
#   実装は 1 箇所に限る (nv12_converter.cpp の readSmallRegionTopLeft)。
#
# 「動くけれど zero-copy ではない」経路は、絵が出てしまうので
# 人間のレビューでは見逃す。機械で塞ぐ。

$forbiddenAlways = @(
    @{ token = 'av_hwframe_transfer_data'; why = 'GPU frame を毎回 CPU へ転送する' }
    @{ token = 'sws_scale';                why = 'swscale による CPU 変換' }
    @{ token = 'sws_getContext';           why = 'swscale による CPU 変換' }
    @{ token = 'swscale.h';                why = 'swscale への依存' }
    @{ token = 'QImage';                   why = 'CPU 画像への copy' }
    @{ token = 'QPixmap';                  why = 'CPU 画像への copy' }
)

# readback を構成する token。**印のある file でのみ、各 1 箇所まで**許可する。
$readbackTokens = @('D3D11_USAGE_STAGING', 'D3D11_CPU_ACCESS_READ', 'D3D11_MAP_READ')
$allowMarker = 'MVM_ALLOW_SMALL_REGION_READBACK'

$gpuViolations = @()
foreach ($f in $sources) {
    $rel = if ($f.FullName.StartsWith($RepoRoot, [StringComparison]::OrdinalIgnoreCase)) {
        $f.FullName.Substring($RepoRoot.Length + 1)
    } else { $f.FullName }

    if ($AsLayer) {
        $isGpuLayer = ($AsLayer -eq 'gpu_preview')
    } else {
        $isGpuLayer = $f.FullName.StartsWith($gpuPreviewDir, [StringComparison]::OrdinalIgnoreCase)
    }
    if (-not $isGpuLayer) { continue }

    $text = Get-Content -Raw -LiteralPath $f.FullName

    foreach ($fb in $forbiddenAlways) {
        if ($text -match [regex]::Escape($fb.token)) {
            $gpuViolations += "$rel : '$($fb.token)' は禁止 ($($fb.why))"
        }
    }

    $allowed = $text -match [regex]::Escape($allowMarker)
    foreach ($t in $readbackTokens) {
        $n = ([regex]::Matches($text, [regex]::Escape($t))).Count
        if ($n -eq 0) { continue }
        if (-not $allowed) {
            $gpuViolations += "$rel : '$t' は CPU readback。許可された小領域実装だけが使える ($allowMarker の印が要る)"
        } elseif ($n -gt 1) {
            # 印のある file でも 2 本目の readback 経路は作らせない。
            $gpuViolations += "$rel : '$t' が $n 箇所ある。小領域 readback は 1 実装だけに限る"
        }
    }
}

if ($gpuViolations.Count -gt 0) {
    Write-Host "違反 $($gpuViolations.Count) 件" -ForegroundColor Red
    $gpuViolations | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    $failed += 'gpu_preview の禁止事項'
} else {
    Write-Host "OK" -ForegroundColor Green
}

# --- media producer は loader を使う -----------------------------------------
Write-Section 'producer service 検査 (loader 強制)'

# mlt_factory_producer に "avformat" を直接指定してはいけない。
# loader が付ける音声正規化 filter が失われ、AAC が壊れ volume filter が
# クラッシュする (docs/research/composition-notes.md 所見 I)。
#
# コメント中の文字列に反応しないよう、呼び出しの形そのものを検査する:
#   mlt_factory_producer( ... , "avformat" , ... )
# consumer 側 (mlt_factory_consumer) の "avformat" は正当なので対象外。
#
# 診断用に意図的な比較実験を行うファイルだけ除外する。
# 診断専用コードのみ除外する。
#   mvm_mlt_audiograph.c : loader と avformat を意図的に比較する実験コード
#   mvm_mlt_explore.c    : avformat producer の生 property を dump する S3 調査コード
#                          (loader にすると wrapper の property が出てしまい目的を果たさない)
#   mvm_mlt_probe.c      : V2 の目的が「avformat producer の解釈を ffprobe と
#                          比較すること」なので直接指定が正しい。loader だと
#                          静止画が qimage で開かれ meta.media.* が生成されず、
#                          メタデータ比較自体が成立しない。
#                          再生・レンダリング経路 (mvm_mlt_compose.c) は loader を使う。
$producerExempt = @('mvm_mlt_audiograph.c', 'mvm_mlt_explore.c', 'mvm_mlt_probe.c')

$producerPattern = 'mlt_factory_producer\s*\([^;]*?"avformat"'
$producerViolations = @()

foreach ($f in $sources) {
    if ($producerExempt -contains $f.Name) { continue }
    $rel = if ($f.FullName.StartsWith($RepoRoot, [StringComparison]::OrdinalIgnoreCase)) {
        $f.FullName.Substring($RepoRoot.Length + 1)
    } else { $f.FullName }

    # 行番号を出すため 1 行ずつ見る。複数行にまたがる呼び出しにも当たるよう
    # 前後を連結した窓でも検査する。
    $lines = Get-Content -LiteralPath $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $window = ($lines[$i..([Math]::Min($i + 2, $lines.Count - 1))] -join ' ')
        if ($window -match $producerPattern) {
            # 同じ呼び出しを複数回報告しないよう、開始行にのみ反応させる
            if ($lines[$i] -match 'mlt_factory_producer') {
                $producerViolations += "$rel : $($i + 1) : $($lines[$i].Trim())"
            }
        }
    }
}

if ($producerViolations.Count -gt 0) {
    Write-Host "違反 $($producerViolations.Count) 件" -ForegroundColor Red
    $producerViolations | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Host ''
    Write-Host '  media producer は mlt_factory_producer(profile, NULL, path) を使うこと。' -ForegroundColor Yellow
    Write-Host '  "avformat" を直接指定すると loader の音声正規化 filter が失われ、' -ForegroundColor Yellow
    Write-Host '  AAC が壊れ volume filter がクラッシュする。' -ForegroundColor Yellow
    Write-Host "  診断用の例外: $($producerExempt -join ', ')" -ForegroundColor Yellow
    $failed += 'producer service 検査'
} else {
    Write-Host "OK (除外: $($producerExempt -join ', '))" -ForegroundColor Green
}

# --- 3. PSScriptAnalyzer ----------------------------------------------------
Write-Section 'PSScriptAnalyzer'
if ($Path) {
    Write-Host '-Path 指定のためスキップ' -ForegroundColor DarkGray
} else {
    $versionFile = Join-Path $PSScriptRoot 'PSScriptAnalyzer.version'
    $requiredVersion = [version](Get-Content -Raw -LiteralPath $versionFile).Trim()
    $analyzer = Get-Module -ListAvailable -Name PSScriptAnalyzer |
        Where-Object { $_.Version -eq $requiredVersion } |
        Select-Object -First 1
    if (-not $analyzer) {
        Write-Host "必要な PSScriptAnalyzer $requiredVersion がありません。" -ForegroundColor Red
        Write-Host "導入: Install-Module PSScriptAnalyzer -RequiredVersion $requiredVersion -Scope CurrentUser" -ForegroundColor Yellow
        $failed += 'PSScriptAnalyzer'
    } else {
        $settings = Join-Path $PSScriptRoot 'PSScriptAnalyzerSettings.psd1'
        $analyzerRunner = Join-Path $PSScriptRoot 'run-script-analyzer.ps1'
        # 対話シェルに別バージョンの型情報が残っていても衝突しないよう、
        # Analyzer は毎回クリーンな子プロセスで実行する。
        $pwsh = (Get-Process -Id $PID).Path
        & $pwsh -NoProfile -NonInteractive -File $analyzerRunner `
            -AnalysisPath (Join-Path $RepoRoot 'scripts') `
            -SettingsPath $settings -RequiredVersion $requiredVersion
        if ($LASTEXITCODE -ne 0) {
            $failed += 'PSScriptAnalyzer'
        }
    }
}

# --- 結果 -------------------------------------------------------------------
Write-Host ''
if ($failed.Count -gt 0) {
    Write-Host "lint 失敗: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host 'lint 通過' -ForegroundColor Green
# 明示的に 0 を返す。これが無いと $LASTEXITCODE に前の値が残り、
# 呼び出し側 (CTest など) が誤判定する。
exit 0
