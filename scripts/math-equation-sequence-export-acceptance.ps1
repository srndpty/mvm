<#
.SYNOPSIS
    P3-6 の実 Manim・製品 UI の保存再読込・preview・export を検査する。
.DESCRIPTION
    【操作可】背面・入力透過の window と display-power lease を使う。
    保存先が既に存在する場合は拒否し、失敗した証拠も保存する。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$EvidencePath,
    [string]$ManimExecutablePath = (Join-Path $env:USERPROFILE '.local\bin\manim.exe'),
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$evidenceDirectory = [IO.Path]::GetFullPath($EvidencePath)
$runLog = $evidenceDirectory + '.log'
if ((Test-Path -LiteralPath $evidenceDirectory) -or (Test-Path -LiteralPath $runLog)) {
    throw '証拠の保存先が既にあります。別の新しい名前を指定してください。'
}
if (-not (Test-Path -LiteralPath $ManimExecutablePath -PathType Leaf)) {
    throw '指定された Manim executable がありません。'
}
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Preset ucrt64-release -Target mvm_test_text_ui_input
    if ($LASTEXITCODE -ne 0) { throw '製品 UI の試験をビルドできませんでした。' }
}
Write-Host '【操作可】試験中も通常の PC 操作を続けられます。描画の電源前提を監視します。'
. (Join-Path $PSScriptRoot 'lib\test-display-lease.ps1')
$previousPath = $env:PATH
$previousManim = $env:MVM_P36_REAL_MANIM
$previousFixedWindow = $env:MVM_TEST_FIXED_WINDOW
$displayLease = $null
$resultCode = 1
try {
    $env:PATH = 'C:\msys64\ucrt64\bin;' +
        (Join-Path $env:LOCALAPPDATA 'Programs\MiKTeX\miktex\bin\x64') + ';' + $previousPath
    $env:MVM_P36_REAL_MANIM = [IO.Path]::GetFullPath($ManimExecutablePath)
    $env:MVM_TEST_FIXED_WINDOW = '1'
    $displayLease = Start-MvmTestDisplayLease
    $displayLease.AssertValid()
    & (Join-Path $repoRoot 'build\ucrt64-release\bin\mvm_test_text_ui_input.exe') `
        --equation-sequence-ui $evidenceDirectory 2>&1 | Tee-Object -FilePath $runLog
    $resultCode = $LASTEXITCODE
    $displayLease.AssertValid()
} finally {
    if ($displayLease) { $displayLease.Dispose() }
    $env:PATH = $previousPath
    $env:MVM_P36_REAL_MANIM = $previousManim
    $env:MVM_TEST_FIXED_WINDOW = $previousFixedWindow
}
exit $resultCode
