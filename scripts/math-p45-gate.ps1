# Graph export の source と実行結果を、新規の証拠 directory に保存する。
[CmdletBinding()]
param([ValidateSet('Dependencies', 'Focused', 'Real', 'RealPng', 'AlphaDomain', 'Controller', 'Regressions', 'BuildIndependent', 'Lint', 'Release', 'Review')]
      [string]$Stage = 'Focused', [string]$EvidenceDirectory, [string]$InputPng)
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
        'AlphaDomain' {
            Invoke-Recorded 'build' $pwshExe @('scripts/build.ps1', '-Target', 'mvm_test_graph_composition', '-ReuseConfigure')
            Invoke-Recorded 'alpha-domain' 'build/ucrt64-release/bin/mvm_test_graph_composition.exe' @('alpha-domain', (Join-Path $evidenceRoot 'pixels'))
            Invoke-Recorded 'nearest-binary' 'C:/msys64/ucrt64/bin/objdump.exe' @('-d', '--start-address=0x384452f10', '--stop-address=0x384453040', 'C:/msys64/ucrt64/lib/mlt/libmltplus.dll')
            Invoke-Recorded 'constants' 'C:/msys64/ucrt64/bin/objdump.exe' @('-s', '-j', '.rdata', '--start-address=0x38446d4e8', '--stop-address=0x38446d4f4', 'C:/msys64/ucrt64/lib/mlt/libmltplus.dll')
            @{ path = 'C:/msys64/ucrt64/lib/mlt/libmltplus.dll'; sha256 = (Get-FileHash -LiteralPath 'C:/msys64/ucrt64/lib/mlt/libmltplus.dll').Hash } |
                ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidenceRoot 'binary-source.json') -Encoding utf8NoBOM
        }
        'RealPng' {
            if (-not $InputPng -or -not (Test-Path -LiteralPath $InputPng -PathType Leaf)) { throw '観測する実 PNG がありません' }
            Copy-Item -LiteralPath $InputPng -Destination (Join-Path $evidenceRoot 'input.png')
            @{ path = (Resolve-Path -LiteralPath $InputPng).Path; sha256 = (Get-FileHash -LiteralPath $InputPng).Hash } |
                ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidenceRoot 'input-source.json') -Encoding utf8NoBOM
            Invoke-Recorded 'build' $pwshExe @('scripts/build.ps1', '-Target', 'mvm_test_graph_composition', '-ReuseConfigure')
            Invoke-Recorded 'real-png' 'build/ucrt64-release/bin/mvm_test_graph_composition.exe' @('real-png', (Join-Path $evidenceRoot 'pixels'), (Join-Path $evidenceRoot 'input.png'))
        }
        'Real' {
            Invoke-Recorded 'build' $pwshExe @('scripts/build.ps1', '-Target', 'mvm_test_text_ui_input', '-ReuseConfigure')
            . (Join-Path $PSScriptRoot 'lib/test-display-lease.ps1')
            $lease = Start-MvmTestDisplayLease
            try {
                $env:MVM_TEST_FIXED_WINDOW = '1'
                $env:MVM_TEST_AUDIO_VOLUME_SCALE = '0.25'
                $manimExe = "$env:APPDATA/uv/tools/manim/Scripts/manim.exe"
                if (-not (Test-Path -LiteralPath $manimExe)) { throw '実 Manim がありません' }
                Invoke-Recorded 'product' 'build/ucrt64-release/bin/mvm_test_text_ui_input.exe' @('--graph-export-ui', $manimExe, (Join-Path $evidenceRoot 'product'))
                $lease.AssertValid()
            } finally { $lease.Dispose() }
        }
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
            Invoke-Recorded 'build-composition' $pwshExe @('scripts/build.ps1', '-Target', 'mvm_test_graph_composition', '-ReuseConfigure')
            foreach ($mode in @('diagnostic', 'oracle', 'differential')) {
                Invoke-Recorded $mode 'build/ucrt64-release/bin/mvm_test_graph_composition.exe' @($mode, (Join-Path $evidenceRoot $mode))
            }
            Invoke-Recorded 'build' $pwshExe @('scripts/build.ps1', '-Target', 'mvm_test_graph_export', '-ReuseConfigure')
            Invoke-Recorded 'focused' 'build/ucrt64-release/bin/mvm_test_graph_export.exe' @((Join-Path $evidenceRoot 'focused'))
            Invoke-Recorded 'encode' 'build/ucrt64-release/bin/mvm_test_graph_export.exe' @('--encode', (Join-Path $evidenceRoot 'encode'))
        }
        'Regressions' {
            foreach ($target in @('mvm_test_graph_domain', 'mvm_test_graph_numeric', 'mvm_test_graph_render',
                'mvm_test_graph_editor', 'mvm_test_equation_export', 'mvm_test_timeline_export_mapping',
                'mvm_test_timeline_export', 'mvm_test_subtitle_export', 'mvm_test_controller_export',
                'mvm_test_clip_effects', 'mvm_test_text_ui_input')) {
                Invoke-Recorded ('build-' + $target) $pwshExe @('scripts/build.ps1', '-Target', $target, '-ReuseConfigure')
            }
            . (Join-Path $PSScriptRoot 'lib/test-display-lease.ps1')
            $lease = Start-MvmTestDisplayLease
            try {
                $pattern = '^(graph_domain|graph_numeric|graph_render_.*|graph_editor_controller|graph_inspector_qml_.*|math_equation_sequence_export_focused|math_equation_sequence_product_ui|m7a_1_clip_effects_focused|m7b_3_timeline_export_mapping_focused|m4_timeline_export_focused_.*|subtitle_export.*|m7b_4_controller_export_lifecycle)$'
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
        'Review' {
            Invoke-Recorded 'product-symbols' 'C:/msys64/ucrt64/bin/nm.exe' @('build/ucrt64-release/bin/mvm.exe')
            Invoke-Recorded 'diagnostic-symbols' 'C:/msys64/ucrt64/bin/nm.exe' @('build/ucrt64-release/bin/mvm_test_graph_composition.exe')
            $productSymbols = Get-Content -LiteralPath (Join-Path $evidenceRoot 'product-symbols.log') -Raw
            $diagnosticSymbols = Get-Content -LiteralPath (Join-Path $evidenceRoot 'diagnostic-symbols.log') -Raw
            if ($productSymbols.Contains('mvm_mlt_rgba_diagnostic') -or -not $diagnosticSymbols.Contains('mvm_mlt_rgba_diagnostic')) {
                throw '診断専用 object のリンク分離を確認できません'
            }
            @{ productionIncludesDiagnostic = $false; diagnosticIncludesDiagnostic = $true; decision = '静的 library に維持する。製品から未参照の object はリンクされない。' } |
                ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidenceRoot 'review.json') -Encoding utf8NoBOM
        }
    }
} finally { Pop-Location }
