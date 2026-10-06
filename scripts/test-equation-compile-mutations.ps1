# P3-2 の変異は build 配下の複製だけに入れ、製品 source を保持する。
[CmdletBinding()]
param([string]$Preset = 'ucrt64-release')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build/$Preset"
$compiler = 'C:/msys64/ucrt64/bin/g++.exe'
$env:PATH = "C:/msys64/ucrt64/bin;$env:PATH"
$outputDir = Join-Path $buildDir ("equation-compile-mutations-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
[void](New-Item -ItemType Directory -Path $outputDir)
$testObject = Join-Path $buildDir 'tests/CMakeFiles/mvm_test_equation_compile.dir/harness/test_equation_compile.cpp.obj'
if (-not (Test-Path -LiteralPath $testObject)) { throw '先に scripts/build.ps1 -Target mvm_test_equation_compile を実行してください' }
$mutations = @(
    @{ Name = 'equal_text_rebinding'; File = 'project/equation_sequence.cpp'; From = 'p.binding.status = BindingStatus::Invalid;'; To = '{ p.binding.status = BindingStatus::Bound; p.binding.revision = revision; }'; Expected = '文字一致で自動再 binding しない' },
    @{ Name = 'begin_insertion_inside'; File = 'project/equation_binding_edit.cpp'; From = '*end <= static_cast<std::size_t>(b.begin)'; To = '*end < static_cast<std::size_t>(b.begin)'; Expected = '境界の手計算 status' },
    @{ Name = 'auto_crosses_semantic'; File = 'app/equation_sequence_compile.cpp'; From = 'autoRegion(begin);'; To = 'autoRegion(end);'; Expected = '原 byte を一回ずつ再構成' },
    @{ Name = 'explicit_double_matching'; File = 'app/equation_sequence_compile.cpp'; From = 'usedFrom[*a] = usedTo[*b] = true;'; To = 'usedFrom[*a] = usedTo[*b] = false;'; Expected = '一つの segment を二度 transform しない' },
    @{ Name = 'canonical_raw_part_id'; File = 'app/equation_sequence_compile.cpp'; From = 'auto matchKey = p->binding.expectedText;'; To = 'auto matchKey = p->binding.expectedText + p->id.value;'; Expected = '正準 plan に依存しない' }
)
$compileArguments = @('-std=c++20', '-O2', '-I', (Join-Path $repoRoot 'src'))
$libraries = @((Join-Path $buildDir 'src/libmvm_equation_sequence_compile.a'),
    (Join-Path $buildDir 'src/libmvm_project.a'), (Join-Path $buildDir 'src/libmvm_math.a'),
    (Join-Path $buildDir 'src/libmvm_util.a'), (Join-Path $buildDir 'src/libmvm_core.a'),
    '-lshell32', '-lole32', '-lbcrypt', '-lkernel32', '-luser32')
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
    $startInfo.ArgumentList.Add((Join-Path $outputDir "$($mutation.Name)-io"))
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
    # クラッシュ・timeout・compile error は変異検出に数えない。
    if ($process.ExitCode -ne 1 -or -not $log.Contains($mutation.Expected)) { throw "変異を対象の検査で検出できません: $($mutation.Name) / 終了 $($process.ExitCode)" }
    ++$killed
    Write-Host "変異検出: $($mutation.Name)"
}
Write-Host "$killed / $($mutations.Count) 変異検出。証拠: $outputDir"
