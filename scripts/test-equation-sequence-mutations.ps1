# P3-1 の純粋実装の変異試験。製品 source は変更せず build 配下の複製だけをコンパイルする。
[CmdletBinding()]
param([string]$Preset = 'ucrt64-release')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build/$Preset"
$compiler = 'C:/msys64/ucrt64/bin/g++.exe'
$env:PATH = "C:/msys64/ucrt64/bin;$env:PATH"
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outputDir = Join-Path $buildDir "equation-mutations-$stamp"
[void](New-Item -ItemType Directory -Path $outputDir)
$testObject = Join-Path $buildDir 'tests/CMakeFiles/mvm_test_equation_sequence.dir/harness/test_equation_sequence.cpp.obj'
if (-not (Test-Path -LiteralPath $testObject)) { throw '先に scripts/build.ps1 -Target mvm_test_equation_sequence を実行してください' }
$mutations = @(
    @{ Name = 'source_count_only'; File = 'project/equation_sequence_edit.cpp'; From = 'if (wasFullTail)'; To = 'if (false)' },
    @{ Name = 'inclusive_end'; File = 'project/equation_sequence.cpp'; From = 'frame >= interval.end'; To = 'frame > interval.end' },
    @{ Name = 'progress_next'; File = 'project/equation_sequence.cpp'; From = 'result.progressNumerator = result.localFrame;'; To = 'result.progressNumerator = result.localFrame + 1;' },
    @{ Name = 'retained_action'; File = 'project/equation_sequence.cpp'; From = 'std::erase_if(c.actions, [&](const auto& a) { return a.state == id; });'; To = '/* 状態を削除しても action を残す変異 */' },
    @{ Name = 'floating_boundary'; File = 'core/source_frame_mapping.cpp'; From = 'return toInt64(roundUp ? divideCeil(numerator, ratio->den)'; To = 'if (!roundUp) return static_cast<std::int64_t>(static_cast<double>(numerator) / static_cast<double>(ratio->den)); return toInt64(roundUp ? divideCeil(numerator, ratio->den)' },
    @{ Name = 'text_rebinding'; File = 'project/equation_sequence.cpp'; From = 'a.targetStatus = EquationTargetStatus::Missing;'; To = 'a.target = c.states[*i].parts.empty() ? PartId{"incorrect"} : c.states[*i].parts[0].id;' }
)
$compileArguments = @('-std=c++20', '-O2', '-I', (Join-Path $repoRoot 'src'))
$libraries = @((Join-Path $buildDir 'src/libmvm_project.a'), (Join-Path $buildDir 'src/libmvm_util.a'),
    (Join-Path $buildDir 'src/libmvm_core.a'), '-lshell32', '-lole32', '-lbcrypt', '-lkernel32', '-luser32')
$killed = 0
foreach ($mutation in $mutations) {
    $original = [IO.File]::ReadAllText((Join-Path $repoRoot "src/$($mutation.File)"))
    if (-not $original.Contains($mutation.From)) { throw "変異箇所がありません: $($mutation.Name)" }
    $mutant = $original.Replace($mutation.From, $mutation.To)
    $source = Join-Path $outputDir "$($mutation.Name).cpp"
    $binary = Join-Path $outputDir "$($mutation.Name).exe"
    [IO.File]::WriteAllText($source, $mutant, [Text.UTF8Encoding]::new($false))
    & $compiler @compileArguments $source $testObject @libraries '-o' $binary
    if ($LASTEXITCODE -ne 0) { throw "変異のコンパイルが失敗しました: $($mutation.Name)" }
    $startInfo = [Diagnostics.ProcessStartInfo]::new($binary)
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($startInfo)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) { $process.Kill($true); throw "変異が timeout しました: $($mutation.Name)" }
    $log = $stdoutTask.GetAwaiter().GetResult() + $stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $outputDir "$($mutation.Name).log"), $log, [Text.UTF8Encoding]::new($false))
    # 検査による終了 1 だけを kill とする。クラッシュ・timeout は検出証拠に数えない。
    if ($process.ExitCode -ne 1 -or $log -notmatch '失敗:') { throw "変異を検査で検出できません: $($mutation.Name) / 終了 $($process.ExitCode)" }
    ++$killed
    Write-Host "変異検出: $($mutation.Name)"
}
Write-Host "$killed / $($mutations.Count) 変異検出。証拠: $outputDir"
