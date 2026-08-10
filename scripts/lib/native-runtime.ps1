Set-StrictMode -Version Latest

function Get-MvmDefaultRuntimeBin {
    return 'C:\msys64\ucrt64\bin'
}

function Test-MvmNativeRuntime {
    param([Parameter(Mandatory)][string]$RuntimeBin)

    $fullPath = [System.IO.Path]::GetFullPath($RuntimeBin)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Container)) {
        return [pscustomobject]@{
            Pass = $false
            RuntimeBin = $fullPath
            Reason = "native runtime binがありません: $fullPath"
        }
    }

    # version番号は固定せず、現在のUCRT64 toolchainが提供するDLL familyを検査する。
    $requiredPatterns = @(
        'Qt6Core.dll',
        'Qt6Gui.dll',
        'avcodec-*.dll',
        'avformat-*.dll',
        'avutil-*.dll',
        'swresample-*.dll',
        'libgcc_s_seh-1.dll',
        'libstdc++-6.dll',
        'libwinpthread-1.dll'
    )
    $missing = @()
    foreach ($pattern in $requiredPatterns) {
        if (@(Get-ChildItem -LiteralPath $fullPath -Filter $pattern -File).Count -eq 0) {
            $missing += $pattern
        }
    }
    if ($missing.Count -ne 0) {
        return [pscustomobject]@{
            Pass = $false
            RuntimeBin = $fullPath
            Reason = "native runtime DLLが不足しています: $($missing -join ', ')"
        }
    }

    return [pscustomobject]@{
        Pass = $true
        RuntimeBin = $fullPath
        Reason = $null
    }
}

function Invoke-MvmNativeProcess {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter(Mandatory)][string[]]$ArgumentList,
        [Parameter(Mandatory)][string]$RuntimeBin
    )

    $childPath = "$RuntimeBin;$env:PATH"
    $process = Start-Process -FilePath $FilePath -ArgumentList $ArgumentList `
        -Environment @{ PATH = $childPath } -Wait -PassThru -NoNewWindow
    return [int]$process.ExitCode
}
