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

.PARAMETER ReuseConfigure
    preset と toolchain の署名、および cache の設定が一致すれば明示的な configure を省く。
    Ninja が必要時に再 configure する。

.PARAMETER StallSeconds
    起動した cmake の Job Object の CPU 時間・子 process・.ninja_log がこの秒数の間どれも
    進まなければ、その job だけを止めて BUILD_STALLED で失敗する (scripts/lib/build-watchdog.ps1)。
    0 で検知を無効にする (job による所有と、終了時の残留 process の停止は続く)。
    長い compile・link は CPU 時間が進むので止めない。

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
    [switch]$ReuseConfigure,
    [string]$Target,
    [string]$WhisperRoot = 'C:\msys64\ucrt64',
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    [ValidateRange(0, 86400)][int]$StallSeconds = 180
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'lib\cmake-toolchain.ps1')
. (Join-Path $PSScriptRoot 'lib\build-watchdog.ps1')

$RepoRoot = Split-Path -Parent $PSScriptRoot
$CMake    = Join-Path $Ucrt64 'bin\cmake.exe'
$BuildDir = Join-Path $RepoRoot "build\$Preset"

if (-not (Test-Path $CMake)) {
    throw "UCRT64 の cmake が見つかりません: $CMake`nscripts/bootstrap-msys2.ps1 を先に実行してください。"
}

# UCRT64 を最優先にする。ホストの pip 版 cmake や MSVC 版 Qt を拾わせない。
Set-MvmUcrt64Environment -Ucrt64 $Ucrt64
$toolchainArguments = @(Get-MvmCMakeToolchainArguments -Ucrt64 $Ucrt64) + @("-DMVM_WHISPER_ROOT=$WhisperRoot")

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

# 呼び出し側 (test.ps1) は $LASTEXITCODE を見るので、& で呼んだときと同じく設定する。
function Invoke-WatchedCMake([string[]]$Arguments, [string]$Step, [int]$Stall) {
    $run = Invoke-MvmWatchedProcess -FilePath $CMake -ArgumentList $Arguments `
        -ProgressFile (Join-Path $BuildDir '.ninja_log') -StallSeconds $Stall
    if ($run.Stalled) {
        $global:LASTEXITCODE = 1
        throw @"
$Step が停止しました。
$($run.Report)
このスクリプトが起動した cmake の Job Object (その子孫だけを含む) を終了しました。
Codex sandbox の既知制約 (AGENTS.md) と同じ症状です。source・.ninja_deps / .ninja_log・
build directory を変更せず、同じコマンドを sandbox 外で 1 回実行してください。
sandbox 外でも再現した場合だけ、AGENTS.md の recovery escalation に従ってください。
"@
    }
    $global:LASTEXITCODE = $run.ExitCode
}

Push-Location $RepoRoot
try {
    $cachePath = Join-Path $BuildDir 'CMakeCache.txt'
    $signaturePath = Join-Path $BuildDir 'mvm-configure-signature.txt'
    $signature = Get-MvmConfigureSignature -Preset $Preset -Ucrt64 $Ucrt64 `
        -RepoRoot $RepoRoot -ToolchainArguments $toolchainArguments
    $reuse = $ReuseConfigure -and -not $Clean -and
        (Test-Path -LiteralPath $cachePath -PathType Leaf) -and
        (Test-Path -LiteralPath $signaturePath -PathType Leaf) -and
        ((Get-Content -LiteralPath $signaturePath -Raw).Trim() -eq $signature)
    if ($reuse) {
        Assert-MvmCachedToolchain -CachePath $cachePath `
            -PresetsPath (Join-Path $RepoRoot 'CMakePresets.json') -Preset $Preset `
            -RepoRoot $RepoRoot -ToolchainArguments $toolchainArguments
        Write-Host "`n--- configure は既存 cache を使用 ---" -ForegroundColor Yellow
    } else {
        Write-Host "`n--- configure ---" -ForegroundColor Yellow
        Invoke-WatchedCMake -Arguments (@('--preset', $Preset) + $toolchainArguments) -Step 'configure' `
            -Stall $StallSeconds
        if ($LASTEXITCODE -ne 0) { throw "configure に失敗しました (exit $LASTEXITCODE)" }
        Assert-MvmCachedToolchain -CachePath $cachePath `
            -PresetsPath (Join-Path $RepoRoot 'CMakePresets.json') -Preset $Preset `
            -RepoRoot $RepoRoot -ToolchainArguments $toolchainArguments
        Set-Content -LiteralPath $signaturePath -Value $signature -Encoding utf8NoBOM
    }

    if ($ConfigureOnly) {
        Write-Host "`nconfigure のみ実行しました。" -ForegroundColor Green
        return
    }

    Write-Host "`n--- build ---" -ForegroundColor Yellow
    $buildArguments = @('--build','--preset',$Preset)
    if ($Target) { $buildArguments += @('--target',$Target) }
    Invoke-WatchedCMake -Arguments $buildArguments -Step 'build' -Stall $StallSeconds
    if ($LASTEXITCODE -ne 0) { throw "build に失敗しました (exit $LASTEXITCODE)" }

    Write-Host "`n完了。成果物:" -ForegroundColor Green
    Get-ChildItem "$BuildDir\bin" -Filter '*.exe' -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host "  $($_.FullName)" }
}
finally {
    Pop-Location
}
