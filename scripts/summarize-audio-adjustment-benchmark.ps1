param(
    [Parameter(Mandatory)][string]$Before,
    [Parameter(Mandatory)][string]$After,
    [Parameter(Mandatory)][string]$Report,
    [switch]$BeforeHadOtherWork
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-Runs([string]$Path) {
    $runs = @(Get-Content -LiteralPath $Path | Where-Object { $_.Trim() } | ForEach-Object { $_ | ConvertFrom-Json })
    if ($runs.Count -ne 3) { throw '比較には固定した 3 回の計測が必要です' }
    for ($index = 0; $index -lt $runs.Count; $index++) {
        $run = $runs[$index]
        if ($run.run -ne $index + 1 -or $run.clips -ne 2 -or $run.secondsPerClip -le 0 -or $run.elapsedMs -le 0) {
            throw '計測の件数・尺・経過時間が不正です'
        }
        if (-not [double]::IsFinite([double]$run.elapsedMs) -or -not [double]::IsFinite([double]$run.secondsPerClip)) {
            throw '尺または経過時間が有限値ではありません'
        }
        foreach ($field in @('voiceLufs', 'voicePeakDb')) {
            if (-not [double]::IsFinite([double]$run.$field)) { throw '測定値が有限値ではありません' }
        }
    }
    return $runs
}
function Get-Statistics($Runs) {
    $times = @($Runs.elapsedMs | Sort-Object)
    return @{
        MedianMs = $times[1]
        MaxMs = $times[2]
        Throughput = $Runs[0].secondsPerClip * $Runs[0].clips * 1000.0 / $times[1]
    }
}
$beforeRuns = @(Read-Runs $Before)
$afterRuns = @(Read-Runs $After)
foreach ($run in @($beforeRuns) + @($afterRuns)) {
    $reference = $beforeRuns[0]
    foreach ($field in @('clips', 'secondsPerClip', 'voiceLufs', 'voicePeakDb', 'ranges')) {
        if ($run.$field -ne $reference.$field) { throw "比較する解析条件または結果が一致しません: $field" }
    }
}
$beforeStats = Get-Statistics $beforeRuns
$afterStats = Get-Statistics $afterRuns
$speedup = $beforeStats.MedianMs / [double]$afterStats.MedianMs
$conditions = if ($BeforeHadOtherWork) {
    '変更前の長尺計測中にはビルド・短い試験も実行したため、この比較は診断値として扱う。'
} else {
    '専用の計測環境で保証した性能値ではなく、この素材と実行環境での診断値として扱う。'
}
$rows = @(
    '# 自動音量調整の処理時間',
    '',
    "各 $($beforeRuns[0].secondsPerClip) 秒の声と BGM、2 クリップを解析。release、固定 3 回。",
    '素材は 48 kHz モノラル PCM、1 kHz の連続正弦波。実スピーチでの計測ではない。',
    $conditions,
    '',
    '| 版 | 中央値 (秒) | 観測最大 (秒) | 音声の総尺 / 処理時間 |',
    '| --- | ---: | ---: | ---: |',
    ('| 変更前 | {0:F3} | {1:F3} | {2:F2} 倍速 |' -f ($beforeStats.MedianMs / 1000), ($beforeStats.MaxMs / 1000), $beforeStats.Throughput),
    ('| 最適化後 | {0:F3} | {1:F3} | {2:F2} 倍速 |' -f ($afterStats.MedianMs / 1000), ($afterStats.MaxMs / 1000), $afterStats.Throughput),
    '',
    ('中央値で {0:F2} 倍。全 6 回の声の LUFS・true peak・検出区間数は一致。' -f $speedup),
    '',
    "生データ: $Before、$After"
)
$markdown = $rows -join "`n"
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Report), $markdown + "`n", [Text.UTF8Encoding]::new($false))
Write-Output $markdown
