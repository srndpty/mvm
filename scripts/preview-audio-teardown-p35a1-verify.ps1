<#
.SYNOPSIS
    P3-5A.1: preview の audio の後始末 (PreviewEngine::removeSource) の解放後の参照を、
    修正の前後で同じ条件で確かめる。

.DESCRIPTION
    決めておいた試験を、(1) debugger の下 (debug heap。scripts/run-under-debug-heap.ps1)、
    (2) 通常の起動、の両方でくり返し、1 回ごとの結果と gdb の出力を残す。
    実行の前に、source・binary・gdb の SHA-256 を identity.json に記録する。

    -Label before : 修正の前の binary。結果を記録するだけで、合否は付けない (失敗を基準として残す)。
    -Label after  : 修正の後の binary。全部が正常に終わらなければ不成立。

    build はしない。sleep を伸ばしたり、試験の順を変えたりしない。
#>
param(
    [Parameter(Mandatory)][ValidateSet('before', 'after')][string]$Label,
    [string]$OutputDirectory = 'build/preview-audio-teardown-p35a1-20261013',
    [int]$DebugRepeats = 3,
    [int]$NormalRepeats = 2,
    [int]$TransitionPreviewDebugRepeats = 12
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo 'build\ucrt64-release\bin'
$ucrt = 'C:\msys64\ucrt64\bin'
$wrapper = Join-Path $PSScriptRoot 'run-under-debug-heap.ps1'
$fixtureA = Join-Path $repo 'tests\assets\p3_audio\p3_av_h264_aac.mp4'
$fixtureB = Join-Path $repo 'tests\assets\p3_audio\p3_video_hevc_b.mp4'
$smokeVideo = Join-Path $repo 'tests\assets\smoke\v1080p60_h264.mp4'
$smokeAudio = Join-Path $repo 'tests\assets\smoke\wav_48k.wav'
$run = Join-Path (Join-Path $repo $OutputDirectory) ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Label")
if (Test-Path -LiteralPath $run) { throw "出力先が既にあります: $run" }
New-Item -ItemType Directory -Force $run | Out-Null
$env:PATH = "$ucrt;$env:PATH"
# CTest が全部の試験に付ける環境 (tests/CMakeLists.txt): 鳴らす音を小さくし、window を画面から切り離す。
$env:MVM_TEST_AUDIO_VOLUME_SCALE = '0.25'
$env:MVM_TEST_FIXED_WINDOW = '1'

# 試験 (実行の前に固定)。どれも製品の PreviewEngine を実 device・実 endpoint で動かす。
#   p5e-remove-audio-source : 停止中に主の audio source を外す (解放後の参照が起きていた道)
#   p5e-remove-audio-readd  : 外す → 足し直す を 3 回くり返してから shutdown (P3-5A.1 で追加。無ければ飛ばす)
#   p5e-hot-swap            : 再生中に video・audio を外し、差し替えた audio も外す
#   p5e-remove-shutdown-race: audio を止めている間に shutdown が入る
#   p5e-remove-audio-stop   : audio を止められないまま外そうとする (fatal)
#   p5e-remove-fatal-event-order: 上の fatal の後、removeSource の中にいる間に teardown が最後まで進む
#   p5e-product             : 外さずに shutdown まで
#   p5d-product             : 2 つ目の audio source (mix) を外す
#   transition-preview      : controller 経由。seek のたびに audio source の集合を差し替える
$cases = @(
    @{ name = 'p5e-remove-audio-source'; exe = 'mvm_p5e_preview_smoke.exe'; args = @($fixtureA, $fixtureB, '--remove-audio-source') },
    @{ name = 'p5e-remove-audio-readd'; exe = 'mvm_p5e_preview_smoke.exe'; args = @($fixtureA, $fixtureB, '--remove-audio-readd'); optional = $true },
    @{ name = 'p5e-hot-swap'; exe = 'mvm_p5e_preview_smoke.exe'; args = @($fixtureA, $fixtureB, '--hot-swap') },
    @{ name = 'p5e-remove-shutdown-race'; exe = 'mvm_p5e_preview_smoke.exe'; args = @($fixtureA, $fixtureB, '--remove-shutdown-race') },
    @{ name = 'p5e-remove-fatal-event-order'; exe = 'mvm_p5e_preview_smoke.exe'; args = @($fixtureA, $fixtureB, '--remove-fatal-event-order') },
    @{ name = 'p5e-remove-audio-stop'; exe = 'mvm_p5e_preview_smoke.exe'; args = @($fixtureA, $fixtureB, '--fault-remove-audio-stop') },
    @{ name = 'p5e-product'; exe = 'mvm_p5e_preview_smoke.exe'; args = @($fixtureA, $fixtureB) },
    @{ name = 'p5d-product'; exe = 'mvm_p5d_preview_smoke.exe'; args = @($fixtureA) },
    @{ name = 'transition-preview'; exe = 'mvm_test_transition_preview.exe'; args = @($smokeVideo, $smokeAudio, (Join-Path $ucrt 'ffmpeg.exe')); debugRepeats = $TransitionPreviewDebugRepeats; signalOnly = $true }
)

$hashed = @('src/preview_engine/preview_engine.cpp', 'src/preview_engine/preview_engine.h',
    'src/media/audio_preview/wasapi_audio_sink.cpp', 'src/media/audio_preview/wasapi_audio_sink.h',
    'src/media/audio_preview/audio_clock.cpp', 'src/media/audio_preview/audio_clock.h',
    'apps/p5e_preview_smoke/main.cpp', 'apps/p5d_preview_smoke/main.cpp', 'tests/harness/test_transition_preview.cpp',
    'apps/mvm/mvm_controller.cpp', 'tests/CMakeLists.txt', 'scripts/run-under-debug-heap.ps1',
    'scripts/preview-audio-teardown-p35a1-verify.ps1') | ForEach-Object { Join-Path $repo $_ }
$hashed += @($cases | ForEach-Object { Join-Path $bin $_.exe } | Sort-Object -Unique)
$hashed += @((Join-Path $ucrt 'gdb.exe'), $fixtureA, $fixtureB, $smokeVideo, $smokeAudio)
$identity = [ordered]@{ label = $Label; started = (Get-Date -Format o); git_head = (git -C $repo rev-parse HEAD)
    git_status = @(git -C $repo status --porcelain); sha256 = [ordered]@{}; modified = [ordered]@{}
}
foreach ($file in $hashed) {
    $identity.sha256[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
    $identity.modified[$file] = (Get-Item -LiteralPath $file).LastWriteTime.ToString('o')
}
$identity | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'identity.json') -Encoding utf8
(git -C $repo diff HEAD) | Set-Content -LiteralPath (Join-Path $run 'git-diff.patch') -Encoding utf8

$rows = @()
$failures = 0
foreach ($case in $cases) {
    $exe = Join-Path $bin $case.exe
    $childArgs = $case.args -join '|'
    if ($case.ContainsKey('optional') -and $Label -eq 'before') {
        # 修正の前の binary には、この scenario が無い。
        Write-Host "$($case.name): 修正の前の binary には無いので飛ばす"
        continue
    }
    $debugRepeats = if ($case.ContainsKey('debugRepeats')) { $case.debugRepeats } else { $DebugRepeats }
    for ($index = 1; $index -le $debugRepeats; ++$index) {
        $log = Join-Path $run "$($case.name)-debug-$index.gdb.txt"
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        # 実時間の再生を検査する試験は、debugger の下では signal だけを見る (wrapper の説明)。
        $signalOnly = $case.ContainsKey('signalOnly')
        $extra = @()
        if ($signalOnly) { $extra += '-SignalOnly' }
        & pwsh -NoProfile -File $wrapper -Exe $exe -ChildArgs $childArgs -LogFile $log @extra *> (Join-Path $run "$($case.name)-debug-$index.txt")
        $code = $LASTEXITCODE
        $text = Get-Content -Raw -LiteralPath $log
        $signal = [regex]::Match($text, 'received signal (\S+),').Groups[1].Value
        # 止まった場所: この repository の source を指す最初の frame。
        $frame = [regex]::Match($text, '#\d+\s+0x[0-9a-f]+ in (\S+).*? at (C:/dev/soft/mvm/\S+)').Value
        $freed = $text -match '0xfeeefeee'
        $childCode = [regex]::Match($text, 'exited with code (\d+)\]').Groups[1].Value
        $rows += [pscustomobject]@{ case = $case.name; mode = 'debug-heap'; index = $index; exit_code = $code; signal = $signal
            signal_only = $signalOnly; child_exited_normally = ($text -match 'exited normally\]'); child_exit_code_octal = $childCode
            freed_pattern_in_registers = $freed; first_repository_frame = $frame; seconds = [math]::Round($clock.Elapsed.TotalSeconds, 1)
        }
        Write-Host ("{0,-26} debug heap #{1,2}: exit={2} {3} {4:n1} s" -f $case.name, $index, $code, $signal, $clock.Elapsed.TotalSeconds)
        if ($Label -eq 'after' -and $code -ne 0) { ++$failures }
    }
    for ($index = 1; $index -le $NormalRepeats; ++$index) {
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        & $exe @($case.args) *> (Join-Path $run "$($case.name)-normal-$index.txt")
        $code = $LASTEXITCODE
        $rows += [pscustomobject]@{ case = $case.name; mode = 'normal'; index = $index; exit_code = $code; signal = ''
            freed_pattern_in_registers = $false; first_repository_frame = ''; seconds = [math]::Round($clock.Elapsed.TotalSeconds, 1)
        }
        Write-Host ("{0,-26} 通常 #{1,2}: exit={2} {3:n1} s" -f $case.name, $index, $code, $clock.Elapsed.TotalSeconds)
        if ($Label -eq 'after' -and $code -ne 0) { ++$failures }
    }
}
$rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
Write-Host "結果: $run"
if ($Label -eq 'before') { Write-Host '修正の前の基準 (合否は付けない)'; exit 0 }
Write-Host "不成立: $failures"
if ($failures -gt 0) { exit 1 }
