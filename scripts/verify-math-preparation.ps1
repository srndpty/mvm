<#
.SYNOPSIS
    準備待機の対照と固定 3 回の同時実行群を、電源前提を維持して記録する。
#>
[CmdletBinding()]
param(
    [ValidateSet('Focused', 'Concurrent')][string]$Mode = 'Focused',
    [string]$CohortPath = (Join-Path $PSScriptRoot '../tests/fixtures/math-preparation-cohort.json')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'lib/test-display-lease.ps1')
. (Join-Path $PSScriptRoot 'lib/math-preparation-cohort.ps1')
# 入力不備は isolated baseline より前に検出する。
$cohortNames = if ($Mode -eq 'Concurrent') { @(Get-MvmMathPreparationCohort -Path $CohortPath) } else { @() }
$destination = Join-Path $repo ('build/math-preparation-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + $Mode)
New-Item -ItemType Directory -Path $destination | Out-Null
$sourceRecords = @()
foreach ($source in (& git -C $repo ls-files --cached --others --exclude-standard 'src' 'apps' 'tests' 'scripts' 'cmake' 'CMakeLists.txt' 'CMakePresets.json')) {
    $target = Join-Path $destination "sources/$source"
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo $source) -Destination $target
    $sourceRecords += [ordered]@{ path = $source; sha256 = (Get-FileHash $target).Hash }
}
$sourceRecords | ConvertTo-Json | Set-Content (Join-Path $destination 'source-hashes.json') -Encoding utf8
& git -C $repo rev-parse HEAD | Set-Content (Join-Path $destination 'head.txt')
& git -C $repo diff --binary | Set-Content (Join-Path $destination 'source.patch') -Encoding utf8
$build = Join-Path $repo 'build/ucrt64-release'
$previousLog = Join-Path $build 'Testing/Temporary/LastTest.log'
if (Test-Path -LiteralPath $previousLog) {
    Copy-Item -LiteralPath $previousLog -Destination (Join-Path $destination 'previous-LastTest.log')
} else {
    '前回の LastTest.log はありません。未実行の build から検証を開始します。' |
        Set-Content (Join-Path $destination 'previous-LastTest-absent.txt') -Encoding utf8
}
Copy-Item (Join-Path $build 'CMakeCache.txt') $destination
$runtime = @()
foreach ($directory in @((Join-Path $build 'bin'), 'C:/msys64/ucrt64/bin')) {
    foreach ($file in Get-ChildItem $directory -File | Where-Object { $_.Extension -eq '.dll' -or $_.Name -eq 'mvm_test_math_controller.exe' }) {
        $runtime += [ordered]@{ path = $file.FullName; sha256 = (Get-FileHash $file.FullName).Hash }
    }
}
$runtime | ConvertTo-Json | Set-Content (Join-Path $destination 'runtime-hashes.json') -Encoding utf8
Write-Host "証拠: $destination"
Write-Host '【操作可】通常の背面 GUI 試験です。PC 操作を続けられます。'
$lease = Start-MvmTestDisplayLease
$ctest = 'C:/msys64/ucrt64/bin/ctest.exe'
$results = @()
function Invoke-RecordedCase([string]$Name, [string]$Pattern, [int]$Jobs, [string[]]$ExpectedNames = @()) {
    $lease.AssertValid()
    $list = & $ctest --test-dir $build -R $Pattern -LE 'performance|stability' --show-only=json-v1
    if ($LASTEXITCODE -ne 0 -or @(($list | ConvertFrom-Json).tests).Count -eq 0) { throw '試験集合が空または取得失敗です' }
    $actualNames = @(($list | ConvertFrom-Json).tests | ForEach-Object name)
    if ($ExpectedNames.Count -gt 0) {
        Assert-MvmMathPreparationCohortSelection -Expected $ExpectedNames -Actual $actualNames
    }
    $list | Set-Content (Join-Path $destination "$Name.tests.json")
    & $ctest --test-dir $build -R $Pattern -LE 'performance|stability' -j $Jobs --timeout 120 -VV 2>&1 |
        Tee-Object -FilePath (Join-Path $destination "$Name.log") | Out-Null
    $code = $LASTEXITCODE
    Copy-Item (Join-Path $build 'Testing/Temporary/LastTest.log') (Join-Path $destination "$Name.LastTest.log")
    $script:results += [ordered]@{ name = $Name; exit_code = $code }
    $script:results | ConvertTo-Json | Set-Content (Join-Path $destination 'results.json')
    $lease.AssertValid()
    Write-Host "$Name : exit $code"
    return $code
}
try {
    # 最終ソースの isolated baseline を先に判定する。FAIL の場合 cohort は開始しない。
    if ((Invoke-RecordedCase 'isolated' '^math_transform_native_playback$' 1) -ne 0) { throw 'isolated baseline が失敗しました' }
    if ($Mode -eq 'Focused') {
        $code = Invoke-RecordedCase 'controls' '^math_transform_preparation_' 1
        if ($code -ne 0) { throw '故障対照または準備待機契約が失敗しました' }
    } else {
        Copy-Item -LiteralPath $CohortPath -Destination (Join-Path $destination 'cohort.json')
        $pattern = '^(' + (($cohortNames | Sort-Object | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$'
        $failed = $false
        foreach ($run in 1..3) {
            if ((Invoke-RecordedCase "parallel-$run" $pattern 8 $cohortNames) -ne 0) { $failed = $true }
        }
        if ($failed) { throw '固定 3 回の cohort に FAIL があります。すべて保存しました' }
    }
    & (Join-Path $PSScriptRoot 'summarize-math-preparation.ps1') -EvidenceDirectory $destination
} finally {
    $lease.Dispose()
}
