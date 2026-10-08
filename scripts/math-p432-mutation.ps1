# Graph の render-time motion を外し、評価済みの初回効果を使い続ける負例を検出する。
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
$file = Join-Path $repo 'apps/mvm/mvm_controller.cpp'
$original = [IO.File]::ReadAllText($file)
$hash = (Get-FileHash -LiteralPath $file).Hash
$before = 'if (!animated)'
$after = 'if (!animated || clip.kind == project::TimelineClipKind::Graph)'
if (($original.Split($before).Count - 1) -ne 1) { throw 'motion の変異箇所を一意に特定できません' }
Write-Host '【操作可】背面 native 描画と display-power lease で、効果の固定化を検出します。'
. (Join-Path $PSScriptRoot 'lib/test-display-lease.ps1')
$lease = Start-MvmTestDisplayLease
$env:MVM_TEST_FIXED_WINDOW = '1'
$record = [ordered]@{ name = 'graph-frozen-effects'; original_sha256 = $hash; killed = $false }
try {
    [IO.File]::WriteAllText($file, $original.Replace($before, $after))
    Copy-Item -LiteralPath $file -Destination (Join-Path $evidence 'frozen-effects.cpp')
    & $pwsh scripts/build.ps1 -Target mvm_test_graph_native_preview -ReuseConfigure *> (Join-Path $evidence 'mutated-build.log')
    if ($LASTEXITCODE -ne 0) { throw '変異 build が失敗しました' }
    & build/ucrt64-release/bin/mvm_test_graph_native_preview.exe --effects unused (Join-Path $evidence 'mutated-native') *> (Join-Path $evidence 'mutated-native.log')
    $record.exit = $LASTEXITCODE
    $rows = Import-Csv -LiteralPath (Join-Path $evidence 'mutated-native/continuous-effects.tsv') -Delimiter "`t"
    $broken = @($rows | Where-Object { $_.case -eq 'position-scale' -and [int]$_.frame -gt 0 -and [int]$_.mismatch_pixels -gt 0 -and [int]$_.oracle_mismatch -gt 0 })
    $record.mismatched_position_frames = $broken.Count
    $record.killed = $record.exit -eq 1 -and $broken.Count -eq 3 -and (Select-String -LiteralPath (Join-Path $evidence 'mutated-native.log') -Pattern '連続 ClipEffects は非再利用の基準と独立 oracle の全画素に一致' -Quiet)
    if (-not $record.killed) { throw '初回効果の固定化を連続 frame で検出できません' }
} finally {
    [IO.File]::WriteAllText($file, $original)
    $record.restored_sha256 = (Get-FileHash -LiteralPath $file).Hash
    if ($record.restored_sha256 -ne $hash) { throw 'controller の復元 hash が一致しません' }
    & $pwsh scripts/build.ps1 -Target mvm_test_graph_native_preview -ReuseConfigure *> (Join-Path $evidence 'restored-build.log')
    if ($LASTEXITCODE -ne 0) { throw '復元 build が失敗しました' }
    & build/ucrt64-release/bin/mvm_test_graph_native_preview.exe --effects unused (Join-Path $evidence 'restored-native') *> (Join-Path $evidence 'restored-native.log')
    $record.restored_exit = $LASTEXITCODE
    $record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidence 'mutation.json') -Encoding utf8NoBOM
    try { $lease.AssertValid() } finally { $lease.Dispose() }
    if ($record.restored_exit -ne 0) { throw '復元後の連続効果試験が失敗しました' }
}
Write-Host '効果の固定化を検出し、source の完全復元と再試験を確認しました'
