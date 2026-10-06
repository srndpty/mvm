# P3-3 の検査が空振りでないことを、build 配下の複製へ入れた変異で確かめる。製品 source は変えない。
# 変異ごとに対象の試験を作り直し、終了コード 1 と対象の検査の失敗メッセージを照合する。
# クラッシュ・timeout・compile error は検出に数えない。
[CmdletBinding()]
param([string]$Preset = 'ucrt64-release')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build/$Preset"
$compiler = 'C:/msys64/ucrt64/bin/g++.exe'
$env:PATH = "C:/msys64/ucrt64/bin;$env:PATH"
$outputDir = Join-Path $buildDir ("equation-renderer-mutations-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
[void](New-Item -ItemType Directory -Path $outputDir)

$backendObjects = @(
    (Join-Path $buildDir 'tests/CMakeFiles/mvm_test_manim_equation_sequence.dir/core/test_manim_equation_sequence.cpp.obj'),
    (Join-Path $buildDir 'tests/CMakeFiles/mvm_test_manim_equation_sequence.dir/mvm_test_manim_equation_sequence_autogen/mocs_compilation.cpp.obj'))
$cacheObjects = @(
    (Join-Path $buildDir 'tests/CMakeFiles/mvm_test_math_raster_cache.dir/harness/test_math_raster_cache.cpp.obj'),
    (Join-Path $buildDir 'tests/CMakeFiles/mvm_test_math_raster_cache.dir/mvm_test_math_raster_cache_autogen/mocs_compilation.cpp.obj'))
foreach ($object in $backendObjects + $cacheObjects) {
    if (-not (Test-Path -LiteralPath $object)) {
        throw "先に scripts/build.ps1 で mvm_test_manim_equation_sequence と mvm_test_math_raster_cache を作ってください: $object"
    }
}
$fakeManim = Join-Path $buildDir 'bin/mvm_fake_math_tex_cli.exe'
$windowsLibraries = @('-lshell32', '-lbcrypt', '-lole32', '-lkernel32', '-luser32', '-lgdi32',
    '-lwinspool', '-loleaut32', '-luuid', '-lcomdlg32', '-ladvapi32')
$backendLibraries = @((Join-Path $buildDir 'src/libmvm_manim.a'), (Join-Path $buildDir 'src/libmvm_util.a'),
    (Join-Path $buildDir 'src/libmvm_math.a'), (Join-Path $buildDir 'src/libmvm_util.a')) + $windowsLibraries
$cacheLibraries = @((Join-Path $buildDir 'apps/mvm/libmvm_math_raster_cache.a'),
    (Join-Path $buildDir 'src/libmvm_util.a'), 'C:/msys64/ucrt64/lib/libQt6Gui.dll.a',
    (Join-Path $buildDir 'src/libmvm_math_clip_render.a'), (Join-Path $buildDir 'src/libmvm_math.a'),
    (Join-Path $buildDir 'src/libmvm_still_image.a'), 'C:/msys64/ucrt64/lib/libavformat.dll.a',
    'C:/msys64/ucrt64/lib/libavcodec.dll.a', 'C:/msys64/ucrt64/lib/libswresample.dll.a',
    'C:/msys64/ucrt64/lib/libavutil.dll.a', 'C:/msys64/ucrt64/lib/libswscale.dll.a',
    (Join-Path $buildDir 'src/libmvm_project.a'), (Join-Path $buildDir 'src/libmvm_util.a'),
    (Join-Path $buildDir 'src/libmvm_core.a'), 'C:/msys64/ucrt64/lib/libQt6Core.dll.a',
    '-lmpr', '-luserenv', '-ld3d11', '-ldxgi', '-ldxguid', '-ld3d12') + $windowsLibraries

$backend = @{
    File = 'src/media/manim/manim_equation_sequence.cpp'; Objects = $backendObjects; Libraries = $backendLibraries
    Include = @('-I', (Join-Path $repoRoot 'src'))
    Arguments = { param($io) @($fakeManim, $io, 'C:/msys64/ucrt64/bin') }
    Timeout = 120000
}
$cache = @{
    File = 'apps/mvm/math_equation_sequence_artifact.cpp'; Objects = $cacheObjects; Libraries = $cacheLibraries
    Include = @('-I', (Join-Path $repoRoot 'src'), '-I', (Join-Path $repoRoot 'apps/mvm'))
    Arguments = { param($io) @($io) }
    Timeout = 300000
}
$mutations = @(
    @{ Name = 'shared_descendant_ignored'; Target = $backend
       From = 'if (claimed[static_cast<std::size_t>(index->second)]++)'
       To = 'if (claimed[static_cast<std::size_t>(index->second)]++ > 1)'
       Expected = '拒否: 同じ子孫を 2 つの handle が所有' },
    @{ Name = 'unclaimed_glyph_ignored'; Target = $backend
       From = 'if (!claimed[at2])'; To = 'if (false)'
       Expected = '拒否: どの handle にも属さない glyph' },
    @{ Name = 'empty_target_accepted'; Target = $backend
       From = 'if (!ownershipOf(action.state, action.segment).nonEmpty)'; To = 'if (false)'
       Expected = 'action の対象が空なら EmptyActionTarget' },
    @{ Name = 'render_before_validation'; Target = $backend
       From = 'if (!first.ready()) {'; To = 'if (false) {'
       Expected = 'FAKE_EQ_SHARED: 起動した段階' },
    @{ Name = 'phase_comparison_removed'; Target = $backend
       From = 'if (!second.ready() || second.segments != first.segments) {'; To = 'if (!second.ready()) {'
       Expected = 'FAKE_EQ_PHASE_DIFF' },
    @{ Name = 'frame_count_unchecked'; Target = $backend
       From = 'countPngs(folder) != expected'; To = 'false'
       Expected = 'FAKE_EQ_SHORT' },
    @{ Name = 'static_equivalence_unchecked'; Target = $backend
       From = 'frame.coverage, request.stateStatics[s], place.placement.left, place.placement.top);'
       To = 'frame.coverage, frame.coverage, 0, 0);'
       Expected = 'FAKE_EQ_STATIC' },
    @{ Name = 'transition_endpoint_unchecked'; Target = $backend
       From = 'if (index == 0 || last) {'; To = 'if (false) {'
       Expected = 'FAKE_EQ_ENDPOINT' },
    @{ Name = 'action_mutation_unchecked'; Target = $backend
       From = 'after.coverage, still, place.placement.left, place.placement.top);'
       To = 'after.coverage, after.coverage, 0, 0);'
       Expected = 'FAKE_EQ_AFTER' },
    # P3-3.1: pulse の base と通常の対象の合成による静止の分解の照合を外す。
    @{ Name = 'pulse_reconstruction_removed_backend'; Target = $backend
       From = '? math::mathEndpointDifference(composed, still, place.placement.left,'
       To = '? 0 * math::mathEndpointDifference(composed, still, place.placement.left,'
       Expected = 'FAKE_EQ_PULSE_FULL' },
    @{ Name = 'pulse_reconstruction_removed_publication'; Target = $cache
       From = 'composed, job.stateStatics[job.spec.actions[a].state], r.ax, r.ay)'
       To = 'composed, composed, 0, 0)'
       Expected = 'pulse の base が対象を含んだままなら Failed' },
    @{ Name = 'provenance_identity_ignored'; Target = $cache
       From = 'provenanceText(job, colors, parsed) == text;'; To = 'true;'
       Expected = '合成の色を書き換えた provenanceは使わず描き直す' },
    @{ Name = 'frame_hash_unchecked'; Target = $cache
       From = 'if (sha256Hex(read.data(), read.size()) != frame.sha256) {'; To = 'if (false) {'
       Expected = '中身の壊れた frame は検査し直しで拒否する' },
    @{ Name = 'publish_without_geometry'; Target = $cache
       From = 'if (!shape || !geometryValid(job, p)) {'; To = 'if (!shape) {'
       Expected = 'backend が Ok でも枚数が違えば Failed' },
    @{ Name = 'publish_ignores_cancel'; Target = $cache
       From = "std::lock_guard lock(*job.publishGate);`n    if (cancel->load())"
       To = "std::lock_guard lock(*job.publishGate);`n    if (false)"
       Expected = 'provenance を書く直前に権限を失えば確定しない' }
)
$killed = 0
foreach ($mutation in $mutations) {
    $target = $mutation.Target
    $original = [IO.File]::ReadAllText((Join-Path $repoRoot $target.File)).Replace("`r`n", "`n")
    if (-not $original.Contains($mutation.From)) { throw "変異箇所がありません: $($mutation.Name)" }
    $mutant = $original.Replace($mutation.From, $mutation.To)
    $source = Join-Path $outputDir "$($mutation.Name).cpp"
    $binary = Join-Path $outputDir "$($mutation.Name).exe"
    [IO.File]::WriteAllText($source, $mutant, [Text.UTF8Encoding]::new($false))
    & $compiler '-std=c++20' '-O2' @($target.Include) '-isystem' 'C:/msys64/ucrt64/include/qt6' `
        '-isystem' 'C:/msys64/ucrt64/include/qt6/QtCore' $source @($target.Objects) @($target.Libraries) '-o' $binary
    if ($LASTEXITCODE -ne 0) { throw "変異のコンパイルが失敗しました: $($mutation.Name)" }
    $startInfo = [Diagnostics.ProcessStartInfo]::new($binary)
    # 試験の作業 directory は短い名前にする (長い変異名で cache の作業 path が 260 文字を超えない)。
    $io = Join-Path $outputDir ("io-" + $killed)
    foreach ($argument in (& $target.Arguments $io)) { $startInfo.ArgumentList.Add($argument) }
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($startInfo)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($target.Timeout)) { $process.Kill($true); throw "変異が timeout しました: $($mutation.Name)" }
    $log = $stdoutTask.GetAwaiter().GetResult() + $stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $outputDir "$($mutation.Name).log"), $log, [Text.UTF8Encoding]::new($false))
    if ($process.ExitCode -ne 1 -or -not $log.Contains($mutation.Expected)) {
        throw "変異を対象の検査で検出できません: $($mutation.Name) / 終了 $($process.ExitCode)"
    }
    ++$killed
    Write-Host "変異検出: $($mutation.Name)"
}
Write-Host "$killed / $($mutations.Count) 変異検出。証拠: $outputDir"
