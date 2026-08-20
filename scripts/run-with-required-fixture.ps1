<#
.SYNOPSIS
    必須fixtureをfail-closedで検査してから子プロセスを実行する。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$RequiredFile,
    [string[]]$AdditionalRequiredFile = @(),
    [Parameter(Mandatory)][string]$Hint,
    [string]$Exe = '',
    [string]$ChildArgs = '',
    [switch]$ProbeOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$requiredFiles = @($RequiredFile) + @($AdditionalRequiredFile)
foreach ($file in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        Write-Host "必須fixtureがありません: $file" -ForegroundColor Red
        Write-Host "生成コマンド: $Hint" -ForegroundColor Red
        exit 2
    }
}

if ($ProbeOnly) {
    Write-Host "必須fixtureを確認しました: $($requiredFiles -join ', ')"
    exit 0
}

if (-not $Exe -or -not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    Write-Host "実行ファイルがありません: $Exe" -ForegroundColor Red
    exit 3
}

$argv = @($ChildArgs -split '\|' | Where-Object { $_ -ne '' })
& $Exe $requiredFiles @argv
exit $LASTEXITCODE
