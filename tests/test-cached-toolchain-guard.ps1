[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Library,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $Library

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$repoRoot = (Resolve-Path -LiteralPath $OutputDir).Path
$repoSlash = $repoRoot.Replace('\', '/')
$cachePath = Join-Path $repoRoot 'CMakeCache.txt'
$presetsPath = Join-Path $repoRoot 'CMakePresets.json'
$ucrt64 = 'C:\msys64\ucrt64'
$preset = 'ucrt64-release'
$toolchainArguments = @(Get-MvmCMakeToolchainArguments -Ucrt64 $ucrt64)

$presetJson = @'
{"version":6,"configurePresets":[
  {"name":"ucrt64-base","hidden":true,"generator":"Ninja",
   "cacheVariables":{"CMAKE_PREFIX_PATH":"C:/msys64/ucrt64",
                     "PKG_CONFIG_EXECUTABLE":"C:/msys64/ucrt64/bin/pkgconf.exe",
                     "CMAKE_EXPORT_COMPILE_COMMANDS":"ON"}},
  {"name":"ucrt64-release","inherits":"ucrt64-base",
   "cacheVariables":{"CMAKE_BUILD_TYPE":"RelWithDebInfo"}}
]}
'@
Set-Content -LiteralPath $presetsPath -Value $presetJson -Encoding utf8NoBOM

$good = @(
    'CMAKE_C_COMPILER:FILEPATH=C:/msys64/ucrt64/bin/gcc.exe'
    'CMAKE_CXX_COMPILER:FILEPATH=C:/msys64/ucrt64/bin/g++.exe'
    'CMAKE_MAKE_PROGRAM:FILEPATH=C:/msys64/ucrt64/bin/ninja.exe'
    'CMAKE_PREFIX_PATH:PATH=C:/msys64/ucrt64'
    'Qt6_DIR:PATH=C:/msys64/ucrt64/lib/cmake/Qt6'
    'PKG_CONFIG_EXECUTABLE:FILEPATH=C:/msys64/ucrt64/bin/pkgconf.exe'
    'MVM_UCRT64_ROOT:PATH=C:/msys64/ucrt64'
    'CMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON'
    'CMAKE_BUILD_TYPE:STRING=RelWithDebInfo'
    'CMAKE_GENERATOR:INTERNAL=Ninja'
    "CMAKE_HOME_DIRECTORY:INTERNAL=$repoSlash"
    'BUILD_TESTING:BOOL=ON'
    'MVM_ENABLE_COVERAGE:BOOL=OFF'
    'MVM_ENABLE_QT:BOOL=ON'
)
Set-Content -LiteralPath $cachePath -Value $good -Encoding utf8NoBOM
$checkArgs = @{
    CachePath = $cachePath
    PresetsPath = $presetsPath
    Preset = $preset
    RepoRoot = $repoRoot
    ToolchainArguments = $toolchainArguments
}
Assert-MvmCachedToolchain @checkArgs

$cases = @(
    @{ Name = '異なる compiler'; Key = 'CMAKE_CXX_COMPILER'; Before = 'g++.exe'; After = 'clang++.exe' }
    @{ Name = '異なる repo'; Key = 'CMAKE_HOME_DIRECTORY'; Before = $repoSlash; After = 'C:/other' }
    @{ Name = '異なる build type'; Key = 'CMAKE_BUILD_TYPE'; Before = 'RelWithDebInfo'; After = 'Release' }
    @{ Name = '異なる prefix'; Key = 'CMAKE_PREFIX_PATH'; Before = 'CMAKE_PREFIX_PATH:PATH=C:/msys64/ucrt64'; After = 'CMAKE_PREFIX_PATH:PATH=C:/other' }
    @{ Name = '異なる pkg-config'; Key = 'PKG_CONFIG_EXECUTABLE'; Before = 'pkgconf.exe'; After = 'other.exe' }
    @{ Name = '異なる Qt'; Key = 'Qt6_DIR'; Before = 'Qt6_DIR:PATH=C:/msys64/ucrt64'; After = 'Qt6_DIR:PATH=C:/other' }
    @{ Name = '異なる generator'; Key = 'CMAKE_GENERATOR'; Before = 'CMAKE_GENERATOR:INTERNAL=Ninja'; After = 'CMAKE_GENERATOR:INTERNAL=Visual Studio' }
    @{ Name = 'テスト無効'; Key = 'BUILD_TESTING'; Before = 'BUILD_TESTING:BOOL=ON'; After = 'BUILD_TESTING:BOOL=OFF' }
    @{ Name = '異なる MVM option'; Key = 'MVM_ENABLE_QT'; Before = 'MVM_ENABLE_QT:BOOL=ON'; After = 'MVM_ENABLE_QT:BOOL=OFF' }
)
foreach ($case in $cases) {
    $bad = $good | ForEach-Object { $_.Replace($case.Before, $case.After) }
    Set-Content -LiteralPath $cachePath -Value $bad -Encoding utf8NoBOM
    $rejected = $false
    try {
        Assert-MvmCachedToolchain @checkArgs
    } catch {
        $rejected = "$($_.Exception.Message)" -match [regex]::Escape($case.Key)
    }
    if (-not $rejected) { throw "$($case.Name) cache を該当設定で拒否できませんでした" }
}

Set-Content -LiteralPath $cachePath -Value $good -Encoding utf8NoBOM
$originalSignature = Get-MvmConfigureSignature -Preset $preset -Ucrt64 $ucrt64 `
    -RepoRoot $repoRoot -ToolchainArguments $toolchainArguments
Set-Content -LiteralPath $presetsPath -Value ($presetJson.Replace('RelWithDebInfo', 'Release')) `
    -Encoding utf8NoBOM
$changedSignature = Get-MvmConfigureSignature -Preset $preset -Ucrt64 $ucrt64 `
    -RepoRoot $repoRoot -ToolchainArguments $toolchainArguments
if ($originalSignature -eq $changedSignature) {
    throw 'CMakePresets.json の変更を署名で検出できませんでした'
}
Set-Content -LiteralPath $presetsPath -Value $presetJson -Encoding utf8NoBOM
$changedToolchainSignature = Get-MvmConfigureSignature -Preset $preset -Ucrt64 $ucrt64 `
    -RepoRoot $repoRoot -ToolchainArguments @($toolchainArguments + '-DMVM_EXTRA:BOOL=ON')
if ($originalSignature -eq $changedToolchainSignature) {
    throw 'toolchain 引数の変更を署名で検出できませんでした'
}
Write-Host 'CMake cache の正例・9負例と preset / toolchain 変更の署名差を確認しました'
