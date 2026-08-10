[CmdletBinding()]
param(
    [string]$Executable = 'build/ucrt64-release/bin/mvm_p4_composition_spike.exe',
    [string]$SourceA = 'tests/assets/p3_audio/p3_av_h264_aac.mp4',
    [string]$SourceB = 'tests/assets/p3_audio/p3_video_hevc_b.mp4',
    [Parameter(Mandatory)][string]$Output,
    [string]$RuntimeBin
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'lib\native-runtime.ps1')

if ([string]::IsNullOrWhiteSpace($RuntimeBin)) {
    $RuntimeBin = Get-MvmDefaultRuntimeBin
}

$checker = Join-Path $PSScriptRoot 'check-p4-smoke-contract.ps1'
$outputPath = if ([System.IO.Path]::IsPathRooted($Output)) {
    [System.IO.Path]::GetFullPath($Output)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $root $Output))
}

Push-Location $root
try {
    $runtime = Test-MvmNativeRuntime -RuntimeBin $RuntimeBin
    if (-not $runtime.Pass) {
        Write-Host "[p4-smoke-runner] FAIL $($runtime.Reason)"
        exit 3
    }
    foreach ($path in @($Executable, $SourceA, $SourceB)) {
        if (-not (Test-Path -LiteralPath $path)) {
            Write-Host "[p4-smoke-runner] FAIL 入力がありません: $path"
            exit 3
        }
    }
    $parent = Split-Path -Parent $outputPath
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $processExit = Invoke-MvmNativeProcess -FilePath $Executable -RuntimeBin $runtime.RuntimeBin `
        -ArgumentList @('--source-a', $SourceA, '--source-b', $SourceB,
            '--metrics', $outputPath, '--workload', 'smoke')
    $rawExists = Test-Path -LiteralPath $outputPath
    $checkerExit = 3
    if ($rawExists) {
        & (Get-Process -Id $PID).Path -NoProfile -File $checker -Json $outputPath
        $checkerExit = $LASTEXITCODE
    }
    Write-Host "[p4-smoke-runner] process_exit=$processExit raw_exists=$rawExists checker_exit=$checkerExit"
    if ($processExit -ne 0 -or -not $rawExists -or $checkerExit -ne 0) {
        exit 3
    }
} finally {
    Pop-Location
}
