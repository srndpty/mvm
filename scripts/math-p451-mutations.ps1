# source を一箇所だけ変更し、build 成功後の assertion 失敗と SHA 復元を検査する。
[CmdletBinding()]
param([string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$pwshExe = (Get-Process -Id $PID).Path
if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $repoRoot ('build/math-p451-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-Mutations')
}
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '既存の証拠を上書きしません' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidenceRoot = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$cases = @(
    @{ name = 'source-frame'; file = 'src/app/graph_export.cpp'; before = 'const auto index = frame->artifactFrame;'; after = 'const auto index = frame->artifactFrame < 0 ? -1 : (frame->artifactFrame + 1) % package.spec.drawFrames;'; target = 'mvm_test_graph_export'; test = 'graph_export_focused'; assertion = 'Draw／端点を混同せず straight alpha を保持する' },
    @{ name = 'opaque-alpha'; file = 'src/app/timeline_export.cpp'; before = 'const auto& rgba = loaded.raster->rgba;'; after = 'auto rgba = loaded.raster->rgba; for (std::size_t k = 3; k < rgba.size(); k += 4) rgba[k] = 255;'; target = 'mvm_test_graph_composition'; test = 'graph_composition_diagnostic'; assertion = '各境界の全画素が独立期待値に完全一致する' },
    @{ name = 'graph-image-divergence'; file = 'src/app/timeline_export.cpp'; before = 'const auto& rgba = loaded.raster->rgba;'; after = 'auto rgba = loaded.raster->rgba; for (std::size_t k = 0; k < rgba.size(); k += 4) rgba[k] = 0;'; target = 'mvm_test_graph_composition'; test = 'graph_composition_diagnostic'; assertion = '各境界の全画素が独立期待値に完全一致する' },
    @{ name = 'alpha-interpretation'; file = 'tests/harness/mlt_rgba_oracle.h'; before = 'binary32(source[c] * weight)'; after = 'binary32(source[c] * binary32(weight * weight))'; target = 'mvm_test_graph_composition'; test = 'graph_composition_oracle'; assertion = '各境界の全画素が独立期待値に完全一致する' },
    @{ name = 'round-channel'; file = 'tests/harness/mlt_rgba_oracle.h'; before = 'std::trunc(value)'; after = 'std::round(value)'; target = 'mvm_test_graph_composition'; test = 'graph_composition_oracle'; assertion = '各境界の全画素が独立期待値に完全一致する' },
    @{ name = 'layer-order'; file = 'src/media/mlt/mvm_mlt_rgba_diagnostic.c'; before = 'paths[layer]'; after = 'paths[count - 1 - layer]'; target = 'mvm_test_graph_composition'; test = 'graph_composition_oracle'; assertion = '各境界の全画素が独立期待値に完全一致する' },
    @{ name = 'audit-format'; file = 'src/media/mlt/mvm_mlt_export.c'; before = 'if (attach_rgba_audit(output, &audit))'; after = 'if (attach_rgba_audit(v1_background, &audit))'; target = 'mvm_test_graph_composition'; test = 'graph_composition_diagnostic'; assertion = '各境界の全画素が独立期待値に完全一致する' },
    @{ name = 'background-alpha'; file = 'src/media/mlt/mvm_mlt_export.c'; before = 'v1_background = mlt_factory_producer(profile, "color", "#000000");'; after = 'v1_background = mlt_factory_producer(profile, "color", "#00000000");'; target = 'mvm_test_graph_composition'; test = 'graph_composition_diagnostic'; assertion = '各境界の全画素が独立期待値に完全一致する' }
)
$records = [System.Collections.Generic.List[object]]::new()
function Invoke-CapturedTest {
    param([string]$Name, [string]$TestName)
    & 'C:/msys64/ucrt64/bin/ctest.exe' --test-dir build/ucrt64-release --output-on-failure --timeout 180 -R ('^' + $TestName + '$') 2>&1 |
        Tee-Object -FilePath (Join-Path $evidenceRoot ($Name + '.log')) | ForEach-Object { Write-Host $_ }
    return $LASTEXITCODE
}
Push-Location $repoRoot
try {
    Write-Host '【操作可】集中変異試験です。対象 source は各試験の finally で SHA を照合して復元します。'
    foreach ($case in $cases) {
        $file = Join-Path $repoRoot $case.file
        $originalBytes = [IO.File]::ReadAllBytes($file)
        $original = [Text.Encoding]::UTF8.GetString($originalBytes)
        $originalSha = (Get-FileHash -LiteralPath $file).Hash
        if (($original.Split($case.before, [StringSplitOptions]::None)).Count -ne 2) {
            throw "変更箇所が一つではありません: $($case.name)"
        }
        $record = @{ name = $case.name; file = $case.file; original_sha256 = $originalSha; detected = $false; restored = $false }
        try {
            [IO.File]::WriteAllText($file, $original.Replace($case.before, $case.after), [Text.UTF8Encoding]::new($false))
            Copy-Item -LiteralPath $file -Destination (Join-Path $evidenceRoot ($case.name + '.source'))
            $record.mutated_sha256 = (Get-FileHash -LiteralPath $file).Hash
            & $pwshExe scripts/build.ps1 -Target $case.target -ReuseConfigure *> (Join-Path $evidenceRoot ($case.name + '-build.log'))
            $record.build_exit = $LASTEXITCODE
            if ($LASTEXITCODE -ne 0) { throw 'build 失敗は変異検出に数えません' }
            $code = Invoke-CapturedTest $case.name $case.test
            $body = Get-Content -LiteralPath (Join-Path $evidenceRoot ($case.name + '.log')) -Raw
            $record.test_exit = $code
            $record.detected = $code -eq 8 -and $body.Contains($case.assertion) -and $body -notmatch 'SEGFAULT|Timeout|Access violation|Exception'
            if (-not $record.detected) { throw "assertion で検出できません: $($case.name)" }
        } finally {
            [IO.File]::WriteAllBytes($file, $originalBytes)
            $record.restored_sha256 = (Get-FileHash -LiteralPath $file).Hash
            $record.restored = $record.restored_sha256 -eq $originalSha
            if (-not $record.restored) { throw 'source 復元 SHA が一致しません' }
            & $pwshExe scripts/build.ps1 -Target $case.target -ReuseConfigure *> (Join-Path $evidenceRoot ($case.name + '-restored-build.log'))
            if ($LASTEXITCODE -ne 0) { throw '復元 build が失敗しました' }
            $record.restored_test_exit = Invoke-CapturedTest ($case.name + '-restored') $case.test
            $records.Add($record)
            $records | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'mutations.json') -Encoding utf8NoBOM
            if ($record.restored_test_exit -ne 0) { throw '復元後の試験が失敗しました' }
        }
    }
} finally { Pop-Location }
@{ count = $records.Count; detected = @($records | Where-Object { $_.detected }).Count; restored = @($records | Where-Object { $_.restored }).Count } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidenceRoot 'result.json') -Encoding utf8NoBOM
