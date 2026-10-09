# P4-5 補遺: encoder の最初の frame を処理中の shutdown を、製品の取消と stale 完了通知の
# 境界だけ変異させて検査する。実製品の Graph 受入 (Real) で検出し、元の byte 列へ復元する。
[CmdletBinding()]
param([string]$EvidenceDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = Split-Path -Parent $PSScriptRoot
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
$pwshExe = (Get-Process -Id $PID).Path
if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $repoRoot ('build/math-p45-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-ActiveShutdownMutations')
}
if (Test-Path -LiteralPath $EvidenceDirectory) { throw '既存の証拠を上書きしません' }
$manimExe = "$env:APPDATA/uv/tools/manim/Scripts/manim.exe"
if (-not (Test-Path -LiteralPath $manimExe)) { throw '実 Manim がありません' }
$null = New-Item -ItemType Directory -Path $EvidenceDirectory
$evidenceRoot = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
# 変異前の source が、通過した Real と同じであることを要求する。
$baseline = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build') -Directory -Filter 'math-p45-*-Real' |
    Sort-Object Name | Select-Object -Last 1
if (-not $baseline) { throw '同じ source の Real 証拠が必要です' }
$baselineCommands = Get-Content -LiteralPath (Join-Path $baseline.FullName 'commands.json') -Raw | ConvertFrom-Json
if (@($baselineCommands | Where-Object { $_.name -eq 'product' -and $_.exit -eq 0 }).Count -ne 1) {
    throw "最新の Real が通過していません: $($baseline.Name)"
}
$baselinePath = Join-Path $baseline.FullName 'source-state.json'
$baselineState = Get-Content -LiteralPath $baselinePath -Raw | ConvertFrom-Json
foreach ($source in $baselineState.hashes) {
    if ((Get-FileHash -LiteralPath (Join-Path $repoRoot $source.path)).Hash -ne $source.sha256) {
        throw "Real 後に source が変わっています: $($source.path)"
    }
}
Copy-Item -LiteralPath $baselinePath -Destination (Join-Path $evidenceRoot 'baseline-source-state.json')
@{ directory = $baseline.FullName; sourceSha256 = $baselineState.sourceSha256 } | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $evidenceRoot 'baseline.json') -Encoding utf8NoBOM
$cases = @(
    @{ name = 'shutdown-cancel-bypass'; file = 'apps/mvm/mvm_controller.cpp'; before = "settleRecoveryWrite();`n    exportCancelRequested_.store(true, std::memory_order_release);"; after = 'settleRecoveryWrite();'; assertion = 'P4-5 補遺: encoder の保持中に shutdown の取消を完了待ち loop が観測し、join で寿命を解決する' },
    @{ name = 'active-stale-completion'; file = 'apps/mvm/mvm_controller.cpp'; before = "if (shutdownStarted_)`n                            return;`n                        finishTimelineExport"; after = "if (false)`n                            return;`n                        finishTimelineExport"; assertion = 'P4-5 補遺: shutdown 後に stale な完了通知を出さない' }
)
$records = [System.Collections.Generic.List[object]]::new()
function Invoke-Product {
    param([string]$Name)
    $output = Join-Path $evidenceRoot ($Name + '-product')
    & 'build/ucrt64-release/bin/mvm_test_text_ui_input.exe' --graph-export-ui $manimExe $output 2>&1 |
        Tee-Object -FilePath (Join-Path $evidenceRoot ($Name + '.log')) | ForEach-Object { Write-Host $_ }
    return $LASTEXITCODE
}
. (Join-Path $PSScriptRoot 'lib/test-display-lease.ps1')
$lease = Start-MvmTestDisplayLease
Push-Location $repoRoot
try {
    Write-Host '【操作可】変異は各一箇所に限定し、finally で元の byte 列を復元します。GUI 試験は背面・入力透過です。'
    $env:MVM_TEST_FIXED_WINDOW = '1'
    $env:MVM_TEST_AUDIO_VOLUME_SCALE = '0.25'
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
        $record = @{ name = $case.name; file = $case.file; assertion = $case.assertion; originalSha256 = (Get-FileHash -LiteralPath $file).Hash; detected = $false; restored = $false }
        try {
            [IO.File]::WriteAllText($file, $original.Replace($before, $after), [Text.UTF8Encoding]::new($false))
            Copy-Item -LiteralPath $file -Destination (Join-Path $evidenceRoot ($case.name + '.source'))
            $record.mutatedSha256 = (Get-FileHash -LiteralPath $file).Hash
            & $pwshExe scripts/build.ps1 -Target mvm_test_text_ui_input -ReuseConfigure *> (Join-Path $evidenceRoot ($case.name + '-build.log'))
            $record.buildExit = $LASTEXITCODE
            if ($LASTEXITCODE -ne 0) { throw 'build 失敗は変異検出に数えません' }
            $record.testExit = Invoke-Product $case.name
            $body = Get-Content -LiteralPath (Join-Path $evidenceRoot ($case.name + '.log')) -Raw
            # 検出は終了コード 1 (assertion の失敗) と、狙った assertion の FAIL 行に限る。
            $record.detected = $record.testExit -eq 1 -and $body.Contains('FAIL: ' + $case.assertion) -and
                $body -notmatch 'SEGFAULT|Access violation|Exception|\(timeout\)'
            if (-not $record.detected) { throw "目的の assertion で検出できません: $($case.name)" }
        } finally {
            [IO.File]::WriteAllBytes($file, $originalBytes)
            $record.restoredSha256 = (Get-FileHash -LiteralPath $file).Hash
            $record.restored = $record.restoredSha256 -eq $record.originalSha256
            if (-not $record.restored) { throw '復元 SHA が一致しません' }
            & $pwshExe scripts/build.ps1 -Target mvm_test_text_ui_input -ReuseConfigure *> (Join-Path $evidenceRoot ($case.name + '-restored-build.log'))
            if ($LASTEXITCODE -ne 0) { throw '復元 build に失敗しました' }
            $record.restoredTestExit = Invoke-Product ($case.name + '-restored')
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
