[CmdletBinding()]
param([Parameter(Mandatory)][string]$Model, [string]$Preset = 'ucrt64-release')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Write-Host '【操作可】音声を出さずに認識を検査します。CPU／GPU負荷は上がりますが、通常のPC操作を続けてかまいません。'
$repoRoot = Split-Path -Parent $PSScriptRoot
$testExe = Join-Path $repoRoot "build/$Preset/bin/mvm_test_transcribe.exe"
$fixture = Join-Path $repoRoot 'tests/assets/transcribe/speech.wav'
$workDir = Join-Path $repoRoot "build/$Preset/transcribe-integration"
if (-not (Test-Path -LiteralPath $Model -PathType Leaf)) { throw 'ローカルの多言語Whisperモデルを指定してください' }
if (-not (Test-Path -LiteralPath $testExe)) { throw '先に pwsh scripts/build.ps1 -Target mvm_test_transcribe を実行してください' }
if (-not (Test-Path -LiteralPath $fixture)) { throw '先に pwsh scripts/make-transcribe-fixture.ps1 を実行してください' }
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
foreach ($backend in @('cpu', 'vulkan')) {
    & $testExe $fixture $workDir $Model $backend
    if ($LASTEXITCODE) { throw "$backend の実認識試験に失敗しました" }
}
Write-Host 'CPU／Vulkanの実モデル認識試験が2/2通過しました。'
