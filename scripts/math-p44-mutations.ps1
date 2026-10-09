# 六つの契約を壊し、正常終了した assertion 失敗だけを検出と数える。
[CmdletBinding()]
param([Parameter(Mandatory)][string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '既存の証拠は上書きしません' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidenceRoot = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$pwshExe = (Get-Process -Id $PID).Path
$gitTool = Get-Command git -ErrorAction SilentlyContinue
$gitExe = if ($gitTool) { $gitTool.Source } else { 'C:\Program Files\Git\cmd\git.exe' }
if (-not (Test-Path -LiteralPath $gitExe)) { throw 'git がありません' }
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
$cases = @(
    @{ name = 'last-function'; file = 'src/project/graph_clip.cpp'; before = 'data.functions.empty()'; after = 'false'; target = 'mvm_test_graph_editor'; test = 'graph_editor_controller'; assertion = '不正な操作の拒否' },
    @{ name = 'invalid-source'; file = 'apps/mvm/mvm_controller.cpp'; before = 'function->expression = it.value().toString().toStdString();'; after = 'function->expression = "x";'; target = 'mvm_test_graph_editor'; test = 'graph_editor_controller'; assertion = '式の完全一致' },
    @{ name = 'reorder-id'; file = 'src/project/graph_clip.cpp'; before = 'auto f = *it;'; after = 'auto f = *it; f.id.value = "mutated-id";'; target = 'mvm_test_graph_editor'; test = 'graph_editor_controller'; assertion = '並べ替えで所有 ID を保つ' },
    @{ name = 'split-draw'; file = 'src/project/graph_edit.cpp'; before = 'frame.sourceFrame = source.frame;'; after = 'frame.sourceFrame = source.frame - clip.sourceInFrame;'; target = 'mvm_test_graph_preview_cache'; test = 'graph_preview_cache'; assertion = 'trim と split は素材原点の位相を維持' },
    @{ name = 'old-key'; file = 'apps/mvm/graph_preview_cache.cpp'; before = 'it.value()->frames.store(std::move(snapshot));'; after = 'if (!snapshot->empty()) it.value()->frames.store(std::move(snapshot));'; target = 'mvm_test_graph_preview_cache'; test = 'graph_preview_cache'; assertion = '旧 key の共有所有者が残っても presentation authority は透明' },
    @{ name = 'narrow-layout'; file = 'apps/mvm/GraphClipInspector.qml'; before = 'objectName: "graphAddFunction"'; after = 'objectName: "graphAddFunction"; visible: false'; target = ''; test = 'graph_inspector_qml_Basic'; assertion = 'test_layout' }
)
$records = @()
function Invoke-CTestCaptured {
    param([string]$LogPath, [string]$TestName)
    $startInfo = [Diagnostics.ProcessStartInfo]::new('C:/msys64/ucrt64/bin/ctest.exe')
    $startInfo.WorkingDirectory = $repoRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $utf8 = [Text.UTF8Encoding]::new($false)
    $startInfo.StandardOutputEncoding = $utf8
    $startInfo.StandardErrorEncoding = $utf8
    foreach ($argument in @('--test-dir', 'build/ucrt64-release', '-R', ('^' + $TestName + '$'), '--output-on-failure', '--timeout', '180')) {
        $startInfo.ArgumentList.Add($argument)
    }
    $process = [Diagnostics.Process]::Start($startInfo)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(240000)) {
        $process.Kill($true)
        throw "ctest が timeout しました: $TestName"
    }
    $process.WaitForExit()
    $log = $stdoutTask.GetAwaiter().GetResult() + $stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText($LogPath, $log, $utf8)
    return @{ exit = $process.ExitCode; body = $log }
}
Push-Location $repoRoot
try {
    foreach ($case in $cases) {
        $file = Join-Path $repoRoot $case.file
        $original = [IO.File]::ReadAllText($file)
        $hash = (Get-FileHash -LiteralPath $file).Hash
        if (-not $original.Contains($case.before)) { throw "変異箇所がありません: $($case.name)" }
        try {
            [IO.File]::WriteAllText($file, $original.Replace($case.before, $case.after))
            Copy-Item -LiteralPath $file -Destination (Join-Path $evidenceRoot ($case.name + '.source'))
            if ($case.target) {
                & $pwshExe scripts/build.ps1 -Target $case.target -ReuseConfigure *> (Join-Path $evidenceRoot ($case.name + '-build.log'))
                if ($LASTEXITCODE -ne 0) { throw '変異 build が失敗しました' }
            }
            $captured = Invoke-CTestCaptured -LogPath (Join-Path $evidenceRoot ($case.name + '.log')) -TestName $case.test
            $code = $captured.exit
            $body = $captured.body
            $detected = $code -eq 8 -and $body.Contains($case.assertion) -and $body -notmatch 'Exception|SEGFAULT|Timeout|Access violation'
            # QML wrapper の assertion 後の throw は正常な試験失敗である。
            if (-not $case.target) { $detected = $code -eq 8 -and $body.Contains($case.assertion) -and $body -match 'FAIL!' -and $body -notmatch 'compile\(\)|Timeout' }
            $records += @{ name = $case.name; exit = $code; detected = $detected; original_sha256 = $hash }
            if (-not $detected) { throw "assertion の検出が成立しません: $($case.name)" }
        } finally {
            [IO.File]::WriteAllText($file, $original)
            if ((Get-FileHash -LiteralPath $file).Hash -ne $hash) { throw 'source の復元 hash が一致しません' }
            if ($case.target) {
                & $pwshExe scripts/build.ps1 -Target $case.target -ReuseConfigure *> (Join-Path $evidenceRoot ($case.name + '-restored-build.log'))
                if ($LASTEXITCODE -ne 0) { throw '復元 build が失敗しました' }
            }
            $restored = Invoke-CTestCaptured -LogPath (Join-Path $evidenceRoot ($case.name + '-restored.log')) -TestName $case.test
            if ($restored.exit -ne 0) { throw '復元後の試験が失敗しました' }
            $records | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'mutations.json') -Encoding utf8NoBOM
        }
    }
} finally { Pop-Location }
@{ exit = 0; stage = 'Mutations'; count = $records.Count } | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $evidenceRoot 'result.json') -Encoding utf8NoBOM
