#requires -Version 7
# qmltestrunner で QML test を 1 file 実行し、実際に test 関数を実行して全部通ったかを判定する。
#
# Windows の qmltestrunner は GUI subsystem なので stdout が CTest へ届かない。
# 結果を file へ書かせて読み直す。qmltestrunner は test 関数が 0 件でも
# initTestCase / cleanupTestCase だけで成功を返すので、Totals の件数では判定しない。
param(
    [Parameter(Mandatory)][string]$Runner,
    [Parameter(Mandatory)][string]$TestFile,
    [Parameter(Mandatory)][string]$OutputFile
)

$ErrorActionPreference = 'Stop'

if (Test-Path -LiteralPath $OutputFile) {
    Remove-Item -LiteralPath $OutputFile
}
& $Runner -platform offscreen -input $TestFile -o "$OutputFile,txt"
$exitCode = $LASTEXITCODE
if (-not (Test-Path -LiteralPath $OutputFile)) {
    throw "qmltestrunner が結果を書き出しませんでした (exit=$exitCode): $TestFile"
}
$lines = Get-Content -LiteralPath $OutputFile
$lines | ForEach-Object { Write-Output $_ }

$passed = @($lines | Where-Object { $_ -match '^PASS\s+:\s+\S+::\S+::test_' }).Count
$failed = @($lines | Where-Object { $_ -match '^FAIL!' }).Count
if ($exitCode -ne 0 -or $failed -ne 0) {
    throw "QML test が失敗しました (exit=$exitCode, 失敗 $failed 件): $TestFile"
}
if ($passed -eq 0) {
    throw "QML test 関数を 1 件も実行していません: $TestFile"
}
Write-Output "QML test: $passed 件の test 関数が通過"
