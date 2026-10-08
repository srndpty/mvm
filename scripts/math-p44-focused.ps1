# P4-4 の集中試験を新規 directory と source snapshot に固定する。
[CmdletBinding()]
param([string]$EvidenceDirectory,
      [ValidateSet('Focused', 'Real')][string]$Stage = 'Focused',
      [string]$Manim = "$env:APPDATA/uv/tools/manim/Scripts/manim.exe")
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$gitTool = Get-Command git -ErrorAction SilentlyContinue
$gitExe = if ($gitTool) { $gitTool.Source } else { 'C:\Program Files\Git\cmd\git.exe' }
if (-not (Test-Path -LiteralPath $gitExe)) { throw 'git がありません' }
if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $repoRoot ('build/math-p44-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $Stage)
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
    @{ path = $relative; sha256 = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash }
}
@{ revision = (& $gitExe -C $repoRoot rev-parse HEAD).Trim(); hashes = @($hashes) } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'source-state.json') -Encoding utf8NoBOM
Push-Location $repoRoot
try {
    Write-Host '【操作可】offline QML と controller の通常検証です。'
    $pwshExe = (Get-Process -Id $PID).Path
    $target = if ($Stage -eq 'Real') { 'mvm_test_text_ui_input' } else { 'mvm_test_graph_editor' }
    & $pwshExe scripts/build.ps1 -Target $target 2>&1 | Tee-Object -FilePath (Join-Path $evidenceRoot 'build.log')
    if ($LASTEXITCODE -ne 0) { throw '専用試験のビルドに失敗しました' }
    if ($Stage -eq 'Real') {
        . (Join-Path $PSScriptRoot 'lib/test-display-lease.ps1')
        $lease = Start-MvmTestDisplayLease
        try {
            $env:MVM_TEST_FIXED_WINDOW = '1'
            $env:MVM_TEST_AUDIO_VOLUME_SCALE = '0.25'
            & build/ucrt64-release/bin/mvm_test_text_ui_input.exe --graph-ui $Manim (Join-Path $evidenceRoot 'product') 2>&1 |
                Tee-Object -FilePath (Join-Path $evidenceRoot 'product.log')
            $code = $LASTEXITCODE
            @{ exit = $code; stage = $Stage; manim = $Manim } | ConvertTo-Json |
                Set-Content -LiteralPath (Join-Path $evidenceRoot 'result.json') -Encoding utf8NoBOM
            $lease.AssertValid()
            if ($code -ne 0) { throw '実製品 UI 試験が失敗しました' }
        } finally { $lease.Dispose() }
        return
    }
    $ctestExe = 'C:\msys64\ucrt64\bin\ctest.exe'
    $pattern = '^(graph_editor_controller|graph_inspector_qml_.*)$'
    & $ctestExe --test-dir build/ucrt64-release -N -R $pattern | Tee-Object -FilePath (Join-Path $evidenceRoot 'inventory.log')
    if ($LASTEXITCODE -ne 0) { throw '試験一覧を取得できません' }
    $inventory = Get-Content -LiteralPath (Join-Path $evidenceRoot 'inventory.log') -Raw
    if ($inventory -notmatch 'Total Tests: 4') { throw '集中試験が四件ありません' }
    & $ctestExe --test-dir build/ucrt64-release --output-on-failure --timeout 180 -R $pattern 2>&1 |
        Tee-Object -FilePath (Join-Path $evidenceRoot 'ctest.log')
    $code = $LASTEXITCODE
    @{ exit = $code; pattern = $pattern; timeout = 180 } | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $evidenceRoot 'result.json') -Encoding utf8NoBOM
    if ($code -ne 0) { throw '集中試験が失敗しました' }
} finally { Pop-Location }
