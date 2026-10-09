# 単一 frame capture と mixer の寸法循環を戻し、既存の厳密な検査で負例を閉じる。
[CmdletBinding()]
param([Parameter(Mandatory)][string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '既存の証拠は上書きしません' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidence = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$pwsh = (Get-Process -Id $PID).Path
$file = Join-Path $repo 'apps/mvm/graph_preview_animation.h'
$original = [IO.File]::ReadAllText($file)
$hash = (Get-FileHash $file).Hash
$before = 'return value.image ? value.index + 2 : 0;'
$after = 'return value.image && value.index == 0 ? value.index + 2 : 0;'
if (-not $original.Contains($before)) { throw '単一 frame capture の変異箇所がありません' }
$records = @()
try {
    [IO.File]::WriteAllText($file, $original.Replace($before, $after))
    Copy-Item -LiteralPath $file -Destination (Join-Path $evidence 'single-frame.h')
    & $pwsh scripts/build.ps1 -Target mvm_test_graph_preview_cache -ReuseConfigure *> (Join-Path $evidence 'build.log')
    if ($LASTEXITCODE -ne 0) { throw '変異の build が失敗しました' }
    & build/ucrt64-release/bin/mvm_test_graph_preview_cache.exe --continuous *> (Join-Path $evidence 'single-frame.log')
    $code = $LASTEXITCODE
    $detected = $code -eq 1 -and (Select-String (Join-Path $evidence 'single-frame.log') -Pattern 'composition を交換せず' -Quiet)
    $records += @{ name = 'single-frame-capture'; exit = $code; killed = $detected; original_sha256 = $hash }
    if (-not $detected) { throw '連続 Draw の負例を検出できません' }
} finally {
    [IO.File]::WriteAllText($file, $original)
    if ((Get-FileHash $file).Hash -ne $hash) { throw 'Graph source の復元が一致しません' }
    & $pwsh scripts/build.ps1 -Target mvm_test_graph_preview_cache -ReuseConfigure *> (Join-Path $evidence 'restored-build.log')
    if ($LASTEXITCODE -ne 0) { throw '復元 build が失敗しました' }
    & build/ucrt64-release/bin/mvm_test_graph_preview_cache.exe --continuous *> (Join-Path $evidence 'restored-continuous.log')
    if ($LASTEXITCODE -ne 0) { throw '復元後の連続試験が失敗しました' }
}
$file = Join-Path $repo 'apps/mvm/AudioMixerPanel.qml'
$original = [IO.File]::ReadAllText($file)
$hash = (Get-FileHash $file).Hash
try {
    $mutated = $original.Replace('rightPadding: 0', 'rightPadding: effectiveScrollBarWidth + padding').Replace('bottomPadding: 0', 'bottomPadding: effectiveScrollBarHeight + padding')
    if ($mutated -eq $original) { throw 'mixer の変異箇所がありません' }
    [IO.File]::WriteAllText($file, $mutated)
    Copy-Item -LiteralPath $file -Destination (Join-Path $evidence 'cyclic-mixer.qml')
    & C:/msys64/ucrt64/bin/ctest.exe --test-dir build/ucrt64-release -R '^audio_mixer_controls_qml_windows$' --output-on-failure --timeout 30 *> (Join-Path $evidence 'cyclic-mixer.log')
    $code = $LASTEXITCODE
    $detected = $code -ne 0 -and (Select-String (Join-Path $evidence 'cyclic-mixer.log') -Pattern 'Binding loop detected for property "visible"' -Quiet)
    $records += @{ name = 'mixer-visible-cycle'; exit = $code; killed = $detected; original_sha256 = $hash }
    if (-not $detected) { throw 'visible binding loop の負例を検出できません' }
} finally {
    [IO.File]::WriteAllText($file, $original)
    if ((Get-FileHash $file).Hash -ne $hash) { throw 'mixer source の復元が一致しません' }
    & C:/msys64/ucrt64/bin/ctest.exe --test-dir build/ucrt64-release -R '^audio_mixer_controls_qml_windows$' --output-on-failure --timeout 30 *> (Join-Path $evidence 'restored-mixer.log')
    if ($LASTEXITCODE -ne 0) { throw '復元後の mixer が失敗しました' }
    $records | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $evidence 'mutations.json') -Encoding utf8NoBOM
}
Write-Host '単一 frame capture と mixer 循環の負例を検出し、復元後も検査しました'
$file = Join-Path $repo 'apps/mvm/mvm_controller.cpp'
$original = [IO.File]::ReadAllText($file)
$hash = (Get-FileHash $file).Hash
$newline = if ($original.Contains("`r`n")) { "`r`n" } else { "`n" }
$start = $original.IndexOf('    connect(graphRasters_.get(), &GraphPreviewCache::changed, this, [this] {')
$end = $original.IndexOf($newline + '    });', $start) + ($newline + '    });').Length
if ($start -lt 0 -or $end -le $start) { throw '字幕 negative control の箇所がありません' }
$oldHandler = $original.Substring($start, $end - $start)
$newHandler = @('    connect(graphRasters_.get(), &GraphPreviewCache::changed, this, [this] {',
    '        if (!shutdownStarted_ && !playing_) refreshTextPreview();', '    });') -join $newline
Write-Host '【操作可】字幕の元の無条件通知を戻す負例も背面 window と display-power lease で検証します。'
. (Join-Path $PSScriptRoot 'lib/test-display-lease.ps1')
$lease = Start-MvmTestDisplayLease
try {
    [IO.File]::WriteAllText($file, $original.Replace($oldHandler, $newHandler))
    Copy-Item -LiteralPath $file -Destination (Join-Path $evidence 'unconditional-notification.cpp')
    & $pwsh scripts/build.ps1 -Target mvm_test_subtitle_controller -ReuseConfigure *> (Join-Path $evidence 'subtitle-build.log')
    if ($LASTEXITCODE -ne 0) { throw '字幕負例の build が失敗しました' }
    & C:/msys64/ucrt64/bin/ctest.exe --test-dir build/ucrt64-release -R '^subtitle_native_preview$' --output-on-failure --timeout 60 *> (Join-Path $evidence 'unconditional-notification.log')
    $code = $LASTEXITCODE
    $detected = $code -ne 0 -and (Select-String (Join-Path $evidence 'unconditional-notification.log') -Pattern '字幕だけの初期seekを繰り返さない|mapping に Graph が無い cache 通知は seek を増やさない' -Quiet)
    $records += @{ name = 'unconditional-graph-cache-notification'; exit = $code; killed = $detected; original_sha256 = $hash }
    if (-not $detected) { throw '字幕の元の負例を検出できません' }
} finally {
    [IO.File]::WriteAllText($file, $original)
    if ((Get-FileHash $file).Hash -ne $hash) { throw 'controller source の復元が一致しません' }
    & $pwsh scripts/build.ps1 -Target mvm_test_subtitle_controller -ReuseConfigure *> (Join-Path $evidence 'subtitle-restored-build.log')
    if ($LASTEXITCODE -ne 0) { throw '字幕復元後の build が失敗しました' }
    & C:/msys64/ucrt64/bin/ctest.exe --test-dir build/ucrt64-release -R '^subtitle_native_preview.*$' --output-on-failure --timeout 60 *> (Join-Path $evidence 'subtitle-restored.log')
    $restoredCode = $LASTEXITCODE
    $records | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $evidence 'mutations.json') -Encoding utf8NoBOM
    try { $lease.AssertValid() } finally { $lease.Dispose() }
    if ($restoredCode -ne 0) { throw '字幕復元後の検査が失敗しました' }
}
