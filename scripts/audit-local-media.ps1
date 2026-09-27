<#
.SYNOPSIS
    ローカル動画を走査し、mvm の import 前提を機械的に検査する。

.DESCRIPTION
    指定 directory 配下の動画候補を再帰的に列挙し、各ファイルへ
    `mvm_bench probe` を実行する。素材は読み取るだけで変更・コピーしない。

    IMPORTABLE は MvmController::addVideoClip() と同じく、MLT が映像 stream、
    有限尺、正の frame count、正の FPS を確認できたことを示す。
    実際の preview 可否は GPU と D3D11VA decoder に依存するため、この結果だけでは
    保証しない。

.EXAMPLE
    pwsh scripts/audit-local-media.ps1 -Root $env:USERPROFILE\Videos

.EXAMPLE
    pwsh scripts/audit-local-media.ps1 -Root D:\Media,E:\Archive -MaxFiles 100
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$Root,
    [string]$Bench = (Join-Path $PSScriptRoot '..\build\ucrt64-release\bin\mvm_bench.exe'),
    [string]$Ucrt64 = 'C:\msys64\ucrt64',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ("..\build\media-compatibility-audit-{0}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [ValidateRange(0, 1000000)][int]$MaxFiles = 0,
    [ValidateRange(1, 3600)][int]$TimeoutSeconds = 120,
    [switch]$Resume
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$videoExtensions = @(
    '.3g2', '.3gp', '.amv', '.asf', '.avi', '.dav', '.divx', '.drc', '.dv', '.f4v',
    '.flv', '.gxf', '.m1v', '.m2p', '.m2t', '.m2ts', '.m2v', '.m4v', '.mkv', '.mod',
    '.mov', '.mp4', '.mpeg', '.mpg', '.mts', '.mxf', '.nut', '.ogm', '.ogv', '.qt',
    '.rm', '.rmvb', '.roq', '.tod', '.ts', '.vob', '.vro', '.webm', '.wmv', '.wtv', '.y4m'
)
$extensionSet = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($videoExtension in $videoExtensions) {
    [void]$extensionSet.Add($videoExtension)
}

$benchPath = [IO.Path]::GetFullPath($Bench)
if (-not (Test-Path -LiteralPath $benchPath -PathType Leaf)) {
    throw "mvm_bench がありません。先に '.\dev.ps1 build' を実行してください: $benchPath"
}
$ucrtBin = Join-Path $Ucrt64 'bin'
if (-not (Test-Path -LiteralPath $ucrtBin -PathType Container)) {
    throw "MSYS2 UCRT64 の bin directory がありません: $ucrtBin"
}

$resolvedRoots = @()
foreach ($candidateRoot in $Root) {
    if (-not (Test-Path -LiteralPath $candidateRoot -PathType Container)) {
        throw "走査 directory がありません: $candidateRoot"
    }
    $resolvedRoots += (Resolve-Path -LiteralPath $candidateRoot).Path
}

$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$rawDirectory = Join-Path $outputPath 'raw'
New-Item -ItemType Directory -Path $rawDirectory -Force | Out-Null

$files = @($resolvedRoots | ForEach-Object {
    Get-ChildItem -LiteralPath $_ -Recurse -File -ErrorAction SilentlyContinue
} | Where-Object {
    $extensionSet.Contains($_.Extension)
} | Sort-Object -Property FullName -Unique)

if ($MaxFiles -gt 0) {
    $files = @($files | Select-Object -First $MaxFiles)
}
if ($files.Count -eq 0) {
    throw '動画候補が 0 件です。Root または対象拡張子を確認してください'
}

function Get-PathId([string]$Value) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Value)
    $hash = [Security.Cryptography.SHA256]::HashData($bytes)
    return [Convert]::ToHexString($hash).Substring(0, 16).ToLowerInvariant()
}

function Invoke-Probe([IO.FileInfo]$File, [string]$JsonPath, [int]$ProbeTimeoutSeconds) {
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $benchPath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    # mvm_bench と同じ UCRT64 の FFmpeg / MLT / GCC runtime だけを解決させる。
    $startInfo.Environment['PATH'] = "$ucrtBin;$($startInfo.Environment['PATH'])"
    foreach ($argument in @('probe', $File.FullName, '--json', $JsonPath)) {
        [void]$startInfo.ArgumentList.Add($argument)
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "probe process を起動できません: $($File.FullName)"
    }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $completed = $process.WaitForExit($ProbeTimeoutSeconds * 1000)
    if (-not $completed) {
        $process.Kill($true)
        $process.WaitForExit()
    }
    $stdoutText = $stdoutTask.GetAwaiter().GetResult()
    $stderrText = $stderrTask.GetAwaiter().GetResult()
    $exitCode = if ($completed) { $process.ExitCode } else { $null }
    $process.Dispose()
    return [pscustomobject]@{
        completed = $completed
        exit_code = $exitCode
        stdout = $stdoutText.Trim()
        stderr = $stderrText.Trim()
    }
}

$records = [Collections.Generic.List[object]]::new()
$index = 0
foreach ($file in $files) {
    $index++
    Write-Progress -Activity 'ローカル動画の互換性検査' -Status "$index / $($files.Count): $($file.Name)" -PercentComplete (($index / $files.Count) * 100)
    $pathId = Get-PathId $file.FullName
    $rawPath = Join-Path $rawDirectory "$pathId.json"

    $probe = $null
    $parseDiagnostic = ''
    if ($Resume -and (Test-Path -LiteralPath $rawPath -PathType Leaf)) {
        try {
            $candidateProbe = Get-Content -LiteralPath $rawPath -Raw -Encoding utf8 | ConvertFrom-Json
            if ([string]$candidateProbe.path -eq $file.FullName) {
                $probe = $candidateProbe
            }
        } catch {
            $parseDiagnostic = $_.Exception.Message
        }
    }
    if ($null -ne $probe) {
        $run = [pscustomobject]@{
            completed = $true
            exit_code = if ($probe.mlt_ok -and $probe.ffprobe_ok -and @($probe.mismatches).Count -eq 0) { 0 } else { 3 }
            stdout = ''
            stderr = '既存の生JSONを再利用しました'
        }
    } else {
        $run = Invoke-Probe -File $file -JsonPath $rawPath -ProbeTimeoutSeconds $TimeoutSeconds
        if (Test-Path -LiteralPath $rawPath -PathType Leaf) {
            try {
                $probe = Get-Content -LiteralPath $rawPath -Raw -Encoding utf8 | ConvertFrom-Json
            } catch {
                $parseDiagnostic = $_.Exception.Message
            }
        }
    }

    $importable = $false
    if ($null -ne $probe) {
        $importable = $probe.mlt_ok -eq $true -and
                      $probe.mlt.has_video -eq $true -and
                      $probe.mlt.is_unbounded_length -ne $true -and
                      [long]$probe.mlt.frame_count -gt 0 -and
                      [long]$probe.mlt.fps_num -gt 0 -and
                      [long]$probe.mlt.fps_den -gt 0
    }

    $status = if (-not $run.completed) {
        'TIMEOUT'
    } elseif ($null -eq $probe) {
        'PROBE_ERROR'
    } elseif (-not $importable) {
        'NOT_IMPORTABLE'
    } elseif ($run.exit_code -eq 0) {
        'IMPORTABLE'
    } else {
        # controller の受理条件は満たすが、MLT と ffprobe の解釈に差がある。
        'IMPORTABLE_WITH_PROBE_MISMATCH'
    }

    $records.Add([pscustomobject][ordered]@{
        path = $file.FullName
        extension = $file.Extension.ToLowerInvariant()
        size_bytes = $file.Length
        status = $status
        probe_exit_code = $run.exit_code
        container = if ($null -ne $probe) { [string]$probe.ffprobe.container } else { '' }
        video_codec = if ($null -ne $probe) { [string]$probe.ffprobe.video_codec } else { '' }
        audio_codec = if ($null -ne $probe) { [string]$probe.ffprobe.audio_codec } else { '' }
        pixel_format = if ($null -ne $probe) { [string]$probe.ffprobe.pix_fmt } else { '' }
        width = if ($null -ne $probe) { [long]$probe.ffprobe.width } else { 0 }
        height = if ($null -ne $probe) { [long]$probe.ffprobe.height } else { 0 }
        raw_json = if (Test-Path -LiteralPath $rawPath) { $rawPath } else { '' }
        diagnostic = if ($parseDiagnostic) { $parseDiagnostic } else { $run.stderr }
    })
}
Write-Progress -Activity 'ローカル動画の互換性検査' -Completed

$statusSummary = @($records | Group-Object -Property status | Sort-Object -Property Name | ForEach-Object {
    [pscustomobject]@{ status = $_.Name; count = $_.Count }
})
$formatSummary = @($records | Group-Object -Property extension, container, video_codec, pixel_format, status |
    Sort-Object -Property Count -Descending | ForEach-Object {
        $sample = $_.Group[0]
        [pscustomobject][ordered]@{
            extension = $sample.extension
            container = $sample.container
            video_codec = $sample.video_codec
            pixel_format = $sample.pixel_format
            status = $sample.status
            count = $_.Count
        }
    })

$report = [ordered]@{
    schema = 'mvm-local-media-audit-1'
    generated_at = (Get-Date).ToString('o')
    authority = 'MLT import eligibility; D3D11VA preview capability is not tested'
    roots = $resolvedRoots
    candidate_extensions = $videoExtensions
    checked_count = $records.Count
    status_summary = $statusSummary
    format_summary = $formatSummary
    files = $records
}
$reportPath = Join-Path $outputPath 'report.json'
$csvPath = Join-Path $outputPath 'files.csv'
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding utf8
$records | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding utf8

$statusSummary | Format-Table -AutoSize
Write-Host "検査結果: $reportPath"
Write-Host "ファイル一覧: $csvPath"

if (@($records | Where-Object { $_.status -notlike 'IMPORTABLE*' }).Count -gt 0) {
    exit 3
}
exit 0
