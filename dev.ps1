<#
.SYNOPSIS
    mvm の開発用コマンドを統一された入口から実行する。

.DESCRIPTION
    既存の正式な build / test / lint スクリプトと GUI 起動手順への薄い入口を提供する。
    判定条件やビルド処理はこのファイルへ重複実装しない。

.EXAMPLE
    .\dev.ps1 build
    .\dev.ps1 run
    .\dev.ps1 gui
    .\dev.ps1 test
    .\dev.ps1 test-full
    .\dev.ps1 lint
    .\dev.ps1 help
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$Command = 'help',

    [string]$Ucrt64 = 'C:\msys64\ucrt64',

    [string]$ManimExecutable =
        (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.local\bin\manim.exe')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = $PSScriptRoot
$scriptsDirectory = Join-Path $repoRoot 'scripts'
$releaseBuildDirectory = Join-Path $repoRoot 'build\ucrt64-release'
$debugBuildDirectory = Join-Path $repoRoot 'build\ucrt64-debug'

function Show-DevHelp {
    Write-Output @'
使い方: .\dev.ps1 <command> [options]

利用可能なコマンド:
  build  通常の release ビルドを実行する
  run    debug の mvm だけを増分ビルドして GUI を起動する
  gui    release ビルド済みの mvm GUI を起動する
  test       release の短縮テストを実行する（日常用）
  test-full  release/debug の portable 通常テストをすべて実行する
  lint   整形差分・静的検査・アーキテクチャ検査を実行する
  help   このヘルプを表示する

共通オプション:
  -Ucrt64 <path>            MSYS2 UCRT64 のルート
  -ManimExecutable <path>   run / gui で使用する manim.exe

各コマンドは既存の正式スクリプトまたは起動手順へ処理を委譲します。
'@
}

function Invoke-CanonicalScript {
    param([Parameter(Mandatory)][string]$Name)

    $scriptPath = Join-Path $scriptsDirectory $Name
    $pwsh = (Get-Process -Id $PID).Path
    & $pwsh -NoProfile -File $scriptPath -Ucrt64 $Ucrt64
    exit $LASTEXITCODE
}

function Invoke-Tests {
    param([string[]]$TestArgs)

    $pwsh = (Get-Process -Id $PID).Path
    & $pwsh -NoProfile -File (Join-Path $scriptsDirectory 'test.ps1') -Ucrt64 $Ucrt64 @TestArgs
    exit $LASTEXITCODE
}

function Start-MvmGui {
    param(
        [Parameter(Mandatory)][string]$BuildDirectory,
        [Parameter(Mandatory)][string]$MissingBuildHint
    )

    $ucrt64Bin = Join-Path $Ucrt64 'bin'
    $executable = Join-Path $BuildDirectory 'bin\mvm.exe'
    $projectDirectory = Join-Path $BuildDirectory 'm6a-gui'
    $projectPath = Join-Path $projectDirectory 'project.mvm'

    if (-not (Test-Path -LiteralPath $ucrt64Bin -PathType Container)) {
        throw "UCRT64 の bin ディレクトリが見つかりません: $ucrt64Bin"
    }
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "mvm GUI が見つかりません: $executable`n$MissingBuildHint"
    }
    if (-not (Test-Path -LiteralPath $ManimExecutable -PathType Leaf)) {
        throw "manim.exe が見つかりません: $ManimExecutable"
    }

    New-Item -ItemType Directory -Path $projectDirectory -Force | Out-Null
    $env:PATH = "$ucrt64Bin;$env:PATH"

    Push-Location $repoRoot
    try {
        & $executable --project $projectPath --manim-executable $ManimExecutable
        exit $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
}

switch ($Command.ToLowerInvariant()) {
    'build' {
        Invoke-CanonicalScript -Name 'build.ps1'
    }
    'run' {
        $buildScript = Join-Path $scriptsDirectory 'build.ps1'
        $pwsh = (Get-Process -Id $PID).Path
        & $pwsh -NoProfile -File $buildScript -Preset ucrt64-debug -Target mvm -Ucrt64 $Ucrt64
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
        Start-MvmGui -BuildDirectory $debugBuildDirectory `
            -MissingBuildHint '.\dev.ps1 run を再実行してください。'
    }
    'test' {
        Invoke-Tests -TestArgs @('-Preset', 'ucrt64-release', '-Group', 'BuildDependent',
            '-Portable', '-Fast')
    }
    'test-full' {
        Invoke-Tests -TestArgs @('-Portable')
    }
    'lint' {
        Invoke-CanonicalScript -Name 'lint.ps1'
    }
    'gui' {
        Start-MvmGui -BuildDirectory $releaseBuildDirectory `
            -MissingBuildHint '先に .\dev.ps1 build を実行してください。'
    }
    'help' {
        Show-DevHelp
        exit 0
    }
    default {
        [Console]::Error.WriteLine("不明な dev コマンドです: $Command")
        Show-DevHelp
        exit 2
    }
}
