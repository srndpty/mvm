[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Library,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $Library

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$cachePath = Join-Path $OutputDir 'CMakeCache.txt'
$ucrt64 = 'C:\msys64\ucrt64'
$repoRoot = 'C:\repo'
$good = @(
    'CMAKE_C_COMPILER:FILEPATH=C:/msys64/ucrt64/bin/gcc.exe'
    'CMAKE_CXX_COMPILER:FILEPATH=C:/msys64/ucrt64/bin/g++.exe'
    'Qt6_DIR:PATH=C:/msys64/ucrt64/lib/cmake/Qt6'
    'CMAKE_HOME_DIRECTORY:INTERNAL=C:/repo'
)
Set-Content -LiteralPath $cachePath -Value $good -Encoding utf8NoBOM
Assert-MvmCachedToolchain -CachePath $cachePath -Ucrt64 $ucrt64 -RepoRoot $repoRoot

$bad = $good | ForEach-Object { $_ -replace 'Qt6_DIR:PATH=C:/msys64/ucrt64', 'Qt6_DIR:PATH=C:/other' }
Set-Content -LiteralPath $cachePath -Value $bad -Encoding utf8NoBOM
$rejected = $false
try {
    Assert-MvmCachedToolchain -CachePath $cachePath -Ucrt64 $ucrt64 -RepoRoot $repoRoot
} catch {
    $rejected = "$($_.Exception.Message)" -match 'Qt6_DIR'
}
if (-not $rejected) {
    throw '異なる Qt の CMake cache を拒否できませんでした'
}
Write-Host 'CMake cache の正例と異なる Qt の負例を確認しました'
