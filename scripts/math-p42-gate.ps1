<#
.SYNOPSIS
    P4-2 の source snapshot と gate の全出力を新しい証拠 directory に保存する。
#>
[CmdletBinding()]
param(
    [ValidateSet('Focused', 'Real', 'Regressions', 'BuildIndependent', 'Lint', 'Release', 'Mutations', 'PublicationMutation')]
    [string]$Stage = 'Focused',
    [string]$EvidenceDirectory,
    [string]$Python = "$env:APPDATA/uv/tools/manim/Scripts/python.exe"
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$pwshExe = (Get-Process -Id $PID).Path
$gitTool = Get-Command git -ErrorAction SilentlyContinue
$gitExe = if ($gitTool) { $gitTool.Source } else { 'C:\Program Files\Git\cmd\git.exe' }
if (-not (Test-Path -LiteralPath $gitExe)) { throw 'git がありません' }
if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $repoRoot ('build/math-p42-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Stage)
}
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '証拠 directory は既存です。上書きしません' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidenceRoot = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$sources = @(& $gitExe -C $repoRoot ls-files --cached --others --exclude-standard src tests scripts cmake CMakeLists.txt)
$hashes = foreach ($relative in $sources) {
    $sourcePath = Join-Path $repoRoot $relative
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) { continue }
    $copyPath = Join-Path $evidenceRoot ('sources/' + $relative)
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $copyPath) -Force
    Copy-Item -LiteralPath $sourcePath -Destination $copyPath
    @{ path = $relative; sha256 = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash }
}
@{ revision = (& $gitExe -C $repoRoot rev-parse HEAD).Trim(); hashes = @($hashes) } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'source-state.json') -Encoding utf8NoBOM
$records = [System.Collections.Generic.List[object]]::new()
function Invoke-Recorded {
    param([string]$Name, [string]$Executable, [string[]]$Arguments)
    $logPath = Join-Path $evidenceRoot ($Name + '.log')
    & $Executable @Arguments 2>&1 | Tee-Object -FilePath $logPath | ForEach-Object { Write-Host $_ }
    $code = $LASTEXITCODE
    $records.Add([ordered]@{ name = $Name; command = $Executable; arguments = $Arguments; exit = $code; log = $logPath })
    $records | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'commands.json') -Encoding utf8NoBOM
    if ($code -ne 0) { throw "$Name の終了コードは $code です" }
}
Push-Location $repoRoot
try {
    switch ($Stage) {
        'Focused' {
            Invoke-Recorded 'build' $pwshExe @('scripts/build.ps1', '-Target', 'mvm_test_graph_render', '-ReuseConfigure')
            Invoke-Recorded 'presentation-artifact' 'build/ucrt64-release/bin/mvm_test_graph_render.exe' @((Join-Path $evidenceRoot 'artifacts'))
        }
        'Real' {
            Write-Host '【操作可】オフライン Manim raster 描画。desktop/presentation の計測は行いません。'
            Invoke-Recorded 'real-render' 'build/ucrt64-release/bin/mvm_test_graph_render.exe' @((Join-Path $evidenceRoot 'artifacts'), $Python, (Join-Path $repoRoot 'src/media/manim/graph_backend.py'))
        }
        'Mutations' { Invoke-Recorded 'mutations' $pwshExe @('scripts/math-p42-mutations.ps1', '-EvidenceDirectory', (Join-Path $evidenceRoot 'controls'), '-Python', $Python) }
        'PublicationMutation' { Invoke-Recorded 'publication-mutation' $pwshExe @('scripts/math-p421-mutation.ps1', '-EvidenceDirectory', (Join-Path $evidenceRoot 'controls')) }
        'Regressions' {
            Invoke-Recorded 'regressions' $pwshExe @('scripts/math-p41-gate.ps1', '-Stage', 'Regressions', '-EvidenceDirectory', (Join-Path $evidenceRoot 'p41'))
            foreach ($target in @('mvm_test_graph_numeric', 'mvm_test_graph_domain', 'mvm_test_manim_math_tex', 'mvm_test_manim_equation_sequence', 'mvm_test_math_render', 'mvm_test_math_raster_cache', 'mvm_test_process')) {
                Invoke-Recorded ('build-' + $target) $pwshExe @('scripts/build.ps1', '-Target', $target, '-ReuseConfigure')
            }
            $pattern = '^(graph_numeric|graph_domain|graph_controller_history|graph_render_presentation|graph_render_artifact|math_render_key_and_layout|math_raster_cache_focused|manim_math_tex_focused|manim_equation_sequence_focused|process_runner_focused)$'
            $ctestExe = 'C:\msys64\ucrt64\bin\ctest.exe'
            $listing = (& $ctestExe --test-dir 'build/ucrt64-release' -N -R $pattern) -join "`n"
            $selectionMatch = [regex]::Match($listing, 'Total Tests: ([1-9][0-9]*)')
            if ($LASTEXITCODE -ne 0 -or -not $selectionMatch.Success) { throw '回帰の対象が0件または列挙失敗です' }
            $listing | Set-Content -LiteralPath (Join-Path $evidenceRoot 'selection.log') -Encoding utf8NoBOM
            Invoke-Recorded 'graph-manim-artifact-process' $ctestExe @('--test-dir', 'build/ucrt64-release', '-R', $pattern, '--output-on-failure', '--timeout', '120')
            Copy-Item -LiteralPath 'build/ucrt64-release/Testing/Temporary/LastTest.log' -Destination (Join-Path $evidenceRoot 'ctest-raw.log')
        }
        'BuildIndependent' { Invoke-Recorded 'independent' $pwshExe @('scripts/test.ps1', '-Preset', 'ucrt64-release', '-Group', 'BuildIndependent') }
        'Lint' { Invoke-Recorded 'lint' $pwshExe @('scripts/lint.ps1') }
        'Release' { Invoke-Recorded 'release' $pwshExe @('scripts/test.ps1', '-Preset', 'ucrt64-release') }
    }
} finally { Pop-Location }
Write-Host "証拠: $evidenceRoot"
