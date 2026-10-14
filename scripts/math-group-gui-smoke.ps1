<#
.SYNOPSIS
    製品 GUI の smoke: 実際の mvm.exe を、P3-6C.2 の driver で操作し、書き出しの UX を確かめる。
    人の操作は要らない (mouse・keyboard は使わない)。

.DESCRIPTION
    scripts/math-group-product-p36c2-accept.ps1 (freeze・reference・session -Drive・verify) を、新しい出力先で
    順に呼ぶ。既定は短い場面だけ (A: 対象外の Project、B: 数式だけの Project。全部で約 4 分)。
    -Full を付けると、v4 全体 (6840 frame、約 3〜5 分) の場面 C も行う。全長は、最終の binary が変わった
    とき、または全長に関わる動き (書き出しの経路・数式 group) を変えたときにだけ使う。

    見えない desktop (既定。P3-7B.1):
      - scripts/lib/hidden-desktop-job.ps1 で新しい desktop を作り、この script をその上で起動し直す。
        mvm.exe の window・保存の dialog・上書きの確認は利用者の画面に出ず、前面も奪わない。driver も同じ
        desktop で動くので、UI Automation と Windows の message はそのまま届く。
      - process の木は Job Object で所有する。時間切れ (-RunnerTimeoutSeconds) では job ごと止め、根が
        終わった後に残った process は記録してから止める。この script が異常終了しても、OS が job を閉じて
        中の process (mvm.exe・driver) を止める。job の外の process (利用者の mvm.exe など) は止めない。
      - Explorer: 見えない desktop から「Explorer で表示」を呼ぶと、Explorer (利用者の desktop の process) が
        SHOpenFolderAndSelectItems を 20 秒〜5 分以上待たせてから E_ABORT (0x80004004) で断り、その間 mvm の
        GUI thread が止まる (2026-10-15 に実測)。そこで mvm.exe を --test-reveal-without-explorer 付きで起動
        する (見えない desktop でだけ受け付ける)。path の正規化と実際の SHParseDisplayName (P3-7A の原因の
        箇所) は通り、Explorer を開く処理だけを成功として扱う。**この smoke は、Explorer が実際に開くことを
        確かめない。** それは -Visible の実行と、P3-7A の試験の範囲である。
      - 画面の保存 (PrintWindow) は見えない desktop では取れない (判定には使わない)。
    -Visible を付けると、以前と同じく利用者の画面で実行する (mvm.exe が前面に出て、書き出しのたびに
    Explorer の window が実際に開く。Explorer で表示できることまで確かめたいときに、利用者が明示して使う)。

    場面ごとの分類 (classification): passed、session-record-missing、session-timeout、driver-failure、
    project-locked、unexpected-exit-during-export、product-crash、reveal-mode-mismatch、
    completion-not-observed (製品の書き出しは終わったが、driver が GUI で結果を確かめられない)、
    driver-no-result、driver-step-failed、verify-missing、verify-failed。
    -Fault は、失敗の注入の試験 (scripts/math-group-gui-smoke-faults.ps1) だけが使う。注入した run は
    分類にかかわらず成立にしない。

    出力先: build/math-group-gui-smoke/<日時>/ (前の結果は上書きしない)。経過は smoke.log、runner の結果は
    runner.json、場面の結果は smoke.json。
    終了コード: 0 = 全部の場面が成立、1 = 不成立がある、2 = 準備 (freeze・reference) の失敗、
    3 = runner の時間切れ、4 = 根の終了の後に job に process が残っていた。
    P3-6C.2 の検収の記録と判定 (build/math-group-product-p36c2-20261014/) には触れない。
#>
param(
    [switch]$Full,
    [string]$OutputDirectory = 'build/math-group-gui-smoke',
    # 利用者の画面で実行する (以前の動き。実際の Explorer が開く)。
    [switch]$Visible,
    # 行う場面 (既定は A と B。-Full で C を足す)。
    [ValidateSet('A', 'B', 'C')][string[]]$Scenario = @(),
    # driver が 1 回の書き出しの終わりを待つ秒数。
    [int]$ExportWaitSeconds = 300,
    # session (1 場面) の上限。超えたら mvm.exe と driver を止めて session-timeout にする。0 なら既定。
    [int]$SessionLimitSeconds = 0,
    # 見えない desktop の runner 全体の上限。0 なら既定 (短い場面 1200 秒、-Full 3600 秒)。
    [int]$RunnerTimeoutSeconds = 0,
    # 失敗の注入 (試験だけ)。
    [ValidateSet('none', 'driver-exit', 'product-crash', 'exit-during-export', 'missing-element', 'unobservable-completion')]
    [string]$Fault = 'none',
    # 内部用: 見えない desktop の上で起動し直したときに、親が渡す出力先 (repo からの相対 path)。
    [string]$RunDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$accept = Join-Path $PSScriptRoot 'math-group-product-p36c2-accept.ps1'
$relative = if ($RunDirectory) { $RunDirectory } else { "$OutputDirectory/$(Get-Date -Format 'yyyyMMdd-HHmmss')" }
$root = Join-Path $repo $relative
New-Item -ItemType Directory -Force $root | Out-Null
$pwshPath = (Get-Process -Id $PID).Path
$letters = if ($Scenario.Count -gt 0) { $Scenario } elseif ($Full) { @('A', 'B', 'C') } else { @('A', 'B') }
$hidden = [bool]$RunDirectory

if (-not $Visible -and -not $RunDirectory) {
    . (Join-Path $PSScriptRoot 'lib\hidden-desktop-job.ps1')
    $arguments = @('-NoProfile', '-File', $PSCommandPath, '-RunDirectory', $relative, '-OutputDirectory', $OutputDirectory,
        '-ExportWaitSeconds', "$ExportWaitSeconds", '-SessionLimitSeconds', "$SessionLimitSeconds", '-Fault', $Fault)
    if ($Full) { $arguments += '-Full' }
    if ($Scenario.Count -gt 0) { $arguments += @('-Scenario') + $Scenario }
    $limitSeconds = if ($RunnerTimeoutSeconds -gt 0) { $RunnerTimeoutSeconds } elseif ($letters -contains 'C') { 3600 } else { 1200 }
    Write-Host "見えない desktop で実行します (利用者の画面には出ません。最大 $limitSeconds 秒): $root"
    $result = Invoke-HiddenDesktopJob -File $pwshPath -Arguments $arguments -WorkingDirectory $repo -TimeoutSeconds $limitSeconds `
        -DesktopName "mvm-gui-smoke-$PID"
    $code = if ($result.timed_out) { 3 } elseif ($result.left_after_exit.Count -gt 0) { 4 } else { $result.exit_code }
    [ordered]@{ created = (Get-Date -Format 'o'); limit_seconds = $limitSeconds; inner_exit_code = $result.exit_code
        timed_out = $result.timed_out; at_timeout = $result.at_timeout; left_after_exit = $result.left_after_exit
        all_ended = $result.all_ended; desktop = $result.desktop; seconds = $result.seconds; exit_code = $code; fault = $Fault
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $root 'runner.json') -Encoding utf8
    # 経過 (transcript の見出しと git の改行の警告を除く) を、この console にも出す。
    $log = Join-Path $root 'smoke.log'
    if (Test-Path -LiteralPath $log) {
        Get-Content -LiteralPath $log | Where-Object { $_ -match '^(freeze|reference|場面 |結果|準備)' }
    }
    if ($result.timed_out) { Write-Host "時間切れ ($limitSeconds 秒) で job ごと止めました。止めた process: $($result.at_timeout -join ', ')" }
    if ($result.left_after_exit.Count -gt 0) { Write-Host "終了の後に残っていた process を止めました: $($result.left_after_exit -join ', ')" }
    if (-not $result.all_ended) { Write-Host 'job の process が全部は終わりませんでした (runner.json を参照)' }
    exit $code
}
if ($RunDirectory) {
    # 見えない desktop の上の console は誰にも見えないので、経過を file に残す。
    Start-Transcript -LiteralPath (Join-Path $root 'smoke.log') | Out-Null
}

function Invoke-Accept([string[]]$Arguments, [string]$Log) {
    & $pwshPath -NoProfile -File $accept @Arguments -OutputDirectory $relative *> (Join-Path $root $Log)
    $LASTEXITCODE
}

# 1 つの場面の判定と、失敗の分類。成立の条件: session と verify の両方が 0 で終わり、verify の開始より
# 後に書かれた verify.json があり、その全項目が成立し、driver の段が 1 つ以上あって全部成功し、失敗を
# 注入していない。古い verify.json (前の run・前の verify) は認めない。分類は session の記録から先に
# 決める (製品・driver・時間の問題を、verify の不成立に混ぜない)。
# 自己試験 (math-group-product-p36c2-selftest.ps1) がこの関数を直接呼ぶ。
function Get-ScenarioVerdict([int]$SessionExit, [int]$VerifyExit, [string]$VerifyPath, [datetime]$VerifyStarted,
    [string]$SessionPath = '', [string]$Fault = 'none', [bool]$HiddenDesktop = $false) {
    $fresh = [bool]$VerifyPath -and (Test-Path -LiteralPath $VerifyPath) -and
        (Get-Item -LiteralPath $VerifyPath).LastWriteTime -ge $VerifyStarted
    $verify = if ($fresh) { Get-Content -Raw -LiteralPath $VerifyPath | ConvertFrom-Json } else { $null }
    $failed = @(if ($verify) { $verify.verdicts.PSObject.Properties | Where-Object { -not $_.Value } | ForEach-Object { $_.Name } }
        else { 'この run の verify.json がありません' })
    $steps = @(if ($verify -and $verify.PSObject.Properties['drive_steps']) { $verify.drive_steps })
    $stepsOk = @($steps | Where-Object { $_.ok }).Count
    $session = if ($SessionPath -and (Test-Path -LiteralPath $SessionPath)) { Get-Content -Raw -LiteralPath $SessionPath | ConvertFrom-Json } else { $null }
    $has = { param($object, $name) $object -and @($object.PSObject.Properties.Name) -contains $name }
    $driveSteps = @(if ((& $has $session 'drive') -and $session.drive -and (& $has $session.drive 'steps')) { $session.drive.steps })
    $classification =
    if ($SessionPath -and -not $session) { 'session-record-missing' }
    elseif ((& $has $session 'ended_by') -and $session.ended_by -eq 'session-limit') { 'session-timeout' }
    elseif ((& $has $session 'ended_by') -and $session.ended_by -eq 'driver-exited') { 'driver-failure' }
    elseif ((& $has $session 'project_locked') -and $session.project_locked) { 'project-locked' }
    elseif ((& $has $session 'unfinished_export') -and $session.unfinished_export) { 'unexpected-exit-during-export' }
    elseif ((& $has $session 'exit_code') -and $session.exit_code -ne 0) { 'product-crash' }
    elseif ($HiddenDesktop -and $session -and -not ((& $has $session 'reveal_simulated') -and $session.reveal_simulated)) { 'reveal-mode-mismatch' }
    elseif (@($driveSteps | Where-Object { -not $_.ok -and $_.step -like '書き出しの終わりを確かめられない*' }).Count -gt 0 -and
        @($session.exports | Where-Object { $_.record.kind -eq 'result' }).Count -gt 0) { 'completion-not-observed' }
    elseif ($session -and (& $has $session 'driven') -and $session.driven -and
        (-not (& $has $session 'drive') -or -not $session.drive -or ((& $has $session.drive 'status') -and $session.drive.status -eq 'no-result'))) { 'driver-no-result' }
    elseif (@($driveSteps | Where-Object { -not $_.ok }).Count -gt 0 -or ($steps.Count -gt 0 -and $stepsOk -ne $steps.Count)) { 'driver-step-failed' }
    elseif (-not $fresh) { 'verify-missing' }
    elseif ($SessionExit -ne 0 -or $VerifyExit -ne 0 -or $failed.Count -gt 0 -or $steps.Count -eq 0) { 'verify-failed' }
    else { 'passed' }
    [pscustomobject]@{
        passed = $classification -eq 'passed' -and $Fault -eq 'none' -and $SessionExit -eq 0 -and $VerifyExit -eq 0 -and $fresh -and
            $failed.Count -eq 0 -and $steps.Count -gt 0 -and $stepsOk -eq $steps.Count
        classification = $classification; fresh = $fresh; failed = $failed; steps = $steps.Count; steps_ok = $stepsOk
    }
}

$rows = @()
foreach ($stage in @('freeze', 'reference')) {
    $code = Invoke-Accept @('-Stage', $stage) "$stage.log"
    Write-Host "$stage exit=$code"
    if ($code -ne 0) { Write-Host "準備に失敗しました: $(Join-Path $root "$stage.log")"; exit 2 }
}
$catalog = @{ A = @('short-fallback-unknown', 'smoke-a', 900); B = @('short-math-only-eligible', 'smoke-b', 600); C = @('v4', 'smoke-c', 2400) }
foreach ($letter in $letters) {
    $project, $label, $defaultLimit = $catalog[$letter]
    $limit = if ($SessionLimitSeconds -gt 0) { $SessionLimitSeconds } else { $defaultLimit }
    # この run のこの場面で作られた session と verify の記録だけを認める (古い記録で通さない)。
    $sessionStarted = Get-Date
    $sessionArguments = @('-Stage', 'session', '-Project', $project, '-Label', $label, '-Drive', '-SessionLimitSeconds', "$limit",
        '-DriveExportWaitSeconds', "$ExportWaitSeconds", '-DriveFault', $Fault)
    if ($hidden) { $sessionArguments += '-SimulateExplorerReveal' }
    $code = Invoke-Accept $sessionArguments "session-$label.log"
    $session = @(Get-ChildItem $root -Directory -Filter "*-session-$project-$label" |
            Where-Object { $_.CreationTime -ge $sessionStarted }) | Select-Object -Last 1
    $verifyStarted = Get-Date
    $verifyCode = if ($session) { Invoke-Accept @('-Stage', 'verify', '-Session', $session.Name) "verify-$label.log" } else { -1 }
    $verifyPath = if ($session) { Join-Path $session.FullName 'verify.json' } else { $null }
    $sessionPath = if ($session) { Join-Path $session.FullName 'session.json' } else { Join-Path $root "missing-session-$label.json" }
    $verdict = Get-ScenarioVerdict $code $verifyCode $verifyPath $verifyStarted $sessionPath $Fault $hidden
    $rows += [pscustomobject]@{ scenario = $letter; project = $project; session = $(if ($session) { $session.Name } else { $null })
        session_exit = $code; verify_exit = $verifyCode; verify_fresh = $verdict.fresh; failed_verdicts = $verdict.failed
        drive_steps_ok = $verdict.steps_ok; drive_steps = $verdict.steps; classification = $verdict.classification; passed = $verdict.passed
    }
    Write-Host ("場面 {0} {1,-26} session={2} verify={3} driver {4}/{5} 分類={6} 判定={7}" -f $letter, $project, $code, $verifyCode,
        $verdict.steps_ok, $verdict.steps, $verdict.classification, $(if ($verdict.passed) { '成立' } else { '不成立' }))
}
$failures = @($rows | Where-Object { -not $_.passed }).Count
[ordered]@{ created = (Get-Date -Format 'o'); full = [bool]$Full; failures = $failures; fault = $Fault
    desktop = $(if ($hidden) { '見えない desktop' } else { '利用者の画面 (-Visible)' })
    explorer_reveal = $(if ($hidden) { '模擬 (--test-reveal-without-explorer。path の解釈までは実際の Shell。Explorer は開かない)' } else { '実際の Explorer' })
    scenarios = $rows
    note = '製品 GUI の smoke。人の操作なし。P3-6C.2 の検収の判定とは別の記録である。'
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $root 'smoke.json') -Encoding utf8
Write-Host "結果: $root"
if ($failures -gt 0) { exit 1 }
exit 0
