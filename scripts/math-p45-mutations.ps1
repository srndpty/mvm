# P4-5 の authority 境界だけを変異させ、実 assertion と source 復元を検査する。
[CmdletBinding()]
param([string]$EvidenceDirectory, [string[]]$CaseName)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$pwshExe = (Get-Process -Id $PID).Path
if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $repoRoot ('build/math-p45-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-Mutations')
}
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '既存の証拠を上書きしません' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidenceRoot = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$baseline = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build') -Directory -Filter 'math-p45-*-Focused' |
    Sort-Object Name | Select-Object -Last 1
if (-not $baseline) { throw '同じ source の Focused 証拠が必要です' }
$baselinePath = Join-Path $baseline.FullName 'source-state.json'
$baselineState = Get-Content -LiteralPath $baselinePath -Raw | ConvertFrom-Json
foreach ($source in $baselineState.hashes) {
    if ((Get-FileHash -LiteralPath (Join-Path $repoRoot $source.path)).Hash -ne $source.sha256) {
        throw "Focused 後に source が変わっています: $($source.path)"
    }
}
Copy-Item -LiteralPath $baselinePath -Destination (Join-Path $evidenceRoot 'baseline-source-state.json')
@{ directory = $baseline.FullName; sourceSha256 = $baselineState.sourceSha256 } | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $evidenceRoot 'baseline.json') -Encoding utf8NoBOM
$cases = @(
    @{ name = 'missing-output-frame'; file = 'src/media/mlt/mvm_mlt_export.c'; before = 'mlt_producer_set_in_and_out(output, 0, (mlt_position)(total_duration - 1));'; after = 'mlt_producer_set_in_and_out(output, 0, (mlt_position)(total_duration - 2));'; target = 'mvm_test_graph_export'; test = 'graph_export_encoder_oracle'; assertion = 'H.264 と encoder 直前の全 frame／全画素 oracle' },
    @{ name = 'absent-rgba-validator'; file = 'src/app/timeline_export.cpp'; before = '.rgba_callback = request.encoderFrameValidator'; after = '.rgba_callback = false'; target = 'mvm_test_graph_export'; test = 'graph_export_encoder_oracle'; assertion = 'H.264 と encoder 直前の全 frame／全画素 oracle' },
    @{ name = 'inaccurate-rgba-validator'; file = 'src/app/timeline_export.cpp'; before = 'return inputRequest->encoderFrameValidator(frame, rgba, width, height) ? 0 : 1;'; after = 'return inputRequest->encoderFrameValidator(frame + 1, rgba, width, height) ? 0 : 1;'; target = 'mvm_test_graph_export'; test = 'graph_export_encoder_oracle'; assertion = 'H.264 と encoder 直前の全 frame／全画素 oracle' },
    @{ name = 'visible-intersection'; file = 'src/app/graph_export.cpp'; before = 'if (first >= last)'; after = 'if (false)'; target = 'mvm_test_graph_export'; test = 'graph_export_focused'; assertion = '画面外の不正式を compile しない' },
    @{ name = 'decoded-integrity'; file = 'src/app/graph_export.cpp'; before = 'if (hash != artifact.pixelHashes[hashIndex])'; after = 'if (false)'; target = 'mvm_test_graph_export'; test = 'graph_export_focused'; assertion = '検証後の画素変更を SHA で拒否する' },
    @{ name = 'source-frame'; file = 'src/app/graph_export.cpp'; before = 'const auto index = frame->artifactFrame;'; after = 'const auto index = frame->artifactFrame < 0 ? -1 : (frame->artifactFrame + 1) % package.spec.drawFrames;'; target = 'mvm_test_graph_export'; test = 'graph_export_focused'; assertion = 'Draw／端点を混同せず straight alpha を保持する' },
    @{ name = 'opaque-alpha'; file = 'src/app/timeline_export.cpp'; before = 'const auto& rgba = loaded.raster->rgba;'; after = 'auto rgba = loaded.raster->rgba; for (std::size_t k = 3; k < rgba.size(); k += 4) rgba[k] = 255;'; target = 'mvm_test_graph_composition'; test = 'graph_composition_diagnostic'; assertion = '各境界の全画素が独立期待値に完全一致する' },
    @{ name = 'double-effects'; file = 'src/app/timeline_export.cpp'; before = 'opacityKeys.push_back({key.localFrame, key.opacity});'; after = 'opacityKeys.push_back({key.localFrame, key.opacity * key.opacity});'; target = 'mvm_test_graph_composition'; test = 'graph_composition_differential'; assertion = '各境界の全画素が独立期待値に完全一致する' },
    @{ name = 'backend-from-encoder'; file = 'src/app/timeline_export.cpp'; before = 'return inputRequest->encoderFrameValidator(frame, rgba, width, height) ? 0 : 1;'; after = 'if (inputRequest->graphEnvironment.preflight) inputRequest->graphEnvironment.preflight(inputRequest->graphEnvironment.cache, inputRequest->graphEnvironment.cancel); return inputRequest->encoderFrameValidator(frame, rgba, width, height) ? 0 : 1;'; target = 'mvm_test_graph_export'; test = 'graph_export_encoder_oracle'; assertion = 'backend の準備は encoding 前の一回だけで encoder callback から呼ばない' },
    @{ name = 'final-cancellation'; file = 'src/app/timeline_export.cpp'; before = 'if (request.progress && request.progress(plan.totalDurationFrames, plan.totalDurationFrames))'; after = 'if (false)'; target = 'mvm_test_graph_export'; test = 'graph_export_encoder_oracle'; assertion = '公開境界での取消を反映し最終出力と一時出力を残さない' },
    @{ name = 'publication-error'; file = 'src/app/timeline_export.cpp'; before = "if (pathError) {`n        const auto publicationError = pathError.message();"; after = "if (false) {`n        const auto publicationError = pathError.message();"; target = 'mvm_test_graph_export'; test = 'graph_export_encoder_oracle'; assertion = '実 rename 失敗を型付きで拒否し既存内容を保持する' },
    @{ name = 'reciprocal-normalization'; file = 'tests/harness/mlt_rgba_oracle.h'; before = 'const double sourceAlpha = binary32(binary32(binary32(opacity) * reciprocal) * source[3]);'; after = 'const double sourceAlpha = binary32(binary32(source[3] / 255.0) * binary32(opacity));'; target = 'mvm_test_graph_composition'; test = 'graph_composition_oracle'; assertion = 'alpha 112 の定数逆数演算を直接除算へ置き換えない' },
    @{ name = 'stale-completion'; file = 'apps/mvm/mvm_controller_export.cpp'; before = "if (shutdownStarted_)`n                            return;`n                        finishTimelineExport"; after = "if (false)`n                            return;`n                        finishTimelineExport"; target = 'mvm_test_controller_export'; test = 'm7b_4_controller_export_lifecycle'; assertion = 'shutdown 後の stale 完了結果を公開通知へ流してはいけません' }
)
if ($CaseName) {
    foreach ($name in $CaseName) {
        if (@($cases | Where-Object { $_.name -eq $name }).Count -ne 1) {
            throw "変異ケースが一つではありません: $name"
        }
    }
    $cases = @($cases | Where-Object { $_.name -in $CaseName })
}
$records = [System.Collections.Generic.List[object]]::new()
function Invoke-Test {
    param([string]$Name, [string]$TestName)
    & 'C:/msys64/ucrt64/bin/ctest.exe' --test-dir build/ucrt64-release --output-on-failure --timeout 180 -R ('^' + $TestName + '$') 2>&1 |
        Tee-Object -FilePath (Join-Path $evidenceRoot ($Name + '.log')) | ForEach-Object { Write-Host $_ }
    return $LASTEXITCODE
}
. (Join-Path $PSScriptRoot 'lib/test-display-lease.ps1')
$lease = Start-MvmTestDisplayLease
Push-Location $repoRoot
try {
    Write-Host '【操作可】変異は各一箇所に限定し、finally で元の byte 列を復元します。表示電源の前提を保持し、GUI 試験は背面・入力透過です。'
    foreach ($case in $cases) {
        $file = Join-Path $repoRoot $case.file
        $originalBytes = [IO.File]::ReadAllBytes($file)
        $original = [Text.Encoding]::UTF8.GetString($originalBytes)
        $before = $case.before
        $after = $case.after
        if ($original.Contains("`r`n")) {
            $before = $before.Replace("`n", "`r`n")
            $after = $after.Replace("`n", "`r`n")
        }
        if (($original.Split($before, [StringSplitOptions]::None)).Count -ne 2) {
            throw "変異箇所が一つではありません: $($case.name)"
        }
        $record = @{ name = $case.name; file = $case.file; originalSha256 = (Get-FileHash -LiteralPath $file).Hash; detected = $false; restored = $false }
        try {
            [IO.File]::WriteAllText($file, $original.Replace($before, $after), [Text.UTF8Encoding]::new($false))
            Copy-Item -LiteralPath $file -Destination (Join-Path $evidenceRoot ($case.name + '.source'))
            $record.mutatedSha256 = (Get-FileHash -LiteralPath $file).Hash
            & $pwshExe scripts/build.ps1 -Target $case.target -ReuseConfigure *> (Join-Path $evidenceRoot ($case.name + '-build.log'))
            $record.buildExit = $LASTEXITCODE
            if ($LASTEXITCODE -ne 0) { throw 'build 失敗は変異検出に数えません' }
            $record.testExit = Invoke-Test $case.name $case.test
            $body = Get-Content -LiteralPath (Join-Path $evidenceRoot ($case.name + '.log')) -Raw
            $failurePattern = '(?m)^(?:FAIL: |失敗: )' + [regex]::Escape($case.assertion)
            $record.detected = $record.testExit -eq 8 -and [regex]::IsMatch($body, $failurePattern) -and $body -notmatch 'SEGFAULT|Timeout|Access violation|Exception'
            if (-not $record.detected) { throw "目的の assertion で検出できません: $($case.name)" }
        } finally {
            [IO.File]::WriteAllBytes($file, $originalBytes)
            $record.restoredSha256 = (Get-FileHash -LiteralPath $file).Hash
            $record.restored = $record.restoredSha256 -eq $record.originalSha256
            if (-not $record.restored) { throw '復元 SHA が一致しません' }
            & $pwshExe scripts/build.ps1 -Target $case.target -ReuseConfigure *> (Join-Path $evidenceRoot ($case.name + '-restored-build.log'))
            if ($LASTEXITCODE -ne 0) { throw '復元 build に失敗しました' }
            $record.restoredTestExit = Invoke-Test ($case.name + '-restored') $case.test
            $records.Add($record)
            $records | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidenceRoot 'mutations.json') -Encoding utf8NoBOM
            if ($record.restoredTestExit -ne 0) { throw '復元後の試験に失敗しました' }
        }
    }
    $lease.AssertValid()
} finally {
    Pop-Location
    $lease.Dispose()
}
@{ count = $records.Count; detected = @($records | Where-Object { $_.detected }).Count; restored = @($records | Where-Object { $_.restored }).Count } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidenceRoot 'result.json') -Encoding utf8NoBOM
