<#
.SYNOPSIS
    mvm を MSYS2 UCRT64 で configure / build する。

.DESCRIPTION
    UCRT64 の gcc・ninja・cmake は依存 DLL を PATH から解決するため、
    C:\msys64\ucrt64\bin を PATH の先頭に置かないと、エラー出力を出さずに
    失敗する。その状態で CMake を動かすと
    「The C compiler is not able to compile a simple test program」という
    原因の分からないメッセージだけが出る。

    本スクリプトは PATH を正しく整えたうえで cmake を呼ぶ。
    ucrt64\bin を PATH の「先頭」に置くのは、他プロジェクト用の Qt 6.8.3 (MSVC)
    や C:\tools\ffmpeg.exe を先に拾わせないため。

.PARAMETER Preset
    ucrt64-release (既定) または ucrt64-debug。

.PARAMETER Clean
    build ディレクトリを削除してから configure する。
    CMakeCache.txt に古い誤った値が残っている場合に使う。

.PARAMETER ConfigureOnly
    configure のみ行い、ビルドしない。

.EXAMPLE
    pwsh scripts/build.ps1
    pwsh scripts/build.ps1 -Preset ucrt64-debug -Clean
#>
[CmdletBinding()]
param(
    [ValidateSet('ucrt64-release', 'ucrt64-debug')]
    [string]$Preset = 'ucrt64-release',

    [switch]$Clean,
    [switch]$ConfigureOnly,
    [string]$Ucrt64 = 'C:\msys64\ucrt64'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
$CMake    = Join-Path $Ucrt64 'bin\cmake.exe'
$BuildDir = Join-Path $RepoRoot "build\$Preset"

if (-not (Test-Path $CMake)) {
    throw "UCRT64 の cmake が見つかりません: $CMake`nscripts/bootstrap-msys2.ps1 を先に実行してください。"
}

# UCRT64 を最優先にする。ホストの pip 版 cmake や MSVC 版 Qt を拾わせない。
$env:PATH = "$Ucrt64\bin;$env:PATH"

# 他プロジェクトの設定が漏れてこないようにする
$env:QTDIR = ''
$env:Qt6_DIR = ''
$env:CMAKE_PREFIX_PATH = ''

Write-Host "=== mvm build ($Preset) ===" -ForegroundColor Cyan
Write-Host "cmake : $CMake"
Write-Host "build : $BuildDir"

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "`nbuild ディレクトリを削除します: $BuildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}

Push-Location $RepoRoot
try {
    Write-Host "`n--- configure ---" -ForegroundColor Yellow
    & $CMake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "configure に失敗しました (exit $LASTEXITCODE)" }

    if ($ConfigureOnly) {
        Write-Host "`nconfigure のみ実行しました。" -ForegroundColor Green
        return
    }

    Write-Host "`n--- build ---" -ForegroundColor Yellow
    & $CMake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "build に失敗しました (exit $LASTEXITCODE)" }

    Write-Host "`n完了。成果物:" -ForegroundColor Green
    Get-ChildItem "$BuildDir\bin" -Filter '*.exe' -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host "  $($_.FullName)" }
}
finally {
    Pop-Location
}
