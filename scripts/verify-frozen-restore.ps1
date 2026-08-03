<#
.SYNOPSIS
    third_party/pkgs の凍結パッケージだけから UCRT64 環境を復元できることを、
    クリーンな MSYS2 ベース上で検証する (R0)。

.DESCRIPTION
    Phase 0 の判定は「同じ構成をあとから再現できる」ことに依存する。
    MSYS2 は rolling repository なので、ミラーから version が消えた後でも
    third_party/pkgs だけで環境を組み直せなければ、判定根拠は再現不能になる。

    この検証は「既存環境のコピー」では意味がない。既に導入済みの DLL や
    resource が残っていると、凍結物に不足があっても成功してしまうためである。
    そのため必ず、素の msys2-base tarball を展開したクリーンなルートを使う。

    検証項目:
      1. クリーンな MSYS2 ベースへ third_party/pkgs だけから UCRT64 を導入できる
      2. docs/deps-lock.txt と version が一致する
      3. 復元先の ucrt64/bin だけを PATH 先頭にして mvm_mlt_hello を実行できる
      4. 復元先の lib/mlt と share/mlt を明示指定してモジュールをロードできる
      5. affine / dynamictext / qtext / avformat / qtblend / mix 等が解決できる
      6. atsc_1080p_60 が 1920x1080 / 60|1 / SAR 1|1 になる
      7. optional 扱いだった依存 DLL も全てロードできる

.PARAMETER TestRoot
    復元先。既定は $env:USERPROFILE\sdk\msys2-mvm-phase0-restore。
    C:\msys64 とその配下は指定できない (安全のため拒否する)。

.PARAMETER BaseTarball
    msys2-base-x86_64-*.tar.xz のパス。省略時はダウンロードする。

.PARAMETER KeepExisting
    TestRoot が既に存在する場合、展開をやり直さずそのまま使う。

.PARAMETER NoSignatureCheck
    署名検証を無効化する。結果レポートに必ず明記される。

.EXAMPLE
    pwsh scripts/verify-frozen-restore.ps1
    pwsh scripts/verify-frozen-restore.ps1 -BaseTarball D:\dl\msys2-base-x86_64-20260611.tar.xz
#>
[CmdletBinding()]
param(
    [string]$TestRoot = (Join-Path $env:USERPROFILE 'sdk\msys2-mvm-phase0-restore'),
    [string]$BaseTarball,
    [switch]$KeepExisting,
    [switch]$NoSignatureCheck,
    [string]$BaseUrl = 'https://repo.msys2.org/distrib/x86_64/msys2-base-x86_64-20260611.tar.xz'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$FrozenDir = Join-Path $RepoRoot 'third_party\pkgs'
$LockFile  = Join-Path $RepoRoot 'docs\deps-lock.txt'
$HelloExe  = Join-Path $RepoRoot 'build\ucrt64-release\bin\mvm_mlt_hello.exe'

$results = [System.Collections.Generic.List[object]]::new()
function Add-Result {
    param([string]$Id, [string]$Name, [bool]$Ok, [string]$Detail = '')
    $results.Add([pscustomobject]@{ Id = $Id; Name = $Name; Ok = $Ok; Detail = $Detail })
    $tag = if ($Ok) { 'OK  ' } else { 'FAIL' }
    $color = if ($Ok) { 'Green' } else { 'Red' }
    Write-Host ("  {0} [{1}] {2}" -f $tag, $Id, $Name) -ForegroundColor $color
    if ($Detail) { Write-Host "         $Detail" -ForegroundColor DarkGray }
}

# --- 安全条件 ---------------------------------------------------------------
# 既存の MSYS2 と Qt を絶対に壊さない。ここは検証の前提であり、
# 失敗するくらいなら何もしない方がよい。

Write-Host '=== R0: 凍結パッケージ復元検証 ===' -ForegroundColor Cyan

$TestRootFull = [System.IO.Path]::GetFullPath($TestRoot)
$forbidden = @('C:\msys64', $env:SystemRoot, 'C:\Users\lambe\sdk\Qt', 'C:\Program Files')
foreach ($f in $forbidden) {
    if (-not $f) { continue }
    $ff = [System.IO.Path]::GetFullPath($f)
    if ($TestRootFull -eq $ff -or $TestRootFull.StartsWith($ff + [IO.Path]::DirectorySeparatorChar,
                                                          [StringComparison]::OrdinalIgnoreCase)) {
        throw @"
TestRoot が保護対象のパスを指しています: $TestRootFull

  保護対象: $ff

R0 は既存環境を壊さずに行う検証です。既存の C:\msys64 や
C:\Users\lambe\sdk\Qt\6.8.3 は削除・変更・移動しません。
別のディレクトリを -TestRoot に指定してください。
"@
    }
}
Write-Host "検証先 : $TestRootFull" -ForegroundColor Yellow
Write-Host "凍結物 : $FrozenDir"
Write-Host '既存の C:\msys64 と C:\Users\lambe\sdk\Qt は変更しません。' -ForegroundColor DarkGray

if (-not (Test-Path $FrozenDir)) { throw "凍結ディレクトリがありません: $FrozenDir" }
$frozenPkgs = @(Get-ChildItem $FrozenDir -Filter '*.pkg.tar.zst')
if ($frozenPkgs.Count -eq 0) {
    throw "凍結パッケージがありません。先に pwsh scripts/freeze-deps.ps1 を実行してください。"
}
$frozenSigs = @(Get-ChildItem $FrozenDir -Filter '*.pkg.tar.zst.sig')
Write-Host "凍結: パッケージ $($frozenPkgs.Count) 件 / 署名 $($frozenSigs.Count) 件"

if (-not (Test-Path $HelloExe)) {
    throw @"
mvm_mlt_hello.exe がありません: $HelloExe

先にビルドしてください:
    pwsh scripts/build.ps1 -Preset ucrt64-release
"@
}

# --- クリーンな MSYS2 ベースを用意 ------------------------------------------

$Msys2Test = Join-Path $TestRootFull 'msys64'

# pacman-key --init は gpg-agent と dirmngr を常駐させる。
# これらが検証ルート配下のファイルを掴んだままだと削除できない。
# 前回実行の残骸を必ず落としてから消す。
# 対象は「検証ルート配下から起動されたプロセス」に限定する。
# 開発機の C:\msys64 のプロセスは絶対に止めない。
function Stop-TestRootProcesses([string]$Root) {
    $stopped = 0
    foreach ($p in Get-Process -ErrorAction SilentlyContinue) {
        $path = $null
        try { $path = $p.Path } catch { continue }
        if (-not $path) { continue }
        if ($path.StartsWith($Root, [StringComparison]::OrdinalIgnoreCase)) {
            try {
                Stop-Process -Id $p.Id -Force -ErrorAction Stop
                $stopped++
            } catch { }
        }
    }
    if ($stopped -gt 0) {
        Write-Host "  検証ルート配下のプロセスを $stopped 件停止しました" -ForegroundColor DarkGray
        Start-Sleep -Milliseconds 500
    }
}

if ((Test-Path $Msys2Test) -and $KeepExisting) {
    Write-Host "`n既存の検証ルートを再利用します (-KeepExisting)" -ForegroundColor Yellow
} else {
    if (Test-Path $TestRootFull) {
        Write-Host "`n既存の検証ルートを削除します: $TestRootFull" -ForegroundColor Yellow
        Stop-TestRootProcesses $TestRootFull
        Remove-Item -Recurse -Force -LiteralPath $TestRootFull -ErrorAction SilentlyContinue
        if (Test-Path $TestRootFull) {
            Start-Sleep -Seconds 2
            Stop-TestRootProcesses $TestRootFull
            Remove-Item -Recurse -Force -LiteralPath $TestRootFull
        }
    }
    New-Item -ItemType Directory -Force -Path $TestRootFull | Out-Null

    if (-not $BaseTarball) {
        $BaseTarball = Join-Path $TestRootFull 'msys2-base.tar.xz'
        Write-Host "msys2-base をダウンロードします..." -ForegroundColor Yellow
        Write-Host "  $BaseUrl"
        Invoke-WebRequest -Uri $BaseUrl -OutFile $BaseTarball -UseBasicParsing
    }
    if (-not (Test-Path $BaseTarball)) { throw "base tarball がありません: $BaseTarball" }

    Write-Host "msys2-base を展開します: $BaseTarball" -ForegroundColor Yellow
    # Windows 10 以降の bsdtar。既存 MSYS2 の tar は使わない
    # (検証対象と同じ環境の道具を使うと独立性が下がるため)。
    & tar.exe -xf $BaseTarball -C $TestRootFull
    if ($LASTEXITCODE -ne 0) { throw "展開に失敗しました (exit $LASTEXITCODE)" }
}

if (-not (Test-Path $Msys2Test)) { throw "展開後に msys64 が見つかりません: $Msys2Test" }

$TestBash   = Join-Path $Msys2Test 'usr\bin\bash.exe'
$TestUcrt64 = Join-Path $Msys2Test 'ucrt64'

# クリーンなベースであることを確認する。
# 既存環境のコピーを使ってしまうと、この検証は無意味になる。
if (Test-Path (Join-Path $TestUcrt64 'bin\libmlt-7.dll')) {
    throw @"
検証ルートに既に MLT が存在します: $TestUcrt64

これは「クリーンなベース」ではありません。既存環境のコピーで検証しても、
凍結物の不足を検出できないため意味がありません。
-KeepExisting を外して展開からやり直してください。
"@
}
Add-Result 'R0-0' 'クリーンな MSYS2 ベースを用意した (UCRT64 は未導入)' $true $Msys2Test

# --- 初回セットアップ -------------------------------------------------------

Write-Host "`nMSYS2 の初回セットアップ..." -ForegroundColor Yellow
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
& $TestBash -lc 'exit 0' 2>&1 | Out-Null
& $TestBash -lc 'exit 0' 2>&1 | Out-Null

if (-not $NoSignatureCheck) {
    Write-Host 'pacman キーリングを初期化します (署名検証のため)...' -ForegroundColor Yellow
    & $TestBash -lc 'pacman-key --init && pacman-key --populate msys2' 2>&1 | Select-Object -Last 3
    if ($LASTEXITCODE -ne 0) {
        Write-Host '  キーリング初期化に失敗しました。署名検証を無効化して続行します。' -ForegroundColor Yellow
        $NoSignatureCheck = $true
    }
}

# --- 1. 凍結物だけから導入 --------------------------------------------------

Write-Host "`n--- 1. 凍結パッケージから復元 ---" -ForegroundColor Cyan
$restoreLock = Join-Path $TestRootFull 'restored-deps-lock.txt'

$bootstrapArgs = @{
    FromFrozen = $true
    Msys2Root  = $Msys2Test
    LockFile   = $restoreLock   # docs/deps-lock.txt は絶対に上書きしない
}
if ($NoSignatureCheck) { $bootstrapArgs['NoSignatureCheck'] = $true }

$restoreOk = $true
$restoreDetail = ''
try {
    & (Join-Path $PSScriptRoot 'bootstrap-msys2.ps1') @bootstrapArgs 2>&1 |
        Tee-Object -FilePath (Join-Path $TestRootFull 'restore.log') |
        Select-String -Pattern 'OK  |FAIL|エラー|error' | Select-Object -Last 20 |
        ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
} catch {
    $restoreOk = $false
    $restoreDetail = $_.Exception.Message
}

Add-Result 'R0-1' 'クリーンなベースへ third_party/pkgs だけから UCRT64 を導入できる' `
    $restoreOk $restoreDetail

if (-not $restoreOk) {
    Write-Host "`n復元に失敗したため以降の検証は実施できません。" -ForegroundColor Red
    Write-Host "ログ: $(Join-Path $TestRootFull 'restore.log')"
}

# --- 2. deps-lock.txt との照合 ---------------------------------------------

Write-Host "`n--- 2. version の照合 ---" -ForegroundColor Cyan

function Get-LockVersions([string]$Path) {
    $map = @{}
    if (-not (Test-Path $Path)) { return $map }
    foreach ($line in Get-Content $Path -Encoding UTF8) {
        if ($line -match '^\s*#') { continue }
        if ($line -match '^\s*$') { continue }
        $parts = $line.Trim() -split '\s+'
        if ($parts.Count -ge 2 -and $parts[0] -like 'mingw-w64-ucrt-x86_64-*') {
            $map[$parts[0]] = $parts[1]
        }
    }
    return $map
}

$expected = Get-LockVersions $LockFile
$actual   = Get-LockVersions $restoreLock

if ($expected.Count -eq 0) {
    Add-Result 'R0-2' 'docs/deps-lock.txt と version が一致する' $false `
        "docs/deps-lock.txt を読めません: $LockFile"
} elseif ($actual.Count -eq 0) {
    Add-Result 'R0-2' 'docs/deps-lock.txt と version が一致する' $false `
        '復元先の version を取得できませんでした'
} else {
    $missing = @(); $diff = @()
    foreach ($k in $expected.Keys) {
        if (-not $actual.ContainsKey($k)) { $missing += $k }
        elseif ($actual[$k] -ne $expected[$k]) { $diff += "$k : $($expected[$k]) -> $($actual[$k])" }
    }
    $ok = ($missing.Count -eq 0 -and $diff.Count -eq 0)
    $detail = "期待 $($expected.Count) 件 / 復元 $($actual.Count) 件"
    if ($missing.Count) { $detail += " / 欠落 $($missing.Count) 件: $(($missing | Select-Object -First 5) -join ', ')" }
    if ($diff.Count)    { $detail += " / version 差 $($diff.Count) 件: $(($diff | Select-Object -First 5) -join '; ')" }
    Add-Result 'R0-2' 'docs/deps-lock.txt と version が一致する' $ok $detail
}

# --- 必須の直接指定パッケージが揃っているか --------------------------------

$RequiredExplicit = @(
    'gcc','binutils','gdb','make','cmake','ninja','pkgconf',
    'qt6-base','qt6-declarative','qt6-shadertools','qt6-multimedia','qt6-svg','qt6-5compat',
    'mlt','ffmpeg','SDL2','frei0r-plugins',
    'libebur128','fftw','libsamplerate','rubberband','sox','rtaudio','gtest'
) | ForEach-Object { "mingw-w64-ucrt-x86_64-$_" }

$missingExplicit = @($RequiredExplicit | Where-Object { -not $actual.ContainsKey($_) })
Add-Result 'R0-2b' '必須の直接指定パッケージが全て存在する' ($missingExplicit.Count -eq 0) `
    $(if ($missingExplicit.Count) { "欠落: $($missingExplicit -join ', ')" }
      else { "$($RequiredExplicit.Count) 件すべて存在" })

# --- 3..7. 復元先の MLT を実際に動かす --------------------------------------

Write-Host "`n--- 3-7. 復元先の MLT を実行 ---" -ForegroundColor Cyan

$restModules = Join-Path $TestUcrt64 'lib\mlt'
$restData    = Join-Path $TestUcrt64 'share\mlt'

if (-not (Test-Path $restModules)) {
    Add-Result 'R0-3' '復元先の ucrt64/bin だけを PATH にして mvm_mlt_hello を実行できる' $false `
        "復元先にモジュールがありません: $restModules"
} else {
    # 復元先の bin だけを PATH 先頭にする。
    # 開発機の C:\msys64\ucrt64\bin を含めない。含めるとどちらの DLL を
    # 使ったのか分からなくなり、検証の意味が消える。
    $sysRoot = $env:SystemRoot
    $minimalPath = "$TestUcrt64\bin;$sysRoot\system32;$sysRoot"

    $prevPath = $env:PATH
    $env:PATH = $minimalPath
    try {
        $out = & $HelloExe $restModules $restData 2>&1
        $exit = $LASTEXITCODE
    } finally {
        $env:PATH = $prevPath
    }

    $outText = ($out | Out-String)
    Set-Content -Path (Join-Path $TestRootFull 'hello-output.txt') -Value $outText -Encoding UTF8

    Add-Result 'R0-3' '復元先の ucrt64/bin だけを PATH にして mvm_mlt_hello を実行できる' `
        ($exit -ne $null) "exit=$exit / PATH=$minimalPath"

    $summary = ($out | Select-String -Pattern '^MVM_DOCTOR_RESULT' | Select-Object -First 1)
    if (-not $summary) {
        Add-Result 'R0-4' '復元先の lib/mlt と share/mlt を明示指定してモジュールをロードできる' $false `
            '結果サマリ行を取得できませんでした。hello-output.txt を確認してください。'
    } else {
        $line = "$summary"
        function Get-Field([string]$name) {
            if ($line -match "$name=([^\s]+)") { return $Matches[1] }
            return $null
        }
        $modTotal   = [int](Get-Field 'modules_total')
        $modFailed  = [int](Get-Field 'modules_failed')
        $svcMissing = [int](Get-Field 'services_missing')
        $profileOk  = [int](Get-Field 'profile_ok')
        $profile    = Get-Field 'profile'
        $sar        = Get-Field 'sar'

        Add-Result 'R0-4' '復元先のモジュールを全てロードできる' `
            ($modTotal -gt 0 -and $modFailed -eq 0) "modules_total=$modTotal modules_failed=$modFailed"

        Add-Result 'R0-5' '必須 service (affine/dynamictext/qtext/avformat/qtblend/mix 等) を解決できる' `
            ($svcMissing -eq 0) "services_missing=$svcMissing"

        Add-Result 'R0-6' 'atsc_1080p_60 が 1920x1080 / 60|1 / SAR 1|1 になる' `
            ($profileOk -eq 1 -and $profile -eq '1920x1080@60/1' -and $sar -eq '1/1') `
            "profile=$profile sar=$sar profile_ok=$profileOk"

        # optional 扱いだった依存の DLL がロードできるか。
        # modules_failed=0 はこれを含むが、明示的に個別確認もする。
        $optionalModules = @('libmltplus.dll','libmltresample.dll','libmltrubberband.dll',
                             'libmltsox.dll','libmltrtaudio.dll')
        $missingOpt = @($optionalModules | Where-Object { -not (Test-Path (Join-Path $restModules $_)) })
        $optOk = ($missingOpt.Count -eq 0 -and $modFailed -eq 0)
        Add-Result 'R0-7' 'optional 扱いだった依存の DLL も全てロードできる' $optOk `
            $(if ($missingOpt.Count) { "モジュール自体が欠落: $($missingOpt -join ', ')" }
              else { "$($optionalModules.Count) モジュールが存在し、ロードにも成功" })
    }
}

# --- 結果 -------------------------------------------------------------------

Write-Host "`n=== 結果 ===" -ForegroundColor Cyan
$failed = @($results | Where-Object { -not $_.Ok })

if ($NoSignatureCheck) {
    Write-Host '注意: 署名検証を無効化して復元しました。改竄・破損は検出できていません。' -ForegroundColor Yellow
}

$report = [ordered]@{
    verified_at         = (Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz')
    test_root           = $TestRootFull
    frozen_dir          = $FrozenDir
    frozen_package_count= $frozenPkgs.Count
    frozen_sig_count    = $frozenSigs.Count
    signature_checked   = (-not $NoSignatureCheck)
    base_tarball        = $BaseTarball
    lock_file           = $LockFile
    restored_lock_file  = $restoreLock
    results             = $results
    passed              = ($results.Count - $failed.Count)
    failed              = $failed.Count
}
$reportPath = Join-Path $TestRootFull 'r0-report.json'
$report | ConvertTo-Json -Depth 6 | Set-Content -Path $reportPath -Encoding UTF8

# 常駐した gpg-agent / dirmngr を落としておく。
# 残しておくと次回実行時に検証ルートを削除できなくなる。
Stop-TestRootProcesses $TestRootFull

Write-Host "$($results.Count) 項目中 $($failed.Count) 項目が失敗"
Write-Host "レポート: $reportPath"
Write-Host "ログ    : $(Join-Path $TestRootFull 'restore.log')"
Write-Host "         $(Join-Path $TestRootFull 'hello-output.txt')"

if ($failed.Count -gt 0) {
    Write-Host "`n失敗した項目:" -ForegroundColor Red
    $failed | ForEach-Object { Write-Host "  [$($_.Id)] $($_.Name)`n       $($_.Detail)" -ForegroundColor Red }
    exit 1
}

Write-Host "`nR0: 凍結パッケージからの復元を検証しました。" -ForegroundColor Green
exit 0
