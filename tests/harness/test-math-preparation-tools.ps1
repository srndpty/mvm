[CmdletBinding()]
param([Parameter(Mandatory)][string]$Scratch)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $repo 'scripts/lib/math-preparation-cohort.ps1')
$directory = Join-Path $Scratch ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$names = @(Get-MvmMathPreparationCohort -Path (Join-Path $repo 'tests/fixtures/math-preparation-cohort.json'))
if ($names.Count -ne 277) { throw '保存した historical cohort の件数が変わりました。' }
function Assert-Rejected([scriptblock]$Action, [string]$Expected) {
    $caught = $false
    try { & $Action | Out-Null } catch {
        if ($_.Exception.Message -notmatch $Expected) { throw }
        $caught = $true
    }
    if (!$caught) { throw "負の対照を拒否していません: $Expected" }
}
Assert-MvmMathPreparationCohortSelection -Expected $names -Actual $names
Assert-Rejected { Assert-MvmMathPreparationCohortSelection -Expected $names -Actual @($names | Select-Object -Skip 1) } '一致しません'
Assert-Rejected { Assert-MvmMathPreparationCohortSelection -Expected $names -Actual @($names + 'unexpected_test') } '一致しません'
foreach ($case in @(
    @{ schema = 2; tests = @('math_transform_native_playback'); error = '形式' },
    @{ schema = 1; tests = @(); error = '空または不正' },
    @{ schema = 1; tests = @('math_transform_native_playback', 'math_transform_native_playback'); error = '重複' },
    @{ schema = 1; tests = @('another_test'); error = '対象' })) {
    $path = Join-Path $directory 'invalid.json'
    $case | ConvertTo-Json | Set-Content $path -Encoding utf8
    Assert-Rejected { Get-MvmMathPreparationCohort -Path $path } $case.error
}
function Record([string]$Stage, [int]$Frame, [int]$Wall) {
    return "1: 準備記録: $Stage wall_us=$Wall cpu_100ns=$Wall thread=7 frame=$Frame bytes=1 success=1 cpu_available=1"
}
$good = @('    Start 1: math_transform_native_playback', (Record 'wait-start' -1 0),
    (Record 'backend-start' -1 10), (Record 'backend-end' -1 20))
foreach ($stage in @('decode', 'extract', 'persist', 'hash', 'manifest-frame')) {
    foreach ($frame in 0..179) {
        $good += Record "$stage-start" $frame (30 + $frame * 2)
        $good += Record "$stage-end" $frame (31 + $frame * 2)
    }
}
$good += Record 'worker-ready' -1 500
$good += Record 'gui-ready' -1 600
$log = Join-Path $directory 'isolated.log'
$good | Set-Content $log -Encoding utf8
$summary = Join-Path $repo 'scripts/summarize-math-preparation.ps1'
& $summary -EvidenceDirectory $directory
$row = Get-Content (Join-Path $directory 'preparation-summary.json') -Raw | ConvertFrom-Json
if ($row.stages.backend.count -ne 1 -or $row.stages.backend.wall_us -ne 10 -or
    $row.stages.backend.cpu_100ns -ne 10 -or @($row.incomplete_stages | Where-Object { $_ -match '/backend$' }).Count -ne 0) {
    throw '正常な backend 区間の wall / CPU 会計が不正です。'
}
$good -replace 'backend-end(.+)frame=-1', 'backend-end$1frame=180' | Set-Content $log -Encoding utf8
Assert-Rejected { & $summary -EvidenceDirectory $directory } 'backend 区間会計'
$good | Where-Object { $_ -notmatch 'backend-end' } | Set-Content $log -Encoding utf8
Assert-Rejected { & $summary -EvidenceDirectory $directory } 'backend 区間会計'
Write-Host 'cohort 入力と backend 段階会計の正常・負の対照が通過しました。'
