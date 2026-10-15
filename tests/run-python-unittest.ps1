#requires -Version 7
# Python の unittest を 1 file 実行し、test が実際に走って全部通ったかを判定する。
#
# CTest の BuildIndependent は先頭が pwsh の試験だけを選ぶ (scripts/lib/ctest-selection.ps1)。
# python を直接登録すると build の後でしか走らないので、ここを通す。
# -I で利用者の site-packages・環境変数・cwd を読まない。-B で repo に __pycache__ を書かない。
param(
    [Parameter(Mandatory)][string]$Python,
    [Parameter(Mandatory)][string]$TestFile
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $TestFile -PathType Leaf)) {
    throw "試験の file がありません: $TestFile"
}
$file = Get-Item -LiteralPath $TestFile
# 日本語の失敗理由を CTest の log で読めるようにする (-I は PYTHONUTF8 を無視するので -X で渡す)。
$output = & $Python -I -B -X utf8 -m unittest discover -v -s $file.DirectoryName -p $file.Name 2>&1
$exitCode = $LASTEXITCODE
$lines = @($output | ForEach-Object { "$_" })
$lines | ForEach-Object { Write-Output $_ }

if ($exitCode -ne 0) {
    throw "Python の試験が失敗しました (exit=$exitCode): $TestFile"
}
$ran = $lines | Where-Object { $_ -match '^Ran (\d+) tests? in ' } | Select-Object -Last 1
if (-not $ran -or [int]([regex]::Match($ran, '^Ran (\d+)').Groups[1].Value) -eq 0) {
    throw "Python の試験を 1 件も実行していません: $TestFile"
}
Write-Output "Python test: $ran"
