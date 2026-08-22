param(
    [string]$PresentMonRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tmp\presentmon-r2-src'),
    [string]$Vcpkg = 'C:\dev\vcpkg\vcpkg\vcpkg.exe',
    [string]$VcpkgInstalledRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tmp\presentmon-r2-vcpkg')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$expectedCommit = '717c5bf14e80a4a06b70cd16415ae8d40a7ce201'
$source = 'https://github.com/GameTechDev/PresentMon.git'

if (-not (Test-Path -LiteralPath $Vcpkg)) { throw "vcpkg executableがありません: $Vcpkg" }
if (-not (Test-Path -LiteralPath $PresentMonRoot)) {
    & git clone --filter=blob:none --no-checkout $source $PresentMonRoot
    if ($LASTEXITCODE -ne 0) { throw 'PresentMon cloneに失敗しました' }
    & git -C $PresentMonRoot fetch --depth 1 origin $expectedCommit
    if ($LASTEXITCODE -ne 0) { throw '固定PresentMon commitの取得に失敗しました' }
    & git -C $PresentMonRoot checkout --detach $expectedCommit
    if ($LASTEXITCODE -ne 0) { throw '固定PresentMon commitのcheckoutに失敗しました' }
}
$actualCommit = (& git -C $PresentMonRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $expectedCommit) {
    throw "PresentMon sourceが固定commitではありません: $actualCommit"
}

New-Item -ItemType Directory -Force -Path $VcpkgInstalledRoot | Out-Null
& $Vcpkg install --triplet x64-windows-static "--x-manifest-root=$PresentMonRoot" `
    "--x-install-root=$VcpkgInstalledRoot"
if ($LASTEXITCODE -ne 0) { throw 'PresentMon decoder依存のvcpkg installに失敗しました' }

& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build-p2-etw-decoder.ps1') `
    -PresentMonRoot $PresentMonRoot -VcpkgInstalledRoot $VcpkgInstalledRoot
if ($LASTEXITCODE -ne 0) { throw 'PresentMon decoder buildに失敗しました' }

