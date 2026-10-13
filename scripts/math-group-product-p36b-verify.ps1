<#
.SYNOPSIS
    P3-6B: 製品の書き出しの入口 (MvmController → 書き出しの要求 → mvm::app::exportTimeline) から、
    数式 group の選択 (Auto) で v4 の Project 全体 (6840 frame) を書き出し、出力を確かめる。

.DESCRIPTION
    段階 (-Stage):
      freeze   source・binary・runtime・Project・数式の成果物・この script の SHA-256 を freeze.json へ書く。
               既にあれば書き直さない (-OutputDirectory を変えて作り直す)。
      focused  製品の入口からの選択と fallback の試験。
               (1) P3-6A の場合 (scripts/math-group-product-p36a-verify.ps1) をそのまま回す。
               (2) P3-6A.1: 数式 group 用の画像の上限を超えたとき、Auto は従来の graph で書き出す
                   (上限は診断用の observer から byte 単位で渡す。製品の上限 64 MB そのものは CTest の
                   math_export_focused が確かめる)。
               (3) --product-pure: 製品の要求を 1 つも書き換えない書き出し (observer・pre-YUV の計測・
                   MLT の計時 hook・FFmpeg の記録の tap を付けない)。ELIGIBLE・UNKNOWN・INELIGIBLE と、
                   それぞれの選択 off。選択 off と on の MP4 を SHA-256 で比べる。
      full     v4 の Project 全体を --product-pure、選択 on で 1 回書き出す。出力の MP4 を、harness の集計を
               使わずに確かめる (ffprobe・ffmpeg)。P3-4・P3-5A の診断用の全長の出力と SHA-256 を比べる。
               画素 (encoder の直前の RGBA) は、この段階では比べない。

    focused と full は、実行の前と後に freeze.json と照合する。build はしない。source を書き換えない。
    期待は実行の前にこの script に書いてある。

    watchdog (製品の timeout 600000 ms とは別):
      harness の中: -CapSeconds (既定 900 秒) で取消を送る。
      この script: -WatchdogSeconds (既定 1500 秒) で process を止める。
    検収の基準 (書き出しの全体が 600 秒以内) は -BudgetSeconds で、watchdog とは別に判定する。
#>
param(
    [Parameter(Mandatory)][ValidateSet('freeze', 'focused', 'full')][string]$Stage,
    [string]$Label = '',
    [string]$ProjectPath = 'build/quadratic-semantic-20261011-v4/quadratic.mvm',
    [string]$OutputDirectory = 'build/math-group-product-p36b-20261014',
    [string[]]$HistoricalOutputs = @(
        'build/math-export-p34-20261013/20261013-011412-full/candidate/v4-full.mp4',
        'build/math-export-p34-20261013/20261013-021747-full-repeat1/candidate/v4-full.mp4',
        'build/math-export-p34-20261013/20261013-023241-full-repeat2/candidate/v4-full.mp4',
        'build/math-export-p35a-20261013/20261013-024935-strong/candidate-full/v4-full.mp4'),
    [int]$TotalFrames = 6840,
    [double]$BudgetSeconds = 600,
    [int]$CapSeconds = 900,
    [int]$WatchdogSeconds = 1500,
    # script の試走用: P3-6A の場合を省く (検収の run では使わない。summary に残る)。
    [switch]$SkipP36aCases
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$ucrt = 'C:\msys64\ucrt64\bin'
$bin = Join-Path $repo 'build\ucrt64-release\bin'
$harness = Join-Path $bin 'mvm_math_score_export_parity.exe'
$manim = Join-Path $env:USERPROFILE '.local\bin\manim.exe'
$latex = Join-Path $env:LOCALAPPDATA 'Programs\MiKTeX\miktex\bin\x64'
$ffprobe = Join-Path $ucrt 'ffprobe.exe'
$ffmpeg = Join-Path $ucrt 'ffmpeg.exe'
$project = Join-Path $repo $ProjectPath
$root = Join-Path $repo $OutputDirectory
$freezePath = Join-Path $root 'freeze.json'
New-Item -ItemType Directory -Force $root | Out-Null

function Get-Sha([string]$Path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash }

function Get-IdentityFiles {
    $files = [System.Collections.Generic.List[string]]::new()
    # source: 追跡している src・apps・tests・CMake と、追跡していない src・apps・tests。
    foreach ($relative in (git -C $repo ls-files -- src apps tests CMakeLists.txt cmake)) { $files.Add((Join-Path $repo $relative)) }
    foreach ($relative in (git -C $repo ls-files --others --exclude-standard -- src apps tests)) { $files.Add((Join-Path $repo $relative)) }
    # 検証の script。
    foreach ($relative in @('scripts/math-group-product-p36b-verify.ps1', 'scripts/math-group-product-p36a-verify.ps1')) {
        $files.Add((Join-Path $repo $relative))
    }
    # binary: 製品の controller と書き出しを link した検証 harness (全長の書き出しを行う process)。
    $files.Add($harness)
    # runtime: MLT (本体と module)、Qt、FFmpeg の library、x264、ffprobe・ffmpeg。
    foreach ($name in @('libmlt-7.dll', 'libmlt++-7.dll', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Qml.dll', 'Qt6Quick.dll', 'avcodec-62.dll',
            'avformat-62.dll', 'avutil-60.dll', 'swscale-9.dll', 'swresample-6.dll', 'libx264-165.dll', 'ffprobe.exe', 'ffmpeg.exe')) {
        $files.Add((Join-Path $ucrt $name))
    }
    foreach ($module in (Get-ChildItem 'C:\msys64\ucrt64\lib\mlt' -Filter *.dll)) { $files.Add($module.FullName) }
    # Project、数式の成果物の cache (stage と、数式 group 用の画像の準備の入力)、Project が絶対 path で参照する素材。
    # cache の jobs\ は、実行のたびに書き直される記録なので除く。
    $files.Add($project)
    $cache = Join-Path (Split-Path -Parent $project) 'cache'
    foreach ($file in (Get-ChildItem $cache -Recurse -File | Where-Object { $_.FullName -notmatch '[\\/]jobs[\\/]' })) { $files.Add($file.FullName) }
    foreach ($match in [regex]::Matches((Get-Content -Raw $project), '"([A-Za-z]:(?:\\|/)[^"]+)"')) {
        $media = $match.Groups[1].Value.Replace('\\', '\')
        if (Test-Path -LiteralPath $media -PathType Leaf) { $files.Add((Get-Item -LiteralPath $media).FullName) }
    }
    @($files | Sort-Object -Unique)
}

function Get-SourceDiffSha {
    # 文書と build の成果物を除いた、HEAD からの差分 (source の状態を 1 つの値で表す)。
    $diff = (git -C $repo diff HEAD -- src apps tests scripts CMakeLists.txt cmake) -join "`n"
    $bytes = [Text.Encoding]::UTF8.GetBytes($diff)
    [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData($bytes)).Replace('-', '')
}

if ($Stage -eq 'freeze') {
    if (Test-Path -LiteralPath $freezePath) { throw "freeze.json が既にあります: $freezePath" }
    $hashes = [ordered]@{}
    foreach ($file in (Get-IdentityFiles)) { $hashes[$file] = Get-Sha $file }
    (git -C $repo diff HEAD -- src apps tests scripts CMakeLists.txt cmake) | Set-Content -LiteralPath (Join-Path $root 'freeze-git-diff.patch') -Encoding utf8
    [ordered]@{
        created = (Get-Date -Format 'o'); git_head = (git -C $repo rev-parse HEAD); git_status = @(git -C $repo status --porcelain)
        source_diff_sha256 = (Get-SourceDiffSha)
        ffmpeg_version = (& $ffmpeg -hide_banner -version | Select-Object -First 1)
        melt_version = (& (Join-Path $ucrt 'melt.exe') --version 2>&1 | Select-Object -First 1)
        # encoder の設定は製品の要求が決める (全長の run の記録の request と、出力の x264 の設定に残る)。
        note = 'mvm.exe は scripts/test.ps1 が実行のたびに link し直すので含めない。全長の書き出しは検証 harness の process が行う。'
        sha256 = $hashes
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $freezePath -Encoding utf8
    Write-Host "freeze: $($hashes.Count) file → $freezePath"
    exit 0
}

if (-not (Test-Path -LiteralPath $freezePath)) { throw "freeze.json がありません: $freezePath" }
$frozen = Get-Content -Raw $freezePath | ConvertFrom-Json -AsHashtable

function Test-Frozen([string]$When) {
    $changed = @($frozen.sha256.Keys | Where-Object { -not (Test-Path -LiteralPath $_) -or (Get-Sha $_) -ne $frozen.sha256[$_] })
    $added = @(Get-IdentityFiles | Where-Object { -not $frozen.sha256.ContainsKey($_) })
    $diffSame = (Get-SourceDiffSha) -eq $frozen.source_diff_sha256
    $headSame = (git -C $repo rev-parse HEAD) -eq $frozen.git_head
    foreach ($file in $changed) { Write-Host "変わった: $file" }
    foreach ($file in $added) { Write-Host "増えた: $file" }
    $ok = $changed.Count -eq 0 -and $added.Count -eq 0 -and $diffSame -and $headSame
    Write-Host ("freeze.json との照合 ({0}): {1} ({2} file)" -f $When, $(if ($ok) { '一致' } else { '不一致' }), $frozen.sha256.Count)
    [pscustomobject]@{ when = $When; match = $ok; files = $frozen.sha256.Count; changed = $changed; added = $added; source_diff_same = $diffSame; head_same = $headSame }
}

$before = Test-Frozen '前'
if (-not $before.match) { throw 'freeze.json と一致しません' }
$env:PATH = "$ucrt;$latex;$env:PATH"
$env:MVM_TEST_FIXED_WINDOW = '1'
$run = Join-Path $root ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Stage" + $(if ($Label) { "-$Label" } else { '' }))
if (Test-Path -LiteralPath $run) { throw "出力先が既にあります: $run" }
New-Item -ItemType Directory -Force $run | Out-Null
$failures = 0

function Invoke-Harness([string]$Name, [string]$Fixture, [string[]]$Extra, [string]$ReportSuffix = 'report') {
    $out = Join-Path $run $Name
    New-Item -ItemType Directory -Force $out | Out-Null
    $arguments = @('--project', $project, '--manim', $manim, '--out', $out, '--ffprobe', $ffprobe, '--fixture', $Fixture,
        '--cap-seconds', "$CapSeconds") + $Extra
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $harness -ArgumentList $arguments -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $out 'harness.out') -RedirectStandardError (Join-Path $out 'harness.log')
    $null = $process.Handle # 終了コードを読めるように handle を保持する
    $killed = $false
    if (-not $process.WaitForExit($WatchdogSeconds * 1000)) {
        $killed = $true
        try { $process.Kill($true) } catch { Write-Verbose "$_" }
        $process.WaitForExit()
    }
    $clock.Stop()
    $reportPath = Join-Path $out "$Fixture-$ReportSuffix.json"
    [pscustomobject]@{ out = $out; exit_code = $process.ExitCode; watchdog_killed = $killed; process_wall_seconds = $clock.Elapsed.TotalSeconds
        report = if (Test-Path -LiteralPath $reportPath) { Get-Content -Raw $reportPath | ConvertFrom-Json } else { $null }
    }
}

# 製品の要求が書き換えられていないこと (--product-pure の記録)。
function Test-PureRequest($Report, [bool]$Selected) {
    $request = $Report.request
    $request.math_group_optimization -eq $(if ($Selected) { 'auto' } else { 'off' }) -and $request.export_observer_supplied -eq $false -and
    $request.encoder_frame_validator_supplied -eq $false -and $request.stage_observer_supplied -eq $false -and
    $request.math_staging_observer_supplied -eq $false -and $request.timeout_ms -eq 600000 -and $request.render_threads -eq 4 -and
    $request.encoder_threads -eq 0 -and $request.video_crf -eq 23 -and $request.width -eq 1920 -and $request.height -eq 1080 -and
    $request.fps_num -eq 60 -and $request.fps_den -eq 1 -and $request.lossless_audio -eq $false -and
    $Report.controller_supplied_observer -eq $false -and $Report.harness_property_probe_hooks -eq 0 -and
    $Report.controller_default_selection -eq $false
}

function Get-MediaLeft([string]$Directory) {
    @(Get-ChildItem $Directory -File | Where-Object { $_.Name -match '\.(mvmtmp|tmp|part)$' } | ForEach-Object { $_.Name })
}

if ($Stage -eq 'focused') {
    # (1) P3-6A の場合 (22 + MP4 2 組 + 主の書き出し 5)。同じ script を、この phase の出力先で回す。
    $p36aLog = Join-Path $run 'p36a-cases.log'
    if ($SkipP36aCases) { 'skipped' | Set-Content -LiteralPath $p36aLog }
    else {
        pwsh (Join-Path $PSScriptRoot 'math-group-product-p36a-verify.ps1') -Label 'p36a-cases' -OutputDirectory (Join-Path $OutputDirectory (Split-Path -Leaf $run)) *> $p36aLog
    }
    $p36aExit = if ($SkipP36aCases) { -1 } else { $LASTEXITCODE }
    $p36aSummaryPath = Get-ChildItem $run -Directory -Filter '*-p36a-cases' | ForEach-Object { Join-Path $_.FullName 'summary.json' } | Select-Object -First 1
    $p36a = if ($p36aSummaryPath -and (Test-Path -LiteralPath $p36aSummaryPath)) { Get-Content -Raw $p36aSummaryPath | ConvertFrom-Json } else { $null }
    $p36aCounts = if ($p36a) {
        [ordered]@{ exit_code = $p36aExit
            candidates = @($p36a.candidates).Count; candidates_as_expected = @($p36a.candidates | Where-Object { $_.as_expected }).Count
            outputs = @($p36a.outputs).Count; outputs_same = @($p36a.outputs | Where-Object { $_.same }).Count
            main = @($p36a.main).Count; main_as_expected = @($p36a.main | Where-Object { $_.as_expected }).Count
            frames_exact = (@($p36a.candidates | Where-Object { $_.expect -eq 'exact' } | ForEach-Object { $_.frames_exact }) | Measure-Object -Sum).Sum
        }
    } else { [ordered]@{ exit_code = $p36aExit } }
    $p36aOk = $SkipP36aCases -or ($p36aExit -eq 0 -and $p36a -and $p36aCounts.candidates -eq 22 -and $p36aCounts.candidates_as_expected -eq 22 -and
        $p36aCounts.outputs -eq 2 -and $p36aCounts.outputs_same -eq 2 -and $p36aCounts.main -eq 5 -and $p36aCounts.main_as_expected -eq 5)
    if (-not $p36aOk) { ++$failures }
    Write-Host ("P3-6A の場合: exit={0} candidate {1}/{2}、MP4 {3}/{4}、主の書き出し {5}/{6} 期待どおり={7}" -f $p36aExit,
        $p36aCounts['candidates_as_expected'], $p36aCounts['candidates'], $p36aCounts['outputs_same'], $p36aCounts['outputs'],
        $p36aCounts['main_as_expected'], $p36aCounts['main'], [bool]$p36aOk)

    # (2) P3-6A.1: 画像の上限 (100000 byte。16 層の準備済みの画像は 14 枚で 220424 byte)。
    $budget = @('--compare', 'product-math-group', '--product-observer', 'diagnostic', '--math-group-budget-bytes', '100000')
    $budgetCases = @(
        @{ name = 'budget-otherwise-eligible'; fixture = 'cmp-layers-16' },
        @{ name = 'budget-otherwise-unknown'; fixture = 'base-transformed' },
        @{ name = 'budget-otherwise-ineligible'; fixture = 'alpha-0' }
    )
    $budgetRows = @()
    foreach ($case in $budgetCases) {
        $result = Invoke-Harness $case.name $case.fixture $budget
        $compare = $result.report.compare
        $product = $result.report.product_selection
        $recorded = $product.candidate_report
        $baselineAffine = $product.main_report.affine_transitions
        $ok = $result.exit_code -eq 0 -and $compare.verdict -eq 'EXACT' -and $compare.frames_exact -eq $result.report.expected_frames -and
        $compare.frames_uncompared -eq 0 -and $compare.baseline_repeat_mismatches -eq 0 -and $compare.group_clean -eq $true -and
        $recorded.requested -eq 'auto' -and $product.candidate_request_mode_seen -eq 1 -and
        $product.candidate_controller_supplied_observer -eq $false -and $recorded.preparation_rejected -eq $true -and
        $recorded.preparation_reason -eq 'prepared-raster-budget-exceeded' -and $recorded.evaluated -eq $false -and
        $recorded.decision -eq '' -and $recorded.graph -eq 'conventional' -and $recorded.groups -eq 0 -and $recorded.grouped_clips -eq 0 -and
        $recorded.composed_frames -eq 0 -and $recorded.errors -eq 0 -and $recorded.raster_budget_bytes -eq 100000 -and
        $recorded.raster_bytes -gt 100000 -and $recorded.affine_transitions -eq $baselineAffine -and
        $recorded.live_transitions_after -eq 0 -and $recorded.active_callbacks_after -eq 0 -and
        $compare.candidate_raster_live_bytes_after_export -eq 0 -and "$($product.candidate_output_sha256)".Length -eq 64
        $budgetRows += [pscustomobject]@{ name = $case.name; fixture = $case.fixture; exit_code = $result.exit_code; verdict = $compare.verdict
            frames_exact = $compare.frames_exact; expected_frames = $result.report.expected_frames
            preparation_rejected = $recorded.preparation_rejected; preparation_reason = $recorded.preparation_reason
            decision = $recorded.decision; graph = $recorded.graph; raster_bytes = $recorded.raster_bytes
            raster_budget_bytes = $recorded.raster_budget_bytes; affine_transitions = $recorded.affine_transitions
            baseline_affine_transitions = $baselineAffine; raster_live_bytes_after = $compare.candidate_raster_live_bytes_after_export
            candidate_output_sha256 = $product.candidate_output_sha256; as_expected = [bool]$ok
        }
        if (-not $ok) { ++$failures }
        Write-Host ("{0,-30} exit={1} {2} 一致={3}/{4} 取りやめ={5} ({6}) graph={7} 画像={8}/{9} byte 期待どおり={10}" -f $case.name,
            $result.exit_code, $compare.verdict, $compare.frames_exact, $result.report.expected_frames, $recorded.preparation_rejected,
            $recorded.preparation_reason, $recorded.graph, $recorded.raster_bytes, $recorded.raster_budget_bytes, [bool]$ok)
    }

    # (3) 製品の要求を書き換えない書き出し。
    $pureCases = @(
        @{ name = 'pure-auto-eligible'; fixture = 'cmp-layers-16'; mode = 'on'; decision = 'ELIGIBLE'; reason = 'base-alpha-proven'; graph = 'math-group' },
        @{ name = 'pure-off-eligible'; fixture = 'cmp-layers-16'; mode = 'off'; decision = ''; reason = ''; graph = 'conventional' },
        @{ name = 'pure-auto-unknown'; fixture = 'base-transformed'; mode = 'on'; decision = 'UNKNOWN'; reason = 'base-clip-transformed'; graph = 'conventional' },
        @{ name = 'pure-off-unknown'; fixture = 'base-transformed'; mode = 'off'; decision = ''; reason = ''; graph = 'conventional' },
        @{ name = 'pure-auto-ineligible'; fixture = 'alpha-0'; mode = 'on'; decision = 'INELIGIBLE'; reason = 'base-alpha-zero'; graph = 'conventional' },
        @{ name = 'pure-off-ineligible'; fixture = 'alpha-0'; mode = 'off'; decision = ''; reason = ''; graph = 'conventional' }
    )
    $pureRows = @()
    foreach ($case in $pureCases) {
        $result = Invoke-Harness $case.name $case.fixture @('--product-math-group', $case.mode, '--product-pure', '1') 'product-pure'
        $report = $result.report
        $recorded = $report.report
        $selected = $case.mode -eq 'on'
        $ok = $result.exit_code -eq 0 -and $report.export_success -eq $true -and (Test-PureRequest $report $selected) -and
        $report.final_file_exists -eq $true -and $report.temporary_file_exists -eq $false -and @(Get-MediaLeft $result.out).Count -eq 0 -and
        $report.export_frame_count -eq $report.expected_frames -and $recorded.decision -eq $case.decision -and
        $recorded.reason -eq $case.reason -and $recorded.graph -eq $case.graph -and $recorded.errors -eq 0 -and
        $recorded.preparation_rejected -eq $false -and $report.controller_last_report.graph -eq $case.graph -and
        $report.live_transitions_after -eq 0 -and $report.active_callbacks_after -eq 0 -and $report.raster_live_bytes_after -eq 0 -and
        $(if ($case.graph -eq 'math-group') { $recorded.composed_frames -gt 0 -and $recorded.grouped_clips -gt 0 }
            else { $recorded.composed_frames -eq 0 -and $recorded.grouped_clips -eq 0 -and $recorded.groups -eq 0 })
        $pureRows += [pscustomobject]@{ name = $case.name; fixture = $case.fixture; mode = $case.mode; exit_code = $result.exit_code
            decision = $recorded.decision; reason = $recorded.reason; graph = $recorded.graph; grouped_clips = $recorded.grouped_clips
            affine_transitions = $recorded.affine_transitions; composed_frames = $recorded.composed_frames
            end_to_end_wall_seconds = $report.end_to_end_wall_seconds; output_sha256 = $report.output_sha256
            request = $report.request; as_expected = [bool]$ok
        }
        if (-not $ok) { ++$failures }
        Write-Host ("{0,-22} exit={1} 判定={2} ({3}) graph={4} 合成={5} {6:n1} s 期待どおり={7}" -f $case.name, $result.exit_code,
            $recorded.decision, $recorded.reason, $recorded.graph, $recorded.composed_frames, $report.end_to_end_wall_seconds, [bool]$ok)
    }
    # 同じ fixture で、選択 off と on (および上限の超過で準備を取りやめた on) の MP4 が同じであること。
    $outputs = @()
    foreach ($kind in @('eligible', 'unknown', 'ineligible')) {
        $offRow = @($pureRows | Where-Object { $_.name -eq "pure-off-$kind" })[0]
        $onRow = @($pureRows | Where-Object { $_.name -eq "pure-auto-$kind" })[0]
        $budgetRow = @($budgetRows | Where-Object { $_.name -eq "budget-otherwise-$kind" })[0]
        $same = $offRow.output_sha256 -and $offRow.output_sha256 -eq $onRow.output_sha256
        $budgetSame = $offRow.output_sha256 -and $offRow.output_sha256 -eq $budgetRow.candidate_output_sha256
        $outputs += [pscustomobject]@{ fixture = $offRow.fixture; off_sha256 = $offRow.output_sha256; auto_sha256 = $onRow.output_sha256
            auto_budget_rejected_sha256 = $budgetRow.candidate_output_sha256; auto_same = [bool]$same; auto_budget_rejected_same = [bool]$budgetSame
        }
        Write-Host ("MP4 {0}: off 対 auto 同じ={1}、off 対 auto (準備を取りやめ) 同じ={2}" -f $offRow.fixture, [bool]$same, [bool]$budgetSame)
        if (-not $same -or -not $budgetSame) { ++$failures }
    }
    $after = Test-Frozen '後'
    if (-not $after.match) { ++$failures }
    [ordered]@{ stage = 'focused'; p36a_cases_skipped = [bool]$SkipP36aCases; identity_before = $before; identity_after = $after; p36a_cases = $p36aCounts; p36a_cases_ok = [bool]$p36aOk
        budget = $budgetRows; pure = $pureRows; outputs = $outputs; failures = $failures
    } | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
    Write-Host "結果: $run"
    Write-Host "不成立: $failures"
    if ($failures -gt 0) { exit 1 }
    exit 0
}

# --- full: 製品の入口から、選択 on で Project 全体を 1 回書き出す ---
$historical = @()
foreach ($relative in $HistoricalOutputs) {
    $path = Join-Path $repo $relative
    if (-not (Test-Path -LiteralPath $path)) { throw "過去の出力がありません: $path" }
    $historical += [pscustomobject]@{ file = $relative; sha256 = (Get-Sha $path); bytes = (Get-Item -LiteralPath $path).Length }
}
$idleBefore = @(Get-CimInstance Win32_Process -Filter "Name='ctest.exe' OR Name='mvm_math_score_export_parity.exe'").Count
if ($idleBefore -gt 0) { throw '全 CTest または別の検証 harness が動いています (同時に実行しない)' }
$started = Get-Date
$result = Invoke-Harness 'product' 'v4-full' @('--product-math-group', 'on', '--product-pure', '1') 'product-pure'
$report = $result.report
if ($null -eq $report) {
    [ordered]@{ stage = 'full'; started = $started.ToString('o'); exit_code = $result.exit_code; watchdog_killed = $result.watchdog_killed
        process_wall_seconds = $result.process_wall_seconds; error = '記録がありません'
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
    Write-Host "記録がありません (exit=$($result.exit_code)、watchdog=$($result.watchdog_killed))"
    exit 1
}
$recorded = $report.report
$mp4 = Join-Path $result.out 'v4-full.mp4'
$verdicts = [ordered]@{}
$verdicts.request_untouched = [bool](Test-PureRequest $report $true)
$verdicts.completed = $result.exit_code -eq 0 -and -not $result.watchdog_killed -and $report.export_success -eq $true -and
$report.export_cancelled -eq $false -and $report.watchdog_fired -eq $false -and $report.export_frame_count -eq $TotalFrames
$verdicts.selected_math_group = $recorded.requested -eq 'auto' -and $recorded.evaluated -eq $true -and $recorded.decision -eq 'ELIGIBLE' -and
$recorded.graph -eq 'math-group' -and $recorded.groups -gt 0 -and $recorded.grouped_clips -gt 0 -and $recorded.composed_frames -gt 0 -and
$recorded.preparation_rejected -eq $false -and $report.controller_last_report.graph -eq 'math-group'
$verdicts.no_runtime_errors = $recorded.errors -eq 0
$verdicts.within_budget = $report.end_to_end_wall_seconds -lt $BudgetSeconds
$verdicts.published = $report.final_file_exists -eq $true -and $report.temporary_file_exists -eq $false -and
(Test-Path -LiteralPath $mp4) -and @(Get-MediaLeft $result.out).Count -eq 0
$verdicts.teardown = $report.live_transitions_after -eq 0 -and $report.active_callbacks_after -eq 0 -and $report.raster_live_bytes_after -eq 0 -and
$recorded.live_transitions_after -eq 0 -and $recorded.active_callbacks_after -eq 0

$output = [ordered]@{ file = $mp4 }
if ($verdicts.published) {
    # --- 出力の MP4 の独立な検査 (harness の集計を使わない) ---
    $stream = (& $ffprobe -v error -select_streams v:0 -count_frames -show_entries `
            'stream=codec_name,profile,level,pix_fmt,width,height,r_frame_rate,avg_frame_rate,time_base,nb_frames,nb_read_frames,duration,start_time,has_b_frames' `
            -of json $mp4 | Out-String | ConvertFrom-Json).streams[0]
    $container = (& $ffprobe -v error -show_entries 'stream=index,codec_type,codec_name,nb_frames,duration:format=format_name,duration,size' -of json $mp4 |
        Out-String | ConvertFrom-Json)
    $packets = @(& $ffprobe -v error -select_streams v:0 -show_entries packet=pts,dts -of csv=p=0 $mp4 | Where-Object { $_ } | ForEach-Object {
            $columns = $_ -split ','; [pscustomobject]@{ pts = [long]$columns[0]; dts = [long]$columns[1] } })
    $framePts = @(& $ffprobe -v error -select_streams v:0 -show_entries frame=pts -of csv=p=0 $mp4 | Where-Object { $_ } | ForEach-Object { [long]($_ -replace ',', '') })
    $base = $stream.time_base -split '/'
    $rate = $stream.r_frame_rate -split '/'
    $step = ([long]$base[1] * [long]$rate[1]) / ([long]$base[0] * [long]$rate[0])
    $framesOrdered = $framePts.Count -eq $TotalFrames
    for ($index = 0; $index -lt $framePts.Count; ++$index) { if ($framePts[$index] -ne $index * $step) { $framesOrdered = $false; break } }
    $packetSet = @($packets | ForEach-Object { $_.pts } | Sort-Object -Unique)
    $packetsCover = $packetSet.Count -eq $TotalFrames -and $packets.Count -eq $TotalFrames -and $packetSet[0] -eq 0 -and
    $packetSet[-1] -eq ($TotalFrames - 1) * $step
    $dtsMonotonic = $true
    for ($index = 1; $index -lt $packets.Count; ++$index) { if ($packets[$index].dts -le $packets[$index - 1].dts) { $dtsMonotonic = $false; break } }
    $dtsNotAfterPts = @($packets | Where-Object { $_.dts -gt $_.pts }).Count -eq 0
    # 全 frame を復号する (error があれば失敗)。
    $decodeLog = Join-Path $run 'ffmpeg-decode.log'
    & $ffmpeg -hide_banner -nostdin -v error -xerror -stats -i $mp4 -map 0:v:0 -f null - 2> $decodeLog
    $decodeExit = $LASTEXITCODE
    $decodeText = Get-Content -Raw $decodeLog
    $decodedFrames = if ($decodeText -match '(?s).*frame=\s*(\d+)') { [int]$Matches[1] } else { -1 }
    $decodeErrors = @((Get-Content $decodeLog) | Where-Object { $_ -and $_ -notmatch 'frame=\s*\d+' }).Count
    # x264 が stream へ書いた設定 (過去の出力のものと比べる)。
    function Get-X264([string]$Path) {
        $bytes = [IO.File]::ReadAllBytes($Path)
        $text = [Text.Encoding]::ASCII.GetString($bytes, 0, [math]::Min($bytes.Length, 4MB))
        if ($text -match 'x264 - core[^\x00]*') { $Matches[0] } else { $null }
    }
    $x264 = Get-X264 $mp4
    $x264Historical = Get-X264 (Join-Path $repo $HistoricalOutputs[0])
    $sha = Get-Sha $mp4
    $output.sha256 = $sha
    $output.bytes = (Get-Item -LiteralPath $mp4).Length
    $output.harness_sha256 = $report.output_sha256
    $output.stream = $stream
    $output.container = $container
    $output.frame_pts_count = $framePts.Count
    $output.frame_pts_step = $step
    $output.frames_in_presentation_order = $framesOrdered
    $output.packet_count = $packets.Count
    $output.packet_pts_unique = $packetSet.Count
    $output.packets_cover_all_frames = $packetsCover
    $output.dts_strictly_increasing = $dtsMonotonic
    $output.dts_not_after_pts = $dtsNotAfterPts
    $output.decode_exit = $decodeExit
    $output.decoded_frames = $decodedFrames
    $output.decode_error_lines = $decodeErrors
    $output.x264_settings = $x264
    $output.x264_settings_same_as_historical = [bool]($x264 -and $x264 -eq $x264Historical)
    $output.historical = $historical
    $verdicts.output_valid = $stream.codec_name -eq 'h264' -and $stream.profile -eq 'High' -and $stream.pix_fmt -eq 'yuv420p' -and
    $stream.width -eq 1920 -and $stream.height -eq 1080 -and $stream.r_frame_rate -eq '60/1' -and $stream.avg_frame_rate -eq '60/1' -and
    [int]$stream.nb_frames -eq $TotalFrames -and [int]$stream.nb_read_frames -eq $TotalFrames -and $framesOrdered -and $packetsCover -and
    $dtsMonotonic -and $dtsNotAfterPts -and $decodeExit -eq 0 -and $decodedFrames -eq $TotalFrames -and $decodeErrors -eq 0 -and
    [math]::Abs([double]$stream.duration - $TotalFrames / 60.0) -lt 1e-3 -and [double]$stream.start_time -eq 0
    $verdicts.encoder_configuration = $output.x264_settings_same_as_historical -and $x264 -match 'crf=23\.0'
    $verdicts.sha256_matches_historical = @($historical | Where-Object { $_.sha256 -ne $sha }).Count -eq 0 -and $historical.Count -gt 0 -and
    $sha -eq $report.output_sha256
} else {
    $verdicts.output_valid = $false; $verdicts.encoder_configuration = $false; $verdicts.sha256_matches_historical = $false
}
$after = Test-Frozen '後'
$verdicts.identity_frozen = [bool]$after.match
$allOk = @($verdicts.Values | Where-Object { -not $_ }).Count -eq 0
[ordered]@{
    stage = 'full'; started = $started.ToString('o'); total_frames = $TotalFrames; all_ok = $allOk; verdicts = $verdicts
    budget_seconds = $BudgetSeconds; product_timeout_ms = $report.request.timeout_ms
    harness_cap_seconds = $CapSeconds; script_watchdog_seconds = $WatchdogSeconds
    exit_code = $result.exit_code; watchdog_killed = $result.watchdog_killed
    # 製品の書き出しの全体: controller の exportTimeline を呼ぶ直前から、controller が書き出し中でなくなるまで (単調な時計)。
    end_to_end_wall_seconds = $report.end_to_end_wall_seconds
    export_function_wall_seconds = $report.export_function_wall_seconds
    # process の起動から終了まで (Project を開く、数式の成果物の確認、出力の SHA-256 を含む)。
    process_wall_seconds = $result.process_wall_seconds
    process_cpu_seconds = $report.process_cpu_seconds
    working_set_peak_bytes = $report.working_set_peak_bytes
    request = $report.request; report = $recorded; controller_last_report = $report.controller_last_report
    export_frame_count = $report.export_frame_count; export_error = $report.export_error; controller_status = $report.controller_status
    live_transitions_after = $report.live_transitions_after; active_callbacks_after = $report.active_callbacks_after
    raster_live_bytes_after = $report.raster_live_bytes_after; math_artifact_count = @($report.math_artifacts).Count
    output = $output; identity_before = $before; identity_after = $after
} | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
Write-Host ("全長: exit={0} 成功={1} 全体 {2:n1} s (基準 {3} s) 判定={4} graph={5} group={6} まとめた={7} affine={8} 合成={9} callback={10} 失敗={11} 画像={12} byte" -f `
        $result.exit_code, $report.export_success, $report.end_to_end_wall_seconds, $BudgetSeconds, $recorded.decision, $recorded.graph,
    $recorded.groups, $recorded.grouped_clips, $recorded.affine_transitions, $recorded.composed_frames, $recorded.callbacks, $recorded.errors,
    $recorded.raster_bytes)
foreach ($key in $verdicts.Keys) { Write-Host ("  {0,-28} {1}" -f $key, $verdicts[$key]) }
Write-Host "結果: $run"
if (-not $allOk) { exit 1 }
