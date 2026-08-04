<#
.SYNOPSIS
    mvm Phase 0 の依存を MSYS2 UCRT64 へ導入し、正確な version を記録する。

.DESCRIPTION
    Phase 0 は MSYS2 UCRT64 (C:\msys64\ucrt64) でツールチェーンを統一する。
    Qt / MLT / FFmpeg / アプリ本体で CRT および C++ ABI を混在させない。

    この開発機には他プロジェクト用の Qt 6.8.3 (MSVC ビルド) が
    C:\Users\lambe\sdk\Qt\6.8.3 に存在するが、mvm はこれを一切参照しない。
    本スクリプトは既存 Qt の削除・変更・移動を行わない。

    MSYS2 は rolling repository であり、古い version はミラーから消える。
    そのため導入後に必ず version を docs/deps-lock.txt へ記録し、
    scripts/freeze-deps.ps1 でパッケージ実体を退避すること。

.PARAMETER FromFrozen
    third_party/pkgs/ に退避済みの .pkg.tar.zst から復元する (再現ビルド用)。

.PARAMETER SkipUpdate
    pacman -Syu を省略する。rolling 更新による構成変化を避けたい場合に使う。

.PARAMETER Msys2Root
    対象の MSYS2 ルート。復元検証では別ルートを指定する。
    既定の C:\msys64 は開発機の本番環境であり、検証で壊してはならない。

.PARAMETER LockFile
    version 記録の出力先。既定は docs/deps-lock.txt。
    復元検証では別ファイルを指定し、コミット済みの lock と照合する。

.PARAMETER NoWriteLock
    version 記録を書き出さない。docs/deps-lock.txt を汚さずに導入だけ行う。

.PARAMETER NoSignatureCheck
    pacman -U の署名検証を無効化する。凍結物に .sig が揃っていない場合の
    最後の手段であり、既定では使わない。使った場合は検証結果に必ず明記すること。

.EXAMPLE
    pwsh scripts/bootstrap-msys2.ps1
    pwsh scripts/bootstrap-msys2.ps1 -FromFrozen
#>
[CmdletBinding()]
param(
    [switch]$FromFrozen,
    [switch]$SkipUpdate,
    [string]$Msys2Root = 'C:\msys64',
    [string]$LockFile,
    [switch]$NoWriteLock,
    [switch]$NoSignatureCheck
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$Bash      = Join-Path $Msys2Root 'usr\bin\bash.exe'
$Ucrt64    = Join-Path $Msys2Root 'ucrt64'
$FrozenDir = Join-Path $RepoRoot 'third_party\pkgs'

if (-not $LockFile) {
    $LockFile = Join-Path $RepoRoot 'docs\deps-lock.txt'
}

# Phase 0 の依存パッケージ (mingw-w64-ucrt-x86_64- プレフィクスを補う)
# NOTE: 'toolchain' は group であり package ではない。group は pacman -Q で引けず
#       version を pin することもできないため、deps-lock.txt の目的に反する。
#       実パッケージを明示する (crt / headers / winpthreads は gcc の依存で入る)。
$Packages = @(
    # toolchain
    'gcc'
    'binutils'
    'gdb'
    'make'
    'cmake'
    'ninja'
    'pkgconf'

    # Qt (UCRT64 版。既存の MSVC 版 Qt 6.8.3 とは別物)
    'qt6-base'
    'qt6-declarative'    # QML
    'qt6-shadertools'    # qt6-declarative の実行時依存
    'qt6-multimedia'
    'qt6-svg'            # アイコン・ロゴ素材、MLT の Qt モジュール
    'qt6-5compat'        # MLT の Qt モジュールが依存しうる Qt5Compat

    # media
    'mlt'                # prebuilt MLT 7 (S1 の一次判定用)
    'ffmpeg'
    'SDL2'
    'frei0r-plugins'

    # MLT モジュールの実行時依存。
    #
    # 重要: MSYS2 の mlt パッケージはこれらを "suggested" (optional) 扱いにしており、
    # 既定では導入されない。しかしモジュール DLL 自体はこれらにリンクされた状態で
    # 同梱されるため、未導入だと該当モジュールが「存在するのに dlopen できない」
    # 状態になる。MLT はこれを stderr に出すだけで処理を継続するので、
    # 気づかないまま「filter が見つからない」という分かりにくい形で表面化する。
    #
    # 実際に影響を受けるもの:
    #   libmltplus       <- libebur128, libfftw3  ... affine (V7 transform/scale),
    #                                                 dynamictext (V3 文字レイヤ)
    #   libmltresample   <- libsamplerate         ... 音声リサンプル
    #   libmltrubberband <- librubberband         ... 音程保持のタイムストレッチ
    #   libmltsox        <- libsox                ... 音声フィルタ
    #   libmltrtaudio    <- librtaudio            ... 音声出力 (preview の代替経路)
    'libebur128'
    'fftw'
    'libsamplerate'
    'rubberband'
    'sox'
    'rtaudio'

    # test / 品質
    'gtest'
    'clang'   # clang-format (整形と lint に使う)
    'gcovr'   # カバレッジ集計
)

function Invoke-Msys2 {
    param([Parameter(Mandatory)][string]$Command)
    if (-not (Test-Path $Bash)) {
        throw "MSYS2 の bash が見つかりません: $Bash"
    }
    $env:MSYSTEM = 'UCRT64'
    $env:CHERE_INVOKING = '1'
    & $Bash -lc $Command
    if ($LASTEXITCODE -ne 0) {
        throw "MSYS2 コマンドが失敗しました (exit $LASTEXITCODE): $Command"
    }
}

# --- 事前確認 ---------------------------------------------------------------

Write-Host '=== mvm Phase 0 bootstrap (MSYS2 UCRT64) ===' -ForegroundColor Cyan

if (-not (Test-Path $Msys2Root)) {
    throw "MSYS2 が見つかりません: $Msys2Root`nhttps://www.msys2.org/ から導入してください。"
}

# 既存 Qt を誤って壊さないことの明示的な確認
$ExistingQt = 'C:\Users\lambe\sdk\Qt\6.8.3'
if (Test-Path $ExistingQt) {
    Write-Host "既存 Qt を検出: $ExistingQt (MSVC 版 / 他プロジェクト用)" -ForegroundColor DarkGray
    Write-Host '  -> 保持します。mvm はこれを参照しません。' -ForegroundColor DarkGray
}

# --- 導入 -------------------------------------------------------------------

$FullNames = $Packages | ForEach-Object { "mingw-w64-ucrt-x86_64-$_" }

if ($FromFrozen) {
    Write-Host "`n凍結パッケージから復元します: $FrozenDir" -ForegroundColor Yellow
    # @() で包む。Get-ChildItem は 0 件で $null、1 件でスカラーを返すため、
    # StrictMode 下では .Count が存在しないというエラーになる。
    $frozen = @(Get-ChildItem -Path $FrozenDir -Filter '*.pkg.tar.zst' -ErrorAction SilentlyContinue)
    if ($frozen.Count -eq 0) {
        throw "凍結パッケージがありません: $FrozenDir`nまず通常導入 + scripts/freeze-deps.ps1 を実行してください。"
    }
    $sigs = @(Get-ChildItem -Path $FrozenDir -Filter '*.pkg.tar.zst.sig' -ErrorAction SilentlyContinue)
    Write-Host "  パッケージ $($frozen.Count) 件 / 署名 $($sigs.Count) 件を検出"

    $unixDir = '/' + ($FrozenDir -replace '\\', '/' -replace '^([A-Za-z]):', '$1').ToLower()

    if ($NoSignatureCheck) {
        # pacman.conf を複製し SigLevel だけ Never に落とす。
        # --nodeps や --overwrite で誤魔化さないこと。それらは依存解決の失敗を
        # 隠してしまい、「復元できた」という判断を無意味にする。
        Write-Host '  !! 署名検証を無効化します (-NoSignatureCheck)' -ForegroundColor Yellow
        Invoke-Msys2 @"
sed -e 's/^SigLevel.*/SigLevel = Never/' -e 's/^LocalFileSigLevel.*/LocalFileSigLevel = Never/' /etc/pacman.conf > /tmp/pacman-nosig.conf
pacman --config /tmp/pacman-nosig.conf -U --noconfirm --needed $unixDir/*.pkg.tar.zst
"@
    } else {
        if ($sigs.Count -lt $frozen.Count) {
            Write-Host "  !! 署名が $($frozen.Count - $sigs.Count) 件不足しています。" -ForegroundColor Yellow
            Write-Host '     署名検証が失敗する場合は scripts/freeze-deps.ps1 -Force を実行するか、' -ForegroundColor Yellow
            Write-Host '     最後の手段として -NoSignatureCheck を使ってください (検証結果に明記すること)。' -ForegroundColor Yellow
        }
        Invoke-Msys2 "pacman -U --noconfirm --needed $unixDir/*.pkg.tar.zst"
    }
}
else {
    if (-not $SkipUpdate) {
        Write-Host "`n[1/2] pacman データベースを更新します..." -ForegroundColor Yellow
        # 初回は pacman 自体の更新でシェルが強制終了することがあるため 2 段階で実行する
        Invoke-Msys2 'pacman -Sy --noconfirm'
    }

    Write-Host "`n[2/2] パッケージを導入します ($($FullNames.Count) 件)..." -ForegroundColor Yellow
    $FullNames | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    Invoke-Msys2 "pacman -S --noconfirm --needed $($FullNames -join ' ')"
}

# --- version 記録 -----------------------------------------------------------
# rolling repo に対する唯一の防衛線。必ず commit すること。

if ($NoWriteLock) {
    Write-Host "`nversion 記録はスキップします (-NoWriteLock)" -ForegroundColor DarkGray
} else {
    Write-Host "`nversion を記録します: $LockFile" -ForegroundColor Yellow
}

$header = @(
    '# mvm Phase 0 - MSYS2 UCRT64 dependency lock'
    '#'
    '# MSYS2 は rolling repository であり、ここに記録した version は将来ミラーから'
    '# 消える。パッケージ実体は scripts/freeze-deps.ps1 で third_party/pkgs/ へ退避する。'
    '#'
    "# 生成日時 : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')"
    "# ホスト   : $env:COMPUTERNAME"
    "# MSYS2    : $Msys2Root"
    '#'
    '# 直接指定したパッケージ (依存で入ったものは後段の全量リストを参照)'
    '# ---------------------------------------------------------------------------'
)

$explicit = foreach ($n in $FullNames) {
    $line = & $Bash -lc "pacman -Q $n 2>/dev/null"
    if ($LASTEXITCODE -eq 0 -and $line) { $line } else { "!! MISSING: $n" }
}

$all = & $Bash -lc "pacman -Q | grep '^mingw-w64-ucrt-x86_64-'"

$content = $header + $explicit + @(
    ''
    '# UCRT64 環境の全インストール済みパッケージ'
    '# ---------------------------------------------------------------------------'
) + $all

if (-not $NoWriteLock) {
    $lockDir = Split-Path -Parent $LockFile
    if ($lockDir -and -not (Test-Path $lockDir)) {
        New-Item -ItemType Directory -Force -Path $lockDir | Out-Null
    }
    Set-Content -Path $LockFile -Value $content -Encoding UTF8
}

# --- 検証 -------------------------------------------------------------------

Write-Host "`n=== 検証 ===" -ForegroundColor Cyan

$missing = $explicit | Where-Object { $_ -like '!! MISSING*' }
if ($missing) {
    Write-Host '導入に失敗したパッケージがあります:' -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    throw '依存の導入が不完全です。'
}

$checks = @{
    'gcc'         = "$Ucrt64\bin\gcc.exe"
    'g++'         = "$Ucrt64\bin\g++.exe"
    'cmake'       = "$Ucrt64\bin\cmake.exe"
    'ninja'       = "$Ucrt64\bin\ninja.exe"
    'pkgconf'     = "$Ucrt64\bin\pkgconf.exe"
    'libmlt-7'    = "$Ucrt64\bin\libmlt-7.dll"
    'MLT headers' = "$Ucrt64\include\mlt-7\framework\mlt.h"
    # 注意: MSYS2 のレイアウトはモジュールが lib/mlt、データが share/mlt。
    #       上流ドキュメントにある lib/mlt-7 ではない。V11 の staging で必要。
    'MLT modules' = "$Ucrt64\lib\mlt"
    'MLT data'    = "$Ucrt64\share\mlt"
    'MLT qt6 mod' = "$Ucrt64\lib\mlt\libmltqt6.dll"
    'MLT profiles'= "$Ucrt64\share\mlt\profiles"
    'Qt6 cmake'   = "$Ucrt64\lib\cmake\Qt6"
    'Qt6Core dll' = "$Ucrt64\bin\Qt6Core.dll"
}

$failed = $false
foreach ($k in $checks.Keys | Sort-Object) {
    if (Test-Path $checks[$k]) {
        Write-Host ("  OK   {0,-12} {1}" -f $k, $checks[$k]) -ForegroundColor Green
    } else {
        Write-Host ("  FAIL {0,-12} {1}" -f $k, $checks[$k]) -ForegroundColor Red
        $failed = $true
    }
}

# MLT の pkg-config が引けるか (FindMLT.cmake が依存する)
$mltPc = & $Bash -lc "PKG_CONFIG_PATH=$($Ucrt64 -replace '\\','/')/lib/pkgconfig pkgconf --modversion mlt-framework-7 2>/dev/null"
if ($LASTEXITCODE -eq 0 -and $mltPc) {
    Write-Host "  OK   mlt-framework-7 pkg-config: $mltPc" -ForegroundColor Green
} else {
    Write-Host '  FAIL mlt-framework-7 の pkg-config が引けません' -ForegroundColor Red
    $failed = $true
}

if ($failed) { throw '検証に失敗しました。上記の FAIL を確認してください。' }

Write-Host "`n完了。次の手順:" -ForegroundColor Cyan
Write-Host '  1. pwsh scripts/freeze-deps.ps1        # パッケージ実体を退避'
Write-Host '  2. C:\msys64\ucrt64\bin\cmake.exe --preset ucrt64-release'
Write-Host '  3. C:\msys64\ucrt64\bin\cmake.exe --build --preset ucrt64-release'
Write-Host '  4. build/ucrt64-release/bin/mvm_mlt_hello.exe'
Write-Host "`n  docs/deps-lock.txt を commit してください。" -ForegroundColor Yellow
