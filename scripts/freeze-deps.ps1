<#
.SYNOPSIS
    導入済み MSYS2 UCRT64 パッケージの実体を third_party/pkgs/ へ退避する。

.DESCRIPTION
    MSYS2 は rolling repository であり、古い version はミラーから消える。
    docs/deps-lock.txt に version を記録しても、その version の .pkg.tar.zst が
    入手できなくなれば再現ビルドは不可能になる。

    本スクリプトは pacman のキャッシュ (var/cache/pacman/pkg) から、
    現在インストールされている version の実体だけを選んで退避する。

    退避先は git 管理外 (数百 MB〜GB 規模のため)。開発機のバックアップ対象として
    別途保全すること。git に入るのは docs/deps-lock.txt のみ。

.EXAMPLE
    pwsh scripts/freeze-deps.ps1
#>
[CmdletBinding()]
param(
    [string]$Msys2Root = 'C:\msys64',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot  = Split-Path -Parent $PSScriptRoot
$Bash      = Join-Path $Msys2Root 'usr\bin\bash.exe'
$CacheDir  = Join-Path $Msys2Root 'var\cache\pacman\pkg'
$FrozenDir = Join-Path $RepoRoot 'third_party\pkgs'

Write-Host '=== mvm Phase 0 : dependency freeze ===' -ForegroundColor Cyan

if (-not (Test-Path $CacheDir)) { throw "pacman キャッシュが見つかりません: $CacheDir" }
New-Item -ItemType Directory -Force -Path $FrozenDir | Out-Null

# インストール済みの UCRT64 パッケージ一覧 ("name version" 形式)
$env:MSYSTEM = 'UCRT64'
$installed = & $Bash -lc "pacman -Q | grep '^mingw-w64-ucrt-x86_64-'"
if ($LASTEXITCODE -ne 0 -or -not $installed) {
    throw 'インストール済みパッケージを取得できませんでした。'
}

Write-Host "対象: $($installed.Count) パッケージ"

$copied    = 0
$sigCopied = 0
$missing   = @()
$noSig     = @()

foreach ($line in $installed) {
    $parts = $line -split '\s+'
    if ($parts.Count -lt 2) { continue }
    $name, $version = $parts[0], $parts[1]

    # pacman キャッシュのファイル名は <name>-<version>-<arch>.pkg.tar.zst
    $pattern = "$name-$version-*.pkg.tar.zst"
    $file = Get-ChildItem -Path $CacheDir -Filter $pattern -ErrorAction SilentlyContinue |
            Select-Object -First 1

    if (-not $file) {
        $missing += "$name $version"
        continue
    }

    $dest = Join-Path $FrozenDir $file.Name
    if (-not (Test-Path $dest) -or $Force) {
        Copy-Item -Path $file.FullName -Destination $dest -Force
        $copied++
    }

    # 署名も一緒に退避する。
    # 署名が無いと復元時に SigLevel=Never へ落とさざるを得ず、
    # 「復元できた」ことの意味が弱くなる (改竄・破損を検出できない)。
    $sig = "$($file.FullName).sig"
    if (Test-Path $sig) {
        $sigDest = "$dest.sig"
        if (-not (Test-Path $sigDest) -or $Force) {
            Copy-Item -Path $sig -Destination $sigDest -Force
            $sigCopied++
        }
    } else {
        $noSig += $file.Name
    }
}

$total = (Get-ChildItem $FrozenDir -Filter '*.pkg.tar.zst' -ErrorAction SilentlyContinue |
          Measure-Object -Property Length -Sum).Sum

Write-Host "`n退避しました: $copied 件 (新規パッケージ), $sigCopied 件 (新規署名)" -ForegroundColor Green
Write-Host ("退避先の合計サイズ: {0:N1} MB" -f ($total / 1MB))

if ($noSig) {
    Write-Host "`n!! 署名が見つからないパッケージ: $($noSig.Count) 件" -ForegroundColor Yellow
    Write-Host '   復元時にこれらは署名検証できません。' -ForegroundColor Yellow
    $noSig | Select-Object -First 10 | ForEach-Object { Write-Host "   $_" -ForegroundColor Yellow }
}

if ($missing) {
    Write-Host "`n!! キャッシュに実体が無いパッケージ: $($missing.Count) 件" -ForegroundColor Yellow
    $missing | Select-Object -First 20 | ForEach-Object { Write-Host "   $_" -ForegroundColor Yellow }
    Write-Host @'

pacman がキャッシュを掃除した (paccache / Clean-up) 可能性があります。
再現性を完全にするには、以下でキャッシュへ再取得してから再実行してください:

    C:\msys64\usr\bin\bash.exe -lc "pacman -Sw --noconfirm --needed <package>..."

なお -Sw は既にインストール済みのパッケージについてはダウンロードのみを行い、
システムを変更しません。
'@ -ForegroundColor Yellow
}

Write-Host "`n完了。docs/deps-lock.txt を commit してください。" -ForegroundColor Cyan
