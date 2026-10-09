<#
.SYNOPSIS
    docs/deps-lock.txt の版どおりに MSYS2 UCRT64 の依存を導入する (CI 用)。

.DESCRIPTION
    CI は MSYS2 の最新ではなく、開発機と同じ凍結版で build と test を行う。
    最新を取ると開発機と CI で gcc・Qt・MLT・FFmpeg・clang の版がずれ、
    片方だけで通る・落ちる状態になる (MLT 7.36.1 の実 DLL に依存する検証もある)。

    lock の「全インストール済みパッケージ」を版指定で取得し、署名を必須にして
    pacman -U で導入する。pacman の設定から repository を外すので、lock に無い依存を
    最新の repository から黙って補うことはない。導入後の UCRT64 パッケージ集合が lock と
    完全一致しなければ失敗する。

    mirror から版が消えていたら代替を探さずに失敗する。そのときは開発機の
    third_party/pkgs (scripts/freeze-deps.ps1 の退避先) を -PackageDirectory で渡す。

.PARAMETER Msys2Root
    導入先の MSYS2 ルート。開発機の C:\msys64 は拒否する (開発機は bootstrap-msys2.ps1 で管理する)。

.PARAMETER CacheDirectory
    取得したパッケージの置き場。既にあるファイルは取得し直さない (署名で検証する)。

.PARAMETER PackageDirectory
    取得済みの .pkg.tar.zst と .sig がある directory。指定時は mirror から取得しない。

.EXAMPLE
    pwsh scripts/install-locked-deps.ps1 -Msys2Root D:\a\_temp\msys64
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Msys2Root,
    [string]$CacheDirectory,
    [string]$PackageDirectory,
    [string]$Mirror = 'https://repo.msys2.org/mingw/ucrt64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$LockFile = Join-Path $RepoRoot 'docs\deps-lock.txt'
$Bash = Join-Path $Msys2Root 'usr\bin\bash.exe'
if (-not $CacheDirectory) { $CacheDirectory = Join-Path $RepoRoot 'build\msys2-pkgs' }

$root = [IO.Path]::GetFullPath($Msys2Root).TrimEnd('\')
if ($root -ieq 'C:\msys64') { throw '開発機の MSYS2 は対象にしません。scripts/bootstrap-msys2.ps1 を使ってください' }
if (-not (Test-Path -LiteralPath $Bash)) { throw "MSYS2 の bash がありません: $Bash" }

# --- lock の読み取り ---------------------------------------------------------
# 先頭の「直接指定」節は全量の部分集合なので、全量の見出し以降だけを読む。
$lines = Get-Content -LiteralPath $LockFile -Encoding utf8
$start = [Array]::FindIndex($lines, [Predicate[string]] { param($l) $l -match '^# UCRT64 環境の全インストール済みパッケージ' })
if ($start -lt 0) { throw "lock に全量の節がありません: $LockFile" }
$locked = [ordered]@{}
foreach ($line in $lines[($start + 1)..($lines.Count - 1)]) {
    if ($line -notmatch '^(mingw-w64-ucrt-x86_64-\S+) (\S+)$') { continue }
    if ($locked.Contains($Matches[1])) { throw "lock に同じパッケージが二度あります: $($Matches[1])" }
    $locked[$Matches[1]] = $Matches[2]
}
if ($locked.Count -eq 0) { throw "lock からパッケージを読めません: $LockFile" }
Write-Host "lock: $($locked.Count) パッケージ"

# --- 取得 --------------------------------------------------------------------
$null = New-Item -ItemType Directory -Force -Path $CacheDirectory
$files = foreach ($name in $locked.Keys) { "$name-$($locked[$name])-any.pkg.tar.zst" }
$missing = [System.Collections.Generic.List[string]]::new()
foreach ($file in $files) {
    foreach ($part in @($file, "$file.sig")) {
        $target = Join-Path $CacheDirectory $part
        if (Test-Path -LiteralPath $target) { continue }
        if ($PackageDirectory) {
            $source = Join-Path $PackageDirectory $part
            if (-not (Test-Path -LiteralPath $source)) { $missing.Add($part); continue }
            Copy-Item -LiteralPath $source -Destination $target
        } else {
            $missing.Add($part)
        }
    }
}
if ($missing.Count -gt 0 -and -not $PackageDirectory) {
    Write-Host "mirror から取得: $($missing.Count) ファイル"
    # 途中で切れたファイルを残さないため、一時名で受けて揃ってから改名する。
    $config = Join-Path $CacheDirectory 'curl-download.txt'
    # curl の設定ファイルは引用符の中の \ を escape として読むので、出力先は / で区切る。
    # curl が exit 0 でも書けていないことがあった (実測) ので、下で実体の有無も検査する。
    $missing | ForEach-Object {
        "url = `"$Mirror/$($_.Replace('~', '%7E'))`""
        "output = `"$((Join-Path $CacheDirectory ($_ + '.part')).Replace('\', '/'))`""
    } | Set-Content -LiteralPath $config -Encoding utf8NoBOM
    & curl.exe --fail --silent --show-error --location --retry 3 --parallel --parallel-max 8 --config $config
    $curlExit = $LASTEXITCODE
    Remove-Item -LiteralPath $config
    $failed = @($missing | Where-Object { -not (Test-Path -LiteralPath (Join-Path $CacheDirectory ($_ + '.part'))) })
    if ($curlExit -ne 0 -or $failed.Count -gt 0) {
        Get-ChildItem -LiteralPath $CacheDirectory -Filter '*.part' | Remove-Item -Force
        throw @"
lock の版を mirror から取得できません (curl exit $curlExit、不足 $($failed.Count) 件、例: $($failed | Select-Object -First 3))。
mirror から古い版が消えた可能性があります。最新へ置き換えず、開発機の凍結物を使ってください:
    pwsh scripts/install-locked-deps.ps1 -Msys2Root <root> -PackageDirectory third_party/pkgs
"@
    }
    foreach ($part in $missing) {
        Move-Item -LiteralPath (Join-Path $CacheDirectory ($part + '.part')) -Destination (Join-Path $CacheDirectory $part)
    }
} elseif ($missing.Count -gt 0) {
    throw "凍結物に lock の版がありません ($($missing.Count) 件、例: $($missing | Select-Object -First 3)): $PackageDirectory"
}

# --- 導入 --------------------------------------------------------------------
# repository を持たない設定で導入する。lock に無い依存は解決できずに失敗する。
# 署名は必須にする (.sig が隣に無ければ失敗する)。
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
$unixCache = (& $Bash -lc "cygpath -u '$CacheDirectory'").Trim()
if ($LASTEXITCODE -ne 0 -or -not $unixCache) { throw "パスを変換できません: $CacheDirectory" }
$pacmanConfig = @'
[options]
Architecture = auto
CheckSpace
SigLevel = Required
LocalFileSigLevel = Required
'@
# bash 側で読むので LF で書く。CRLF だと各パスの末尾に \r が残り、全件が見つからない (実測)。
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $CacheDirectory 'install-list.txt'), ($files -join "`n") + "`n", $utf8)
[IO.File]::WriteAllText((Join-Path $CacheDirectory 'pacman-locked.conf'), ($pacmanConfig -replace "`r`n", "`n") + "`n", $utf8)
# 全件を一回の pacman に渡す。分けると各回が他の回のパッケージを依存として解決できない。
# xargs は長い引数列を黙って分割したので使わない (実測)。相対名にして引数長を抑える。
& $Bash -lc "cd '$unixCache' && mapfile -t packages < install-list.txt && pacman --config pacman-locked.conf -U --noconfirm --needed `"`${packages[@]}`""
if ($LASTEXITCODE -ne 0) { throw "lock の版を導入できません (pacman exit $LASTEXITCODE)" }

# --- 照合 --------------------------------------------------------------------
$installed = & $Bash -lc "pacman -Q | grep '^mingw-w64-ucrt-x86_64-'"
if ($LASTEXITCODE -ne 0) { throw '導入済みパッケージを取得できません' }
$actual = [ordered]@{}
foreach ($line in $installed) {
    $parts = $line -split '\s+'
    $actual[$parts[0]] = $parts[1]
}
$differences = @(
    foreach ($name in $locked.Keys) {
        if (-not $actual.Contains($name)) { "不足: $name $($locked[$name])" }
        elseif ($actual[$name] -ne $locked[$name]) { "版違い: $name $($actual[$name]) (lock: $($locked[$name]))" }
    }
    foreach ($name in $actual.Keys) {
        if (-not $locked.Contains($name)) { "lock に無い: $name $($actual[$name])" }
    }
)
if ($differences.Count -gt 0) {
    $differences | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    throw "導入後の UCRT64 パッケージが lock と一致しません ($($differences.Count) 件)"
}
Write-Host "lock と一致: $($actual.Count) パッケージ" -ForegroundColor Green
