# 実 toolchain を使わず、既存の受け入れ結果を消さない CLI 契約を検査する。
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Root
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$caseDirectory = Join-Path $Root ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $caseDirectory -Force | Out-Null
$markerPath = Join-Path $caseDirectory '保存する結果.txt'
[IO.File]::WriteAllText($markerPath, '過去の結果は上書きしない')
$before = (Get-FileHash -LiteralPath $markerPath).Hash
& $Exe (Join-Path $caseDirectory '存在しないmanim.exe') $caseDirectory
$code = $LASTEXITCODE
if ($code -ne 2) {
    throw "既存 directory の拒否は終了コード 2 が必要です: $code"
}
if (-not (Test-Path -LiteralPath $markerPath) -or
    (Get-FileHash -LiteralPath $markerPath).Hash -ne $before -or
    @(Get-ChildItem -LiteralPath $caseDirectory).Count -ne 1) {
    throw '拒否した directory の既存結果を変更しました'
}
Write-Host '既存結果を変更せず、外部 renderer の確認より前に拒否しました'
