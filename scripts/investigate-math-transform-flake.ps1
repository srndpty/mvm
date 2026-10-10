<#
.SYNOPSIS
    初回 FAIL の証拠を保存し、同時実行された CTest 群と診断結果を記録する。
#>
[CmdletBinding()]
param([switch]$Run)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$historical = Join-Path $repo 'build/review-evidence-2026-10-10'
$destination = Join-Path $repo ('build/math-transform-flake-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $destination | Out-Null
$records = @()
foreach ($file in Get-ChildItem $historical -File) {
    Copy-Item -LiteralPath $file.FullName -Destination $destination
    $records += [ordered]@{ path = $file.FullName; sha256 = (Get-FileHash $file.FullName).Hash }
}
$temporary = Join-Path $repo 'build/ucrt64-release/Testing/Temporary'
foreach ($file in Get-ChildItem $temporary -File) {
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $destination ('pre-' + $file.Name))
    $records += [ordered]@{ path = $file.FullName; sha256 = (Get-FileHash $file.FullName).Hash }
}
$records | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $destination 'historical-hashes.json') -Encoding utf8
& git -C $repo rev-parse HEAD | Set-Content (Join-Path $destination 'head.txt')
& git -C $repo diff --binary | Set-Content (Join-Path $destination 'source.patch') -Encoding utf8
New-Item -ItemType Directory -Path (Join-Path $destination 'sources') | Out-Null
foreach ($source in @('tests/harness/test_math_controller.cpp', 'tests/harness/math_fake_backend.h',
                      'tests/harness/test_mvm_controller_export.cpp', 'apps/mvm/mvm_controller.cpp',
                      'tests/harness/test_mvm_controller_project_io.cpp',
                      'tests/harness/test_mvm_controller_fixture.h',
                      'apps/mvm/mvm_controller_detail.h', 'apps/mvm/mvm_controller_detail.cpp',
                      'apps/mvm/mvm_controller_export.cpp', 'apps/mvm/mvm_controller_effects.cpp',
                      'apps/mvm/mvm_controller_media.cpp', 'apps/mvm/mvm_controller_timeline_edit.cpp',
                      'apps/mvm/mvm_controller_project_io.cpp',
                      'cmake/mvm_controller.cmake',
                      'apps/mvm/math_raster_cache.cpp', 'tests/CMakeLists.txt', 'scripts/test.ps1',
                      'scripts/investigate-math-transform-flake.ps1')) {
    Copy-Item -LiteralPath (Join-Path $repo $source) -Destination (Join-Path $destination 'sources')
}
# tests の CMakeLists.txt と同名なので、製品側の定義は別名で保存する。
Copy-Item -LiteralPath (Join-Path $repo 'apps/mvm/CMakeLists.txt') `
    -Destination (Join-Path $destination 'sources/apps-mvm-CMakeLists.txt')
# console は完了順なので、Start/完了の区間から同時に active だった集合を復元する。
$active = @{}
$cohort = @{}
$inside = $false
$events = @()
foreach ($line in Get-Content (Join-Path $historical 'gate-final-both-all.log')) {
    if ($line -match '^=== ucrt64-debug ===') { break }
    if ($line -match '^\s*Start\s+(\d+):\s+(\S+)') {
        $testId = $Matches[1]
        $testName = $Matches[2]
        $active[$testId] = $testName
        if ($testName -eq 'math_transform_native_playback') {
            $inside = $true
            foreach ($key in $active.Keys) { $cohort[$key] = $active[$key] }
        }
        if ($inside) { $cohort[$testId] = $testName }
    }
    if ($inside) { $events += $line }
    if ($line -match '^\s*\d+/\d+ Test\s+#(\d+):') {
        $testId = $Matches[1]
        if ($active[$testId] -eq 'math_transform_native_playback') { $inside = $false }
        $active.Remove($testId)
    }
}
$events | Set-Content (Join-Path $destination 'historical-overlap.log') -Encoding utf8
$cohort | ConvertTo-Json | Set-Content (Join-Path $destination 'cohort.json') -Encoding utf8
if ($cohort.Count -eq 0 -or $events.Count -eq 0) { throw '初回 FAIL の同時実行群を復元できません' }
Write-Host "証拠の保存先: $destination"
if (!$Run) { exit 0 }
Write-Host '【操作可】通常の背面 GUI 試験です。PC 操作を続けられます。'
. (Join-Path $PSScriptRoot 'lib/test-display-lease.ps1')
$displayLease = Start-MvmTestDisplayLease
$ctest = 'C:/msys64/ucrt64/bin/ctest.exe'
$build = Join-Path $repo 'build/ucrt64-release'
$env:MVM_TEST_TRANSFORM_TRACE = '1'
try {
    $testMetadata = & $ctest --test-dir $build --show-only=json-v1
    $testMetadata | Set-Content (Join-Path $destination 'ctest-tests.json') -Encoding utf8
    foreach ($binary in @('mvm_test_math_controller.exe', 'mvm_test_controller_export.exe',
                          'mvm_test_controller_project_io.exe')) {
        Get-FileHash (Join-Path $build "bin/$binary") | ConvertTo-Json |
            Set-Content (Join-Path $destination "$binary.hash.json")
    }
    $pattern = '^(' + (($cohort.Values | Sort-Object) -join '|') + ')$'
    $results = @()
    foreach ($case in @('isolated', 'parallel-1', 'parallel-2', 'parallel-3', 'delayed-publish', 'selection')) {
        $displayLease.AssertValid()
        $filter = if ($case -like 'parallel-*') { $pattern }
                  elseif ($case -eq 'selection') { '^m7b_4_selection_identity$' }
                  else { '^math_transform_native_playback$' }
        if ($case -eq 'delayed-publish') { $env:MVM_TEST_TRANSFORM_DELAY_PUBLISH = '1' }
        else { Remove-Item Env:MVM_TEST_TRANSFORM_DELAY_PUBLISH -ErrorAction SilentlyContinue }
        & $ctest --test-dir $build -R $filter -LE 'performance|stability' -j 8 --timeout 120 -VV 2>&1 |
            Tee-Object -FilePath (Join-Path $destination "$case.log") | Out-Null
        $code = $LASTEXITCODE
        $results += [ordered]@{ case = $case; exit_code = $code }
        Copy-Item (Join-Path $temporary 'LastTest.log') (Join-Path $destination "$case.LastTest.log")
        $results | ConvertTo-Json | Set-Content (Join-Path $destination 'results.json')
        Write-Host "$case : exit $code"
    }
    $positiveLog = Get-Content (Join-Path $destination 'delayed-publish.log') -Raw
    if ($results[4].exit_code -ne 0 -or
        $positiveLog -notmatch '準備結果: outcome=0 control=delayed' -or
        $positiveLog -match 'FAIL:') {
        throw '公開遅延の対照実験が正しい準備完了と再生を確認していません'
    }
    & (Join-Path $PSScriptRoot 'summarize-math-transform-flake.ps1') -EvidenceDirectory $destination
    if (@($results | Where-Object { $_.exit_code -ne 0 }).Count -gt 0) {
        throw "通常条件で FAIL を記録しました: $destination/results.json"
    }
} finally {
    $displayLease.Dispose()
    Remove-Item Env:MVM_TEST_TRANSFORM_TRACE -ErrorAction SilentlyContinue
    Remove-Item Env:MVM_TEST_TRANSFORM_DELAY_PUBLISH -ErrorAction SilentlyContinue
}
