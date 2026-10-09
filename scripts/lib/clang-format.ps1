# 版を固定した clang-format の所在。版は scripts/clang-format.requirements.txt にだけ書く。
#
# CI と開発機で同じ clang-format を使うための固定である。MSYS2 の clang は rolling 更新で
# 版が変わり、版ごとに整形結果が違う (22.1.8 と 23.1.3 で lambda の閉じ括弧の字下げが
# 食い違い、手元で通る lint が CI で落ちた)。
# 版を上げるときは requirements の version と hash を一緒に更新し、scripts/format.ps1 で
# 全体を整形し直す。requirements は pip がロケールの符号化で読むので ASCII だけで書く。

function Get-MvmClangFormat {
    param([Parameter(Mandatory)][string]$RepoRoot)

    $requirements = Join-Path $RepoRoot 'scripts\clang-format.requirements.txt'
    $pin = Select-String -LiteralPath $requirements -Pattern '^clang-format==(\S+)\s' |
        Select-Object -First 1
    if (-not $pin) { throw "clang-format の版を読めません: $requirements" }
    $version = $pin.Matches[0].Groups[1].Value
    $root = Join-Path $RepoRoot "build\tools\clang-format-$version"
    return [pscustomobject]@{
        Version      = $version
        Requirements = $requirements
        Root         = $root
        Exe          = Join-Path $root 'clang_format\data\bin\clang-format.exe'
    }
}
