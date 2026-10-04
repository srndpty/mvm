param([Parameter(Mandatory)][string]$WorkDir)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$PSNativeCommandUseErrorActionPreference = $false
$tool = Join-Path $PSScriptRoot '../../scripts/summarize-audio-adjustment-benchmark.ps1'
$runnerCommand = Get-Command pwsh -ErrorAction SilentlyContinue
$runner = if ($runnerCommand) { $runnerCommand.Source } else { Join-Path $env:ProgramFiles 'PowerShell/7/pwsh.exe' }
if (-not (Test-Path -LiteralPath $runner)) { throw '検査に必要な PowerShell 7 がありません' }
New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
function Write-Runs([string]$Path, [int]$ElapsedMs) {
    $lines = foreach ($runNumber in 1..3) {
        @{run = $runNumber; clips = 2; secondsPerClip = 3600; elapsedMs = $ElapsedMs;
          voiceLufs = -21.07; voicePeakDb = -21.074; ranges = 1} | ConvertTo-Json -Compress
    }
    [IO.File]::WriteAllLines($Path, [string[]]$lines)
}
$beforePath = Join-Path $WorkDir 'before.jsonl'
$afterPath = Join-Path $WorkDir 'after.jsonl'
Write-Runs $beforePath 100
Write-Runs $afterPath 10
$reportPath = Join-Path $WorkDir 'good.md'
$null = & $runner -NoProfile -File $tool -Before $beforePath -After $afterPath -Report $reportPath 2>&1
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $reportPath)) { throw '対照群の集計が失敗しました' }
foreach ($caseName in @('zero', 'missing', 'count', 'mismatch', 'nonfinite')) {
    Write-Runs $afterPath 10
    $lines = @(Get-Content -LiteralPath $afterPath)
    $first = $lines[0] | ConvertFrom-Json
    switch ($caseName) {
        'zero' { $first.elapsedMs = 0 }
        'missing' { $first.PSObject.Properties.Remove('voicePeakDb') }
        'count' { $lines = $lines[0..1] }
        'mismatch' { $first.voiceLufs = -22 }
        'nonfinite' { $first.elapsedMs = 'NaN' }
    }
    $lines[0] = $first | ConvertTo-Json -Compress
    [IO.File]::WriteAllLines($afterPath, [string[]]$lines)
    $badReport = Join-Path $WorkDir ($caseName + '-' + [guid]::NewGuid().ToString('N') + '.md')
    $null = & $runner -NoProfile -File $tool -Before $beforePath -After $afterPath -Report $badReport 2>&1
    if ($LASTEXITCODE -ne 1 -or (Test-Path -LiteralPath $badReport)) { throw "不正な計測を成功にしました: $caseName" }
}
Write-Output '計測の対照群と 5 種の不正データの拒否を確認しました'
