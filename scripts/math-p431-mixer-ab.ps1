# 同じ Qt・fixture・実行条件で pre-P4-3 と現在を比較し、生の警告を保存する。
[CmdletBinding()]
param([string]$EvidenceDirectory, [string]$Baseline = 'build/p431-baseline',
    [ValidateSet('CTest', 'Basic', 'Windows')][string]$Style = 'CTest')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
if (-not $EvidenceDirectory) { $EvidenceDirectory = Join-Path $repo ('build/math-p431-mixer-ab-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '既存の証拠は上書きしません' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidence = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$baselineRoot = (Resolve-Path -LiteralPath $Baseline).Path
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$testInfo = & C:/msys64/ucrt64/bin/ctest.exe --test-dir (Join-Path $repo 'build/ucrt64-release') --show-only=json-v1 | ConvertFrom-Json
$originalTest = $testInfo.tests | Where-Object name -eq audio_mixer_controls_qml
if ($Style -eq 'CTest') {
    foreach ($entry in ($originalTest.properties | Where-Object name -eq ENVIRONMENT).value) {
        $separator = $entry.IndexOf('=')
        if ($separator -gt 0) {
            [Environment]::SetEnvironmentVariable($entry.Substring(0, $separator), $entry.Substring($separator + 1), 'Process')
        }
    }
} else { $env:QT_QUICK_CONTROLS_STYLE = $Style }
$env:MVM_TEST_FIXED_WINDOW = '1'
$runner = 'C:\msys64\ucrt64\bin\qmltestrunner.exe'
$pwsh = (Get-Process -Id $PID).Path
$records = @()
Write-Host '【操作可】両 arm は同じ offscreen 描画と display-power lease を使います。'
. (Join-Path $PSScriptRoot 'lib/test-display-lease.ps1')
$lease = Start-MvmTestDisplayLease
try {
    foreach ($arm in @(@{ name = 'baseline'; root = $baselineRoot }, @{ name = 'current'; root = $repo })) {
        $armRoot = $arm.root
        Push-Location (Join-Path $repo 'build/ucrt64-release/tests')
        try {
            $arguments = @('-NoProfile', '-File', (Join-Path $armRoot 'tests/qml/run-qml-test.ps1'), '-Runner', $runner,
                '-TestFile', (Join-Path $armRoot 'tests/qml/tst_audio_mixer.qml'), '-OutputFile', (Join-Path $evidence ($arm.name + '-qml.txt')))
            & $pwsh @arguments 2>&1 | Tee-Object -FilePath (Join-Path $evidence ($arm.name + '.log')) | ForEach-Object { Write-Host $_ }
            $records += @{ arm = $arm.name; exit = $LASTEXITCODE; command = $pwsh; arguments = $arguments;
                environment = @(Get-ChildItem Env: | Sort-Object Name | Select-Object Name, Value);
                fixture = (Get-FileHash (Join-Path $armRoot 'tests/qml/tst_audio_mixer.qml')).Hash;
                mixer = (Get-FileHash (Join-Path $armRoot 'apps/mvm/AudioMixerPanel.qml')).Hash;
                qt = @(Get-ChildItem C:/msys64/ucrt64/bin/Qt6*.dll | Get-FileHash | Select-Object Path, Hash);
                plugins = @(Get-ChildItem C:/msys64/ucrt64/share/qt6/qml/QtQuick -Recurse -File | Get-FileHash | Select-Object Path, Hash);
                lease = $lease.State; platform = 'offscreen'; window = '500x400'; revision = 'baseline=7220820';
                original_test = $originalTest; style_condition = $Style }
        } finally { Pop-Location }
    }
} finally {
    $records | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $evidence 'comparison.json') -Encoding utf8NoBOM
    try { $lease.AssertValid() } finally { $lease.Dispose() }
}
Write-Host "証拠: $evidence"
