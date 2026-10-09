# Graph export の source と実行結果を、新規の証拠 directory に保存する。
[CmdletBinding()]
param([ValidateSet('Dependencies', 'Focused', 'Controller', 'Regressions', 'BuildIndependent', 'Lint', 'Release')]
      [string]$Stage = 'Focused', [string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$pwshExe = (Get-Process -Id $PID).Path
$gitTool = Get-Command git -ErrorAction SilentlyContinue
$gitExe = if ($gitTool) { $gitTool.Source } else { 'C:\Program Files\Git\cmd\git.exe' }
if (-not (Test-Path -LiteralPath $gitExe)) { throw 'git がありません' }
if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $repoRoot ('build/math-p45-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Stage)
}
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '既存の証拠を上書きしません' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidenceRoot = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$sources = @(& $gitExe -C $repoRoot ls-files --cached --others --exclude-standard src apps tests scripts cmake CMakeLists.txt)
$hashes = foreach ($relative in $sources) {
    $sourcePath = Join-Path $repoRoot $relative
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) { continue }
    $copyPath = Join-Path $evidenceRoot ('sources/' + $relative)
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $copyPath) -Force
    Copy-Item -LiteralPath $sourcePath -Destination $copyPath
    @{ path = $relative; sha256 = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash }
}
$sourceLines = @($hashes | Sort-Object path | ForEach-Object { $_.path + ':' + $_.sha256 }) -join "`n"
$sourceSha = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($sourceLines)))
@{ revision = (& $gitExe -C $repoRoot rev-parse HEAD).Trim(); sourceSha256 = $sourceSha; hashes = @($hashes) } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'source-state.json') -Encoding utf8NoBOM
$records = [System.Collections.Generic.List[object]]::new()
function Invoke-Recorded {
    param([string]$Name, [string]$Executable, [string[]]$Arguments)
    & $Executable @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $evidenceRoot ($Name + '.log')) | ForEach-Object { Write-Host $_ }
    $code = $LASTEXITCODE
    $records.Add(@{ name = $Name; command = $Executable; arguments = $Arguments; exit = $code })
    $records | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'commands.json') -Encoding utf8NoBOM
    if ($code -ne 0) { throw "$Name が終了コード $code で失敗しました" }
}
Push-Location $repoRoot
try {
    Write-Host '【操作可】通常の検証です。GUI 試験は既存の背面・入力透過と display-power lease を使います。'
    switch ($Stage) {
        'Dependencies' {
            Invoke-Recorded 'build' $pwshExe @('scripts/build.ps1', '-Target', 'mvm_test_graph_export')
            Invoke-Recorded 'dependencies' 'build/ucrt64-release/bin/mvm_test_graph_export.exe' @((Join-Path $evidenceRoot 'dependencies'))
        }
        'Controller' {
            Invoke-Recorded 'build' $pwshExe @('scripts/build.ps1', '-Target', 'mvm_test_graph_editor')
            Invoke-Recorded 'inventory' 'C:\msys64\ucrt64\bin\ctest.exe' @('--test-dir', 'build/ucrt64-release', '-N', '-R', '^graph_editor_controller$')
            if ((Get-Content -LiteralPath (Join-Path $evidenceRoot 'inventory.log') -Raw) -notmatch 'Total Tests: 1') {
                throw 'controller 試験が一件ありません'
            }
            Invoke-Recorded 'controller' 'C:\msys64\ucrt64\bin\ctest.exe' @('--test-dir', 'build/ucrt64-release', '--output-on-failure', '--timeout', '180', '-R', '^graph_editor_controller$')
        }
        'Focused' {
            Invoke-Recorded 'build' $pwshExe @('scripts/build.ps1', '-Target', 'mvm_test_graph_export')
            Invoke-Recorded 'focused' 'build/ucrt64-release/bin/mvm_test_graph_export.exe' @((Join-Path $evidenceRoot 'focused'))
            Invoke-Recorded 'encode' 'build/ucrt64-release/bin/mvm_test_graph_export.exe' @('--encode', (Join-Path $evidenceRoot 'encode'))
        }
        'Regressions' {
            foreach ($target in @('mvm_test_graph_domain', 'mvm_test_graph_numeric', 'mvm_test_graph_render',
                'mvm_test_graph_editor', 'mvm_test_equation_export', 'mvm_test_timeline_export_mapping',
                'mvm_test_timeline_export', 'mvm_test_subtitle_export', 'mvm_test_mvm_controller_export')) {
                Invoke-Recorded ('build-' + $target) $pwshExe @('scripts/build.ps1', '-Target', $target, '-ReuseConfigure')
            }
            . (Join-Path $PSScriptRoot 'lib/test-display-lease.ps1')
            $lease = Start-MvmTestDisplayLease
            try {
                $pattern = '^(graph_domain|graph_numeric_.*|graph_render_.*|graph_editor_controller|graph_inspector_qml_.*|math_equation_sequence_export_focused|m7b_3_timeline_export_mapping_focused|m4_timeline_export_focused_.*|subtitle_export.*|mvm_controller_export.*)$'
                Invoke-Recorded 'inventory' 'C:\msys64\ucrt64\bin\ctest.exe' @('--test-dir', 'build/ucrt64-release', '-N', '-R', $pattern)
                $inventory = Get-Content -LiteralPath (Join-Path $evidenceRoot 'inventory.log') -Raw
                if ($inventory -notmatch 'Total Tests: ([1-9][0-9]*)') { throw '回帰試験がありません' }
                Invoke-Recorded 'regressions' 'C:\msys64\ucrt64\bin\ctest.exe' @('--test-dir', 'build/ucrt64-release', '--output-on-failure', '--timeout', '180', '-R', $pattern)
                $lease.AssertValid()
            } finally { $lease.Dispose() }
        }
        'BuildIndependent' { Invoke-Recorded 'independent' $pwshExe @('scripts/test.ps1', '-Preset', 'ucrt64-release', '-Group', 'BuildIndependent') }
        'Lint' { Invoke-Recorded 'lint' $pwshExe @('scripts/lint.ps1') }
        'Release' { Invoke-Recorded 'release' $pwshExe @('scripts/test.ps1', '-Preset', 'ucrt64-release') }
    }
} finally { Pop-Location }
