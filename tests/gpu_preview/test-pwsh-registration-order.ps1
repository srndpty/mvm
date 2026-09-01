$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$cmakePath = Join-Path $PSScriptRoot '..\CMakeLists.txt'
$cmakeText = Get-Content -LiteralPath $cmakePath -Raw
$definition = $cmakeText.IndexOf('find_program(MVM_PWSH pwsh)', [StringComparison]::Ordinal)
$firstUse = $cmakeText.IndexOf('COMMAND "${MVM_PWSH}"', [StringComparison]::Ordinal)

if ($definition -lt 0) {
    throw 'MVM_PWSHの解決処理がありません'
}
if ($firstUse -lt 0) {
    throw 'MVM_PWSHを使うCTest登録がありません'
}
if ($definition -gt $firstUse) {
    throw 'MVM_PWSHが最初のCTest登録より後で解決されています。clean configureではNot Runになります'
}

Write-Output 'PowerShell CTest registration order: PASS'
