<#
.SYNOPSIS
    版を固定した clang-format を build/tools へ導入する。

.DESCRIPTION
    scripts/clang-format.requirements.txt の版と SHA256 に一致する PyPI の wheel だけを入れる。
    CI と開発機の両方がこれを使う。MSYS2 の clang-format は使わない。

.PARAMETER Python
    pip を実行する Python。wheel は hash で照合するので、どの Python でも同じ binary になる。

.EXAMPLE
    pwsh scripts/install-clang-format.ps1
#>
[CmdletBinding()]
param([string]$Python = 'python')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'lib\clang-format.ps1')
$tool = Get-MvmClangFormat -RepoRoot $RepoRoot

if (-not (Test-Path -LiteralPath $tool.Exe)) {
    # 途中で失敗した導入先を残さない。次回は最初から入れ直す。
    if (Test-Path -LiteralPath $tool.Root) { Remove-Item -LiteralPath $tool.Root -Recurse -Force }
    & $Python -m pip install --disable-pip-version-check --no-deps --only-binary=:all: `
        --require-hashes --requirement $tool.Requirements --target $tool.Root
    if ($LASTEXITCODE -ne 0) {
        if (Test-Path -LiteralPath $tool.Root) { Remove-Item -LiteralPath $tool.Root -Recurse -Force }
        throw "clang-format $($tool.Version) を導入できません (pip exit $LASTEXITCODE)"
    }
}

$actual = & $tool.Exe --version
if ("$actual" -notmatch "clang-format version $([regex]::Escape($tool.Version))(\s|$)") {
    throw "導入した clang-format の版が固定と違います: $actual (固定: $($tool.Version))"
}
Write-Host "clang-format: $actual ($($tool.Exe))"
