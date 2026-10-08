<#
.SYNOPSIS
    P4-1 の検証を source snapshot と実行ログ付きで保存する。
#>
[CmdletBinding()]
param(
    [ValidateSet('Focused', 'Mutations', 'Regressions', 'BuildIndependent', 'Lint', 'Release')]
    [string]$Stage = 'Focused',
    [string]$EvidenceDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$ctestExe = 'C:\msys64\ucrt64\bin\ctest.exe'
$pwshExe = (Get-Process -Id $PID).Path
$gitCommand = Get-Command git -ErrorAction SilentlyContinue
$gitExe = if ($gitCommand) { $gitCommand.Source } else { 'C:\Program Files\Git\cmd\git.exe' }
if (-not (Test-Path -LiteralPath $gitExe)) { throw 'git がありません' }
if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $repoRoot ('build/math-p41-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Stage)
}
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '証拠 directory は既存です。上書きしません' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidenceRoot = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$sourceRoot = Join-Path $evidenceRoot 'sources'
$null = New-Item -ItemType Directory -Path $sourceRoot
$records = [System.Collections.Generic.List[object]]::new()
$revision = (& $gitExe -C $repoRoot rev-parse HEAD).Trim()
$sourceFiles = @(& $gitExe -C $repoRoot ls-files --cached --others --exclude-standard src apps tests scripts cmake CMakeLists.txt)
$sourceHashes = foreach ($relative in $sourceFiles) {
    $path = Join-Path $repoRoot $relative
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $copy = Join-Path $sourceRoot $relative
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $copy) -Force
    Copy-Item -LiteralPath $path -Destination $copy
    [ordered]@{ path = $relative; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
}
@{ revision = $revision; hashes = @($sourceHashes) } | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $evidenceRoot 'source-state.json') -Encoding utf8NoBOM
& $gitExe -C $repoRoot diff --binary | Set-Content -LiteralPath (Join-Path $evidenceRoot 'source.diff') -Encoding utf8NoBOM

function Invoke-RecordedCommand {
    param([string]$Name, [string]$Executable, [string[]]$Arguments, [int]$ExpectedExit = 0)
    $log = Join-Path $evidenceRoot ($Name + '.log')
    $command = $Executable + ' ' + ($Arguments -join ' ')
    Write-Host "実行: $command"
    & $Executable @Arguments 2>&1 | Tee-Object -FilePath $log | ForEach-Object { Write-Host $_ }
    $code = $LASTEXITCODE
    $records.Add([ordered]@{ name = $Name; command = $command; revision = $revision;
        exit = $code; expected_exit = $ExpectedExit; result = $(if ($code -eq $ExpectedExit) { 'PASS' } else { 'FAIL' }); log = $log })
    $records | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'commands.json') -Encoding utf8NoBOM
    if ($code -ne $ExpectedExit) { throw "$Name の終了コードは $code です (期待 $ExpectedExit)" }
}

function Invoke-FocusedCTest {
    param([string]$Name, [string]$Pattern, [int]$ExpectedExit = 0)
    $buildDir = Join-Path $repoRoot 'build/ucrt64-release'
    $listing = (& $ctestExe --test-dir $buildDir -N -R $Pattern) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $listing -notmatch 'Total Tests: ([1-9][0-9]*)') { throw '対象テストが 0 件または列挙失敗です' }
    $count = [int]$Matches[1]
    $listing | Set-Content -LiteralPath (Join-Path $evidenceRoot ($Name + '-selection.log')) -Encoding utf8NoBOM
    Invoke-RecordedCommand $Name $ctestExe @('--test-dir', $buildDir, '-R', $Pattern, '--output-on-failure', '--timeout', '120') $ExpectedExit
    $records[$records.Count - 1].test_count = $count
    $records | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'commands.json') -Encoding utf8NoBOM
    Copy-Item -LiteralPath (Join-Path $buildDir 'Testing/Temporary/LastTest.log') -Destination (Join-Path $evidenceRoot ($Name + '-raw.log'))
}

Push-Location $repoRoot
try {
    switch ($Stage) {
        'Focused' {
            foreach ($target in @('mvm_test_graph_domain', 'mvm_test_graph_numeric', 'mvm_test_math_controller')) {
                Invoke-RecordedCommand ('build-' + $target) $pwshExe @('scripts/build.ps1', '-Target', $target, '-ReuseConfigure')
            }
            Invoke-FocusedCTest 'domain-schema' '^graph_domain$'
            Invoke-FocusedCTest 'numeric-sampling-draw' '^graph_numeric$'
            Invoke-FocusedCTest 'controller-history' '^graph_controller_history$'
        }
        'Regressions' {
            foreach ($target in @('mvm_test_two_track_project', 'mvm_test_project_math', 'mvm_test_equation_sequence', 'mvm_test_equation_authoring', 'mvm_test_timeline_edit', 'mvm_test_equation_compile', 'mvm_test_equation_sequence_render')) {
                Invoke-RecordedCommand ('build-' + $target) $pwshExe @('scripts/build.ps1', '-Target', $target, '-ReuseConfigure')
            }
            Invoke-FocusedCTest 'project-regression' '^(m7b_1_two_track_project_focused|math_project_json_focused)$'
            Invoke-FocusedCTest 'math-equation-regression' '^(math_equation_sequence_domain|math_equation_sequence_authoring|math_equation_sequence_compile|math_equation_sequence_render_contract|math_equation_sequence_history|math_controller_focused|m5_timeline_edit_focused)$'
        }
        'BuildIndependent' { Invoke-RecordedCommand 'independent' $pwshExe @('scripts/test.ps1', '-Preset', 'ucrt64-release', '-Group', 'BuildIndependent') }
        'Lint' { Invoke-RecordedCommand 'lint' $pwshExe @('scripts/lint.ps1') }
        'Release' { Invoke-RecordedCommand 'release' $pwshExe @('scripts/test.ps1', '-Preset', 'ucrt64-release', '-Fast') }
        'Mutations' {
            & (Join-Path $PSScriptRoot 'math-p41-mutations.ps1') -EvidenceDirectory $evidenceRoot
            if ($LASTEXITCODE -ne 0) { throw '変異検査に失敗しました' }
        }
    }
} finally { Pop-Location }
Write-Host "証拠: $evidenceRoot"
