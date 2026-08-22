param(
    [string]$PresentMonRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tmp\presentmon-r2-src'),
    [string]$VcpkgInstalledRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tmp\presentmon-r2-vcpkg'),
    [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\p2-etw-decoder')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$expectedCommit = '717c5bf14e80a4a06b70cd16415ae8d40a7ce201'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
$project = Join-Path $repo 'tools\presentmon_oracle\mvm_present_history_decoder.vcxproj'
$props = Join-Path $repo 'tools\presentmon_oracle\presentmon-vcpkg.props'

foreach ($path in @($PresentMonRoot, $VcpkgInstalledRoot, $msbuild, $project, $props)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "ETW decoder buildの必須pathがありません: $path" }
}
$PresentMonRoot = (Resolve-Path -LiteralPath $PresentMonRoot).Path
$VcpkgInstalledRoot = (Resolve-Path -LiteralPath $VcpkgInstalledRoot).Path
$actualCommit = (& git -C $PresentMonRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $expectedCommit) {
    throw "PresentMon commitが固定値と一致しません: $actualCommit"
}
$include = Join-Path $VcpkgInstalledRoot 'x64-windows-static\include'
if (-not (Test-Path -LiteralPath $include)) { throw "vcpkg include rootがありません: $include" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

& $msbuild $project /m:1 /p:Configuration=Release /p:Platform=x64 `
    "/p:PresentMonRoot=$PresentMonRoot" "/p:MvmDecoderOutDir=$OutputDirectory" `
    "/p:MvmPresentMonVcpkgInclude=$include" "/p:ForceImportBeforeCppTargets=$props" `
    /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw "ETW Present-History decoder buildに失敗しました: $LASTEXITCODE" }
$exe = Join-Path $OutputDirectory 'mvm_present_history_decoder.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "decoder executableがありません: $exe" }
[ordered]@{
    schema='mvm-p2-etw-decoder-build-1'; presentmon_commit=$actualCommit
    presentmon_tag='v2.3.1'; decoder_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    presentmon_license='MIT'; presentmon_source='https://github.com/GameTechDev/PresentMon'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'provenance.json') -Encoding utf8
Write-Host "ETW Present-History decoder build: PASS ($exe)"
