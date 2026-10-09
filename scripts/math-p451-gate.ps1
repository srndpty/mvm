# 合成契約だけを順序付きで検証し、ソース・DLL・画素・終了コードを固定する。
[CmdletBinding()]
param([ValidateSet('Focused', 'Regressions', 'Lint')][string]$Stage = 'Focused',
      [string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$pwshExe = (Get-Process -Id $PID).Path
$gitTool = Get-Command git -ErrorAction SilentlyContinue
$gitExe = if ($gitTool) { $gitTool.Source } else { 'C:\Program Files\Git\cmd\git.exe' }
if (-not (Test-Path -LiteralPath $gitExe)) { throw 'git がありません' }
if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $repoRoot ('build/math-p451-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Stage)
}
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '既存の証拠は上書きしません' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidenceRoot = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$sources = @(& $gitExe -C $repoRoot ls-files --cached --others --exclude-standard src apps tests scripts cmake CMakeLists.txt)
$hashes = foreach ($relative in $sources) {
    $sourcePath = Join-Path $repoRoot $relative
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) { continue }
    $copyPath = Join-Path $evidenceRoot ('sources/' + $relative)
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $copyPath) -Force
    Copy-Item -LiteralPath $sourcePath -Destination $copyPath
    @{ path = $relative; sha256 = (Get-FileHash -LiteralPath $sourcePath).Hash }
}
@{ revision = (& $gitExe -C $repoRoot rev-parse HEAD).Trim(); hashes = @($hashes) } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'source-state.json') -Encoding utf8NoBOM
$provenance = @()
foreach ($path in @('C:/msys64/ucrt64/bin/libmlt-7.dll', 'C:/msys64/ucrt64/lib/mlt/libmltplus.dll',
    'C:/msys64/ucrt64/lib/mlt/libmltqt6.dll', 'C:/msys64/ucrt64/lib/mlt/libmltcore.dll',
    'C:/msys64/ucrt64/lib/mlt/libmltavformat.dll',
    'C:/msys64/var/lib/pacman/local/mingw-w64-ucrt-x86_64-mlt-7.36.1-1/desc')) {
    if (-not (Test-Path -LiteralPath $path)) { throw "authority のファイルがありません: $path" }
    Copy-Item -LiteralPath $path -Destination $evidenceRoot
    $provenance += @{ path = $path; sha256 = (Get-FileHash -LiteralPath $path).Hash }
}
foreach ($source in @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build') -Filter 'mlt-7.36.1-*' -File)) {
    Copy-Item -LiteralPath $source.FullName -Destination $evidenceRoot
    $provenance += @{ path = $source.Name; sha256 = (Get-FileHash -LiteralPath $source.FullName).Hash }
}
$provenance | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'mlt-provenance.json') -Encoding utf8NoBOM
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
    Write-Host '【操作可】集中検証です。製品 UI や desktop の性能計測は実行しません。'
    switch ($Stage) {
        'Focused' {
            Invoke-Recorded 'build-composition' $pwshExe @('scripts/build.ps1', '-Target', 'mvm_test_graph_composition')
            Invoke-Recorded 'build-export' $pwshExe @('scripts/build.ps1', '-Target', 'mvm_test_graph_export', '-ReuseConfigure')
            foreach ($part in @('diagnostic', 'oracle', 'differential')) {
                Invoke-Recorded $part 'build/ucrt64-release/bin/mvm_test_graph_composition.exe' @($part, (Join-Path $evidenceRoot $part))
            }
            Invoke-Recorded 'dependencies-loader' 'build/ucrt64-release/bin/mvm_test_graph_export.exe' @((Join-Path $evidenceRoot 'dependencies-loader'))
            Invoke-Recorded 'original-encoder' 'build/ucrt64-release/bin/mvm_test_graph_export.exe' @('--encode', (Join-Path $evidenceRoot 'original-encoder'))
        }
        'Regressions' {
            foreach ($target in @('mvm_test_graph_render', 'mvm_test_image_clip', 'mvm_test_equation_export',
                'mvm_test_timeline_export', 'mvm_test_timeline_export_mapping', 'mvm_test_mlt_export_capacity')) {
                Invoke-Recorded ('build-' + $target) $pwshExe @('scripts/build.ps1', '-Target', $target, '-ReuseConfigure')
            }
            $pattern = '^(graph_render_presentation|graph_render_artifact|image_clip_contract|math_equation_sequence_export_focused|m4_timeline_export_focused_.*|m7b_3_timeline_export_mapping_focused|mlt_export_capacity_overflow)$'
            Invoke-Recorded 'inventory' 'C:/msys64/ucrt64/bin/ctest.exe' @('--test-dir', 'build/ucrt64-release', '-N', '-R', $pattern)
            if ((Get-Content -LiteralPath (Join-Path $evidenceRoot 'inventory.log') -Raw) -notmatch 'Total Tests: 9') {
                throw '指定した回帰試験が九件ありません'
            }
            Invoke-Recorded 'regressions' 'C:/msys64/ucrt64/bin/ctest.exe' @('--test-dir', 'build/ucrt64-release', '--output-on-failure', '--timeout', '180', '-R', $pattern)
        }
        'Lint' { Invoke-Recorded 'lint' $pwshExe @('scripts/lint.ps1') }
    }
} finally { Pop-Location }
