<#
.SYNOPSIS
    P3-6C.2 の検収の道具 (scripts/math-group-product-p36c2-accept.ps1 と手順書) の自己試験。
    人による検収ではない。GUI を起動しない。mvm.exe の書き出しもしない。

.DESCRIPTION
    lint が通っても、PowerShell の script が実行できるとは限らない (構文の誤りを lint が通した例が
    ある)。ここでは、道具を実際に動かして確かめる。

      1. 構文: 検収に使う script を PowerShell の parser に通す (誤り 0)。
      2. 制御文字: 手順書・script・短い fixture に、tab・改行以外の制御文字が無い。
      3. (削除) 手順書の人向けの command の試験。手順書は人の操作を求めなくなった (2026-10-14)。
         掴んだ file への書き出しの失敗は、driver の場面 A で、実際の mvm.exe に対して確かめる。
      4. 引数と終了コード: 不正な段階、session の指定なし、存在しない session、参照の無い session の起動。
      5. session の記録: 偽の app (pwsh) を起動し、stderr の行を pipe から受け取った時刻で、書き出しの
         開始と結果を記録できること。-LaunchRealApplication なら、固定した mvm.exe も起動して強制終了する。
      6. 事後の検査 (verify): 作りものの session の記録 (synthetic_selftest = true) を与える。
         正しい記録は通り、1 か所ずつ壊した記録は、決めておいた判定だけが false になること。
         出力の MP4 は、ffmpeg で作った 30 frame の H.264 (1920x1080、60 fps) である。

    5 の記録は、この script が作ったものである。verify は「人による検収ではない」と記録する
    (human_acceptance = false)。出力先は検収の証拠と別の directory (selftest/) にする。

    終了コード: 0 = 全部が期待どおり、1 = 期待と違うものがある。
#>
param(
    [string]$OutputDirectory = 'build/math-group-product-p36c2-20261014/selftest',
    # 固定した mvm.exe を、書き出しをせずに起動し、15 秒後に強制終了して、起動の行と画面の保存を確かめる。
    # GUI は操作しない (window を閉じる操作もしない)。人の検収ではない。
    [switch]$LaunchRealApplication
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$accept = Join-Path $PSScriptRoot 'math-group-product-p36c2-accept.ps1'
$procedure = Join-Path $repo 'docs\math-group-product-p36c2-procedure.md'
$ffmpeg = 'C:\msys64\ucrt64\bin\ffmpeg.exe'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$relativeRoot = "$OutputDirectory/$stamp"
$root = Join-Path $repo $relativeRoot
New-Item -ItemType Directory -Force $root | Out-Null
$results = [System.Collections.Generic.List[object]]::new()

function Add-Result([string]$Group, [string]$Name, [bool]$Ok, [string]$Detail = '') {
    $results.Add([pscustomobject]@{ group = $Group; name = $Name; ok = $Ok; detail = $Detail })
    Write-Host ("{0} {1,-12} {2} {3}" -f $(if ($Ok) { 'ok  ' } else { 'FAIL' }), $Group, $Name, $Detail)
}

# --- 1. 構文 ---
foreach ($name in @('math-group-product-p36c2-accept.ps1', 'math-group-product-p36c2-selftest.ps1', 'math-group-product-p36c2-drive.ps1', 'math-group-product-p36b-verify.ps1',
        'math-group-product-p36a-verify.ps1', 'gui-environment-preflight.ps1')) {
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name), [ref]$null, [ref]$errors)
    Add-Result 'parse' $name ($errors.Count -eq 0) $(if ($errors.Count) { $errors[0].Message } else { '' })
}

# --- 2. 制御文字 ---
$scanned = @($accept, $PSCommandPath, (Join-Path $PSScriptRoot 'math-group-product-p36c2-drive.ps1'), $procedure, (Join-Path $repo 'docs\math-group-product-p36c2.md')) +
@(Get-ChildItem (Join-Path $repo 'build\math-group-product-p36c2-20261014\fixtures') -Filter *.mvm -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
foreach ($file in $scanned) {
    $text = [IO.File]::ReadAllText($file, [Text.UTF8Encoding]::new($false, $true))
    $found = @()
    $line = 1
    foreach ($character in $text.ToCharArray()) {
        $code = [int]$character
        if ($code -eq 10) { ++$line; continue }
        if (($code -lt 32 -and $code -ne 9 -and $code -ne 13) -or $code -eq 127 -or ($code -ge 128 -and $code -lt 160) -or
            $code -in @(0xFEFF, 0x200B, 0x200E, 0x200F, 0x2028, 0x2029, 0xFFFD)) {
            $found += ('行 {0} U+{1:X4}' -f $line, $code)
        }
    }
    Add-Result 'control' (Split-Path -Leaf $file) ($found.Count -eq 0) (($found | Select-Object -First 3) -join ', ')
}

# --- 4・5 の準備: 対象の固定 (本物)、作りものの出力と参照 ---
function Invoke-Accept([string[]]$Arguments, [string]$Log) {
    $all = @('-NoProfile', '-File', $accept) + $Arguments
    pwsh @all *> $Log
    $LASTEXITCODE
}

$code = Invoke-Accept @('-Stage', 'nonsense', '-OutputDirectory', $relativeRoot) (Join-Path $root 'args-bad-stage.log')
Add-Result 'arguments' '不正な段階は失敗する' ($code -ne 0) "exit=$code"
$code = Invoke-Accept @('-Stage', 'verify', '-OutputDirectory', $relativeRoot) (Join-Path $root 'args-no-freeze.log')
Add-Result 'arguments' 'freeze.json が無ければ失敗する' ($code -ne 0) "exit=$code"
$code = Invoke-Accept @('-Stage', 'freeze', '-OutputDirectory', $relativeRoot) (Join-Path $root 'freeze.log')
Add-Result 'arguments' 'freeze を作れる' ($code -eq 0 -and (Test-Path (Join-Path $root 'freeze.json'))) "exit=$code"
$code = Invoke-Accept @('-Stage', 'freeze', '-OutputDirectory', $relativeRoot) (Join-Path $root 'freeze-again.log')
Add-Result 'arguments' 'freeze は上書きしない' ($code -ne 0) "exit=$code"
$code = Invoke-Accept @('-Stage', 'session', '-Project', 'v4', '-OutputDirectory', $relativeRoot) (Join-Path $root 'args-session-no-reference.log')
Add-Result 'arguments' '参照が無ければ session を起動しない' ($code -ne 0 -and -not (Get-Process mvm -ErrorAction SilentlyContinue)) "exit=$code"

$media = Join-Path $root 'media'
New-Item -ItemType Directory -Force $media | Out-Null
function New-Clip([string]$Name, [int]$Frames, [string]$Color, [int]$Crf) {
    $path = Join-Path $media $Name
    & $ffmpeg -hide_banner -nostdin -v error -y -f lavfi -i "color=c=${Color}:s=1920x1080:r=60" -frames:v $Frames -c:v libx264 -profile:v high `
        -pix_fmt yuv420p -crf $Crf -video_track_timescale 15360 $path
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg で $Name を作れません" }
    $path
}
$good = New-Clip 'good.mp4' 30 'black' 23
$other = New-Clip 'other-content.mp4' 30 'white' 30
$short = New-Clip 'short-29-frames.mp4' 29 'black' 23
$broken = Join-Path $media 'broken.mp4'
Set-Content -LiteralPath $broken -Value 'これは MP4 ではない'
$goodSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $good).Hash
$goodRelative = "$relativeRoot/media/good.mp4"
[ordered]@{ created = (Get-Date -Format 'o'); synthetic_selftest = $true
    fixtures = [ordered]@{
        'short-math-only-eligible/on' = [ordered]@{ output_sha256 = $goodSha.ToLower(); frames = 30 }
        'short-fallback-unknown/on'   = [ordered]@{ output_sha256 = $goodSha.ToLower(); frames = 30 }
    }
    v4 = [ordered]@{ sha256 = $goodSha; frames = 30; historical = @() }
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'reference.json') -Encoding utf8

$code = Invoke-Accept @('-Stage', 'verify', '-OutputDirectory', $relativeRoot) (Join-Path $root 'args-verify-no-session.log')
Add-Result 'arguments' 'session の指定が無ければ verify は失敗する' ($code -ne 0) "exit=$code"
$code = Invoke-Accept @('-Stage', 'verify', '-Session', 'no-such-session', '-OutputDirectory', $relativeRoot) (Join-Path $root 'args-verify-missing.log')
Add-Result 'arguments' '存在しない session の verify は失敗する' ($code -ne 0) "exit=$code"

# --- 5. session の記録 (pipe から受け取った時刻) ---
$fake = Join-Path $root 'fake-mvm.ps1'
@'
param([string]$Output)
$stream = [Console]::OpenStandardError()
function Send([string]$Text) { $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text + "`n"); $stream.Write($bytes, 0, $bytes.Length); $stream.Flush() }
Send 'Projectを開きます: fake'
Start-Sleep -Milliseconds 800
Send "書き出しを始めます: $Output (数式の高速化: 選択あり)"
Send '[libx264 @ 0000] noise'
Start-Sleep -Seconds 3
Set-Content -LiteralPath $Output -Value 'x'
Send "書き出しの結果: 成功 frame=30 error='' | 数式の高速化: outcome=used decision=ELIGIBLE reason=base-alpha-proven graph=math-group groups=1 grouped_clips=2 affine=0 composed=30 callbacks=30 errors=0 rasters=2 raster_bytes=10 preparation_rejected=0 live_transitions=0 active_callbacks=0"
Start-Sleep -Milliseconds 600
'@ | Set-Content -LiteralPath $fake -Encoding utf8
$fakeOutput = Join-Path $root 'fake-output.mp4'
$pwshPath = (Get-Process -Id $PID).Path
# 引数の先頭の | は、値が - で始まって option と解釈されないようにするため (空の要素は捨てられる)。
$code = Invoke-Accept @('-Stage', 'session', '-Project', 'v4', '-Label', 'pipe-capture', '-OutputDirectory', $relativeRoot, '-SkipEnvironmentPreflight',
    '-ScreenshotSeconds', '0', '-ApplicationOverride', $pwshPath, '-ApplicationArgumentsOverride', "|-NoProfile|-File|$fake|$fakeOutput") (Join-Path $root 'session-pipe-capture.log')
$sessionDirectory = Get-ChildItem $root -Directory -Filter '*-session-v4-pipe-capture' | Select-Object -First 1
$record = if ($sessionDirectory -and (Test-Path -LiteralPath (Join-Path $sessionDirectory.FullName 'session.json'))) { Get-Content -Raw (Join-Path $sessionDirectory.FullName 'session.json') | ConvertFrom-Json } else { $null }
$captured = $record -and @($record.exports).Count -eq 1
Add-Result 'session' '偽の app の書き出しの開始と結果を、両方記録する' ($code -eq 0 -and $captured) "exit=$code"
if ($captured) {
    $export = @($record.exports)[0]
    Add-Result 'session' '経過は、行を受け取った時刻の差 (約 3 秒)' ($export.elapsed_seconds -ge 2.7 -and $export.elapsed_seconds -le 4.5) ('{0:n2} 秒' -f $export.elapsed_seconds)
    Add-Result 'session' '結果の行の値を読む' ($export.record.outcome -eq 'used' -and $export.record.graph -eq 'math-group' -and $export.record.composed -eq '30' -and
        $export.final_file_exists -eq $true)
    Add-Result 'session' '書き出しの間の操作を記録する' ($export.quiet.samples -gt 0 -and $null -ne $export.quiet.input_changes) "samples=$($export.quiet.samples)"
    Add-Result 'session' '解析しない行も log に残す' ((Get-Content -Raw (Join-Path $sessionDirectory.FullName 'mvm-stderr.log')) -match 'libx264 @ 0000')
    Add-Result 'session' '固定した mvm.exe ではないと記録する' ($record.application_matches_freeze -eq $false -and $record.application_override -eq $true -and $record.capture -eq 'pipe')
}

if ($LaunchRealApplication) {
    $realLog = Join-Path $root 'session-real-launch.log'
    $launcher = Start-Job -ScriptBlock {
        pwsh -NoProfile -File $using:accept -Stage session -Project short-math-only-eligible -Label real-launch-killed `
            -OutputDirectory $using:relativeRoot -SkipEnvironmentPreflight *> $using:realLog
        $LASTEXITCODE
    }
    $application = $null
    for ($wait = 0; $wait -lt 100 -and -not $application; ++$wait) { Start-Sleep -Milliseconds 200; $application = Get-Process mvm -ErrorAction SilentlyContinue | Select-Object -First 1 }
    Start-Sleep -Seconds 15
    if ($application) { Stop-Process -Id $application.Id -Force }
    $launchCode = Receive-Job -Job $launcher -Wait -AutoRemoveJob
    $realDirectory = Get-ChildItem $root -Directory -Filter '*-session-short-math-only-eligible-real-launch-killed' | Select-Object -First 1
    $real = if ($realDirectory -and (Test-Path -LiteralPath (Join-Path $realDirectory.FullName 'session.json'))) { Get-Content -Raw (Join-Path $realDirectory.FullName 'session.json') | ConvertFrom-Json } else { $null }
    Add-Result 'session' '固定した mvm.exe を起動して記録できる' ([bool]$application -and $real -and $real.application_matches_freeze -eq $true) "exit=$launchCode"
    if ($real) {
        $firstLine = Get-Content -LiteralPath (Join-Path $realDirectory.FullName 'mvm-stderr.log') -TotalCount 1
        Add-Result 'session' 'mvm.exe の起動の行を pipe から受け取る' ("$firstLine" -like 'Projectを開きます: *')
        Add-Result 'session' '読み込まれた DLL が freeze と一致する' (@($real.loaded_modules_not_matching_freeze).Count -eq 0 -and $real.loaded_module_count -gt 0) "DLL $($real.loaded_module_count)"
        Add-Result 'session' 'window の内容を保存する (PrintWindow)' ($real.screenshots -gt 0) "画面 $($real.screenshots) 枚"
    }
}

# --- 6. 事後の検査 ---
$settingsBase = [ordered]@{ 'HKEY_CURRENT_USER\Software\mvm\mvm\workspace\leftPanelWidth' = '560'
    'HKEY_CURRENT_USER\Software\mvm\mvm\transcribe\language' = 'ja'
}
function Copy-Map($Map) {
    $copy = [ordered]@{}
    foreach ($key in $Map.Keys) { $copy[$key] = $Map[$key] }
    $copy
}

function New-Session([string]$Name, [string]$Project, [scriptblock]$Change) {
    $directory = Join-Path $root $Name
    New-Item -ItemType Directory -Force $directory | Out-Null
    $output = Join-Path $directory 'export.mp4'
    Copy-Item -LiteralPath $good -Destination $output
    $export = [ordered]@{ output = $output; selected = $true; start_seconds = 10.0; finish_seconds = 110.0; elapsed_seconds = 100.0
        final_file_exists = $true; temporary_file_exists = $false; media_left = @()
        quiet = [ordered]@{ samples = 2500; input_changes = 0; foreground_other = 0 }
        record = [ordered]@{ kind = 'result'; result = '成功'; frames = 30; error = ''; outcome = 'used'; decision = 'ELIGIBLE'
            reason = 'base-alpha-proven'; graph = 'math-group'; groups = '2'; grouped_clips = '97'; affine = '23'; composed = '60'; callbacks = '61'
            errors = '0'; rasters = '81'; raster_bytes = '2947112'; preparation_rejected = '0'; live_transitions = '0'; active_callbacks = '0'
        }
    }
    if ($Project -ne 'v4') {
        $export.record.grouped_clips = '16'
    }
    $record = [ordered]@{ stage = 'session'; synthetic_selftest = $true; project = $Project; environment_before = 'VALID'
        application_matches_freeze = $true; loaded_module_count = 205; loaded_modules_not_matching_freeze = @(); foreign_modules = @()
        exports = @($export); settings_before = (Copy-Map $settingsBase); settings_after = (Copy-Map $settingsBase)
        identity_before = [ordered]@{ match = $true }; identity_after = [ordered]@{ match = $true }
    }
    # 変える対象: r = session の記録、e = 1 つ目の書き出し、d = session の directory。
    if ($Change) { & $Change ([pscustomobject]@{ r = $record; e = $export; d = $directory }) }
    $record | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $directory 'session.json') -Encoding utf8
}

$mathGroupKey = 'HKEY_CURRENT_USER\Software\mvm\mvm\export\mathGroupOptimization'
$cases = @(
    @{ name = 'valid-v4'; project = 'v4'; fails = @(); change = $null },
    @{ name = 'valid-short-fixture'; project = 'short-math-only-eligible'; fails = @(); change = $null },
    # 数式の高速化と関係のない書き出しの設定は、あっても、session の間に変わっても、不成立にしない。
    @{ name = 'unrelated-export-setting-present-and-changed'; project = 'v4'; fails = @(); change = {
            param($c) $c.r.settings_before['HKEY_CURRENT_USER\Software\mvm\mvm\export\lastQuality'] = 'standard'
            $c.r.settings_after['HKEY_CURRENT_USER\Software\mvm\mvm\export\lastQuality'] = 'high'
            $c.r.settings_after['HKEY_CURRENT_USER\Software\mvm\mvm\workspace\leftPanelWidth'] = '600' }
    },
    @{ name = 'math-group-selection-persisted-new-key'; project = 'v4'; fails = @('math_group_selection_not_persisted'); change = {
            param($c) $c.r.settings_after[$mathGroupKey] = 'true' }
    },
    @{ name = 'math-group-selection-already-persisted'; project = 'v4'; fails = @('math_group_selection_not_persisted'); change = {
            param($c) $c.r.settings_before[$mathGroupKey] = 'true'; $c.r.settings_after[$mathGroupKey] = 'true' }
    },
    @{ name = 'accelerate-key-removed-during-session'; project = 'v4'; fails = @('math_group_selection_not_persisted'); change = {
            param($c) $c.r.settings_before['HKEY_CURRENT_USER\Software\mvm\mvm\exportDialog\accelerateMath'] = 'true' }
    },
    @{ name = 'settings-snapshot-missing'; project = 'v4'; fails = @('math_group_selection_not_persisted'); change = {
            param($c) $c.r.Remove('settings_before'); $c.r.Remove('settings_after') }
    },
    # driver が操作した session: driver の各段が判定に入る。環境と書き出しの間の操作は参考 (判定にしない)。
    @{ name = 'driven-all-steps-ok'; project = 'v4'; fails = @(); change = {
            param($c) $c.r.driven = $true; $c.r.drive = [ordered]@{ all_ok = $true; status = 'completed'; steps = @([ordered]@{ step = 'C5'; ok = $true }) }
            $c.r.environment_before = 'ENVIRONMENT_INVALID'; $c.e.quiet.input_changes = 5; $c.e.quiet.foreground_other = 9 }
    },
    @{ name = 'driven-step-failed'; project = 'v4'; fails = @('automated_gui_operations'); change = {
            param($c) $c.r.driven = $true; $c.r.drive = [ordered]@{ all_ok = $false; status = 'completed'; steps = @([ordered]@{ step = 'C6'; ok = $false }) } }
    },
    @{ name = 'driven-without-result'; project = 'short-math-only-eligible'; fails = @('automated_gui_operations'); change = {
            param($c) $c.r.driven = $true; $c.r.drive = $null }
    },
    @{ name = 'user-input-during-export'; project = 'v4'; fails = @('quiet_during_export'); change = { param($c) $c.e.quiet.input_changes = 3 } },
    @{ name = 'other-window-in-front-during-export'; project = 'v4'; fails = @('quiet_during_export'); change = { param($c) $c.e.quiet.foreground_other = 40 } },
    @{ name = 'quiet-not-recorded'; project = 'v4'; fails = @('quiet_during_export'); change = { param($c) $c.e.Remove('quiet') } },
    @{ name = 'over-budget'; project = 'v4'; fails = @('within_budget'); change = { param($c) $c.e.elapsed_seconds = 600.5; $c.e.finish_seconds = 610.5 } },
    @{ name = 'wrong-content'; project = 'v4'; fails = @('sha256_matches_reference', 'encoder_configuration'); change = {
            param($c) Copy-Item -LiteralPath $other -Destination $c.e.output -Force }
    },
    @{ name = 'frame-missing'; project = 'v4'; fails = @('output_valid', 'sha256_matches_reference'); change = {
            param($c) Copy-Item -LiteralPath $short -Destination $c.e.output -Force }
    },
    @{ name = 'not-a-video'; project = 'v4'; fails = @('output_valid', 'sha256_matches_reference', 'encoder_configuration'); change = {
            param($c) Copy-Item -LiteralPath $broken -Destination $c.e.output -Force }
    },
    @{ name = 'conventional-graph-reported'; project = 'v4'; fails = @('math_group_selected'); change = {
            param($c) $c.e.record.outcome = 'conventional'; $c.e.record.graph = 'conventional'; $c.e.record.decision = 'UNKNOWN'; $c.e.record.grouped_clips = '0' }
    },
    @{ name = 'zero-composition'; project = 'v4'; fails = @('math_group_selected'); change = { param($c) $c.e.record.composed = '0' } },
    @{ name = 'runtime-error-counted'; project = 'v4'; fails = @('no_runtime_errors'); change = { param($c) $c.e.record.errors = '1' } },
    @{ name = 'transition-left'; project = 'v4'; fails = @('teardown'); change = { param($c) $c.e.record.live_transitions = '1' } },
    @{ name = 'temporary-file-left'; project = 'v4'; fails = @('exports_clean'); change = {
            param($c) Set-Content -LiteralPath ($c.e.output + '.mvmtmp') -Value 'left' }
    },
    @{ name = 'checkbox-not-selected'; project = 'v4'; fails = @('full_length_completed'); change = { param($c) $c.e.selected = $false } },
    @{ name = 'export-failed-and-left-temporary'; project = 'v4'; fails = @('exports_clean', 'full_length_completed'); change = {
            param($c) $c.e.record.result = '失敗'; $c.e.record.error = 'x'; $c.e.temporary_file_exists = $true }
    },
    @{ name = 'no-exports'; project = 'v4'; fails = @('exports_clean', 'full_length_completed'); change = { param($c) $c.r.exports = @() } },
    @{ name = 'other-executable'; project = 'v4'; fails = @('application_is_frozen_executable'); change = { param($c) $c.r.application_matches_freeze = $false } },
    @{ name = 'runtime-dll-differs'; project = 'v4'; fails = @('runtime_modules_match_freeze'); change = {
            param($c) $c.r.loaded_modules_not_matching_freeze = @([ordered]@{ path = 'C:\msys64\ucrt64\bin\libmlt-7.dll'; sha256 = '00'; frozen = $true; matches_freeze = $false }) }
    },
    @{ name = 'environment-invalid'; project = 'v4'; fails = @('environment_valid'); change = { param($c) $c.r.environment_before = 'ENVIRONMENT_INVALID' } },
    @{ name = 'identity-changed-during-session'; project = 'v4'; fails = @('identity_frozen_during_session'); change = {
            param($c) $c.r.identity_after = [ordered]@{ match = $false } }
    },
    @{ name = 'short-fixture-wrong-content'; project = 'short-fallback-unknown'; fails = @('short_outputs_match_reference'); change = {
            param($c) Copy-Item -LiteralPath $other -Destination $c.e.output -Force }
    },
    @{ name = 'short-fixture-cancel-clean'; project = 'short-fallback-unknown'; fails = @(); change = {
            param($c) $cancelled = [ordered]@{ output = (Join-Path $c.d 'cancelled.mp4'); selected = $true; start_seconds = 120.0; finish_seconds = 123.0
                elapsed_seconds = 3.0; final_file_exists = $false; temporary_file_exists = $false; media_left = @()
                record = (Copy-Map $c.e.record) }
            $cancelled.record.result = '取消'; $cancelled.record.outcome = 'incomplete'
            $c.r.exports = @($c.e, $cancelled) }
    }
)
foreach ($case in $cases) {
    New-Session $case.name $case.project $case.change
    $code = Invoke-Accept @('-Stage', 'verify', '-Session', $case.name, '-OutputDirectory', $relativeRoot, '-TotalFrames', '30',
        '-HistoricalOutputs', $goodRelative) (Join-Path $root "$($case.name)\verify.log")
    $verifyPath = Join-Path $root "$($case.name)\verify.json"
    if (-not (Test-Path -LiteralPath $verifyPath)) { Add-Result 'verify' $case.name $false "verify.json がありません (exit=$code)"; continue }
    $verify = Get-Content -Raw $verifyPath | ConvertFrom-Json
    $failed = @($verify.verdicts.PSObject.Properties | Where-Object { -not $_.Value } | ForEach-Object { $_.Name } | Sort-Object)
    $expected = @($case.fails | Sort-Object)
    $ok = (($failed -join ',') -eq ($expected -join ',')) -and ($code -eq $(if ($expected.Count) { 1 } else { 0 })) -and
    $verify.all_ok -eq ($expected.Count -eq 0) -and $verify.synthetic_selftest -eq $true -and $verify.human_acceptance -eq $false
    if ($case.name -eq 'driven-all-steps-ok') {
        # 参考の項目として残り、判定には入らない。
        $ok = $ok -and $verify.driven -eq $true -and $verify.information.environment_valid -eq $false -and $verify.information.quiet_during_export -eq $false
    }
    Add-Result 'verify' $case.name $ok ("exit={0} false になった判定=[{1}] 期待=[{2}]" -f $code, ($failed -join ','), ($expected -join ','))
}
# 記録の項目が欠けていれば、判定を作らずに失敗する。
New-Session 'record-field-missing' 'v4' { param($c) $c.r.Remove('exports') }
$code = Invoke-Accept @('-Stage', 'verify', '-Session', 'record-field-missing', '-OutputDirectory', $relativeRoot, '-TotalFrames', '30',
    '-HistoricalOutputs', $goodRelative) (Join-Path $root 'record-field-missing\verify.log')
Add-Result 'verify' 'record-field-missing' ($code -ne 0 -and -not (Test-Path (Join-Path $root 'record-field-missing\verify.json'))) "exit=$code"

$failures = @($results | Where-Object { -not $_.ok }).Count
[ordered]@{ created = (Get-Date -Format 'o'); note = '道具の自己試験。人による検収ではない。GUI は起動していない。'
    total = $results.Count; failures = $failures; results = $results
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $root 'selftest.json') -Encoding utf8
Write-Host "結果: $(Join-Path $root 'selftest.json')"
Write-Host "検査 $($results.Count)、不成立 $failures"
if ($failures -gt 0) { exit 1 }
