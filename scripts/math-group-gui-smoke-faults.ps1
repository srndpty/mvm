<#
.SYNOPSIS
    P3-7B.1: 見えない desktop の製品 GUI の smoke (scripts/math-group-gui-smoke.ps1) へ失敗を注入し、
    分類と後始末を確かめる。人の操作は要らない。利用者の画面には何も出ない。

.DESCRIPTION
    場合ごとに、実際の mvm.exe と driver を見えない desktop で動かす (場面 B: 数式だけの 2 秒の Project)。
    確かめること:
      - smoke の分類 (smoke.json の classification、または runner.json) が期待どおり。
      - 終わった後に、その run の process (runner の job の中・runner の子孫) が 1 つも残っていない。
      - Project lock が残っていない (fixture の .mvm.lock を排他で開ける、または無い)。
      - job の外の process (この script が置く見張りの process) は止められていない。
    場合:
      normal                    注入なし。成立する
      driver-exit               driver が記録も後始末もせずに終わる → driver-failure
      product-crash             書き出しの前に mvm.exe が 0xC0000005 で終わる → product-crash
      exit-during-export        書き出しの最中に mvm.exe が終わる → unexpected-exit-during-export
      missing-element           無い部品・閉じた dialog の古い部品 → driver-step-failed
      unobservable-completion   製品の書き出しは終わるが、driver が結果を確かめられない → completion-not-observed
                                (観測の記録に、状態の文と GUI の応答が残ること)
      session-limit             session の上限 (25 秒) → session-timeout
      runner-timeout            runner の上限 (180 秒。mvm.exe が動いている最中) → job ごと止める (終了コード 3)
      runner-killed             runner の process を外から止める → OS が job を閉じ、子孫が止まる
      stale-lock-file           持ち主の無い .mvm.lock の file がある → 影響なく成立する
      held-lock                 job の外の process が Project lock を持っている → project-locked。持ち主は止めない
    出力: build/math-group-gui-smoke-faults/<日時>/faults.json と、各 smoke の出力先。
    終了コード: 0 = 全部が期待どおり、1 = 期待と違うものがある。
#>
param(
    [string]$OutputDirectory = 'build/math-group-gui-smoke-faults',
    [string[]]$Case = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$smoke = Join-Path $PSScriptRoot 'math-group-gui-smoke.ps1'
$root = Join-Path $repo "$OutputDirectory/$(Get-Date -Format 'yyyyMMdd-HHmmss')"
New-Item -ItemType Directory -Force $root | Out-Null
$smokeRelative = "$OutputDirectory/$(Split-Path -Leaf $root)/smoke"
$pwshPath = (Get-Process -Id $PID).Path
$fixtures = Join-Path $repo 'build/math-group-product-p36c2-20261014/fixtures'
$lockFiles = @('short-math-only-eligible.mvm.lock', 'short-fallback-unknown.mvm.lock') | ForEach-Object { Join-Path $fixtures $_ }
$results = [System.Collections.Generic.List[object]]::new()

function Add-Result([string]$Name, [bool]$Ok, [string]$Detail, $Record = $null) {
    $results.Add([pscustomobject]@{ case = $Name; ok = $Ok; detail = $Detail; record = $Record })
    Write-Host ("{0} {1,-24} {2}" -f $(if ($Ok) { 'ok  ' } else { 'FAIL' }), $Name, $Detail)
}

# lock の file が無いか、排他で開ける (誰も持っていない)。
function Test-LocksFree {
    foreach ($file in $lockFiles) {
        if (-not (Test-Path -LiteralPath $file)) { continue }
        try { [IO.File]::Open($file, 'Open', 'ReadWrite', 'None').Dispose() } catch { return $false }
    }
    $true
}

# この PC で、指定の時刻より後に作られた mvm.exe (この試験の run が作ったもの)。
function Get-NewMvm([datetime]$Since) {
    @(Get-CimInstance Win32_Process -Filter "Name='mvm.exe'" | Where-Object { $_.CreationDate -ge $Since })
}

# runner の子孫 (ParentProcessId を辿る)。PID の再利用を避けるため、作成時刻も持つ。
function Get-Descendants([int]$RootId) {
    $all = @(Get-CimInstance Win32_Process)
    $found = [System.Collections.Generic.List[object]]::new()
    $queue = [System.Collections.Generic.Queue[int]]::new(); $queue.Enqueue($RootId)
    while ($queue.Count -gt 0) {
        $parent = $queue.Dequeue()
        foreach ($child in @($all | Where-Object { $_.ParentProcessId -eq $parent })) {
            $found.Add([pscustomobject]@{ id = [int]$child.ProcessId; name = $child.Name; created = $child.CreationDate }); $queue.Enqueue([int]$child.ProcessId)
        }
    }
    @($found)
}

function Test-Gone($Processes) {
    foreach ($entry in @($Processes)) {
        $alive = Get-CimInstance Win32_Process -Filter "ProcessId=$($entry.id)" | Where-Object { $_.CreationDate -eq $entry.created }
        if ($alive) { return $false }
    }
    $true
}

# smoke を 1 回、見えない desktop で動かす。結果: 終了コード・出力先・smoke.json と runner.json。
function Invoke-Smoke([string]$Name, [string[]]$Extra) {
    $before = @(Get-ChildItem (Join-Path $repo $smokeRelative) -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    $since = Get-Date
    & $pwshPath -NoProfile -File $smoke -OutputDirectory $smokeRelative -Scenario B @Extra *> (Join-Path $root "$Name.log")
    $code = $LASTEXITCODE
    $run = @(Get-ChildItem (Join-Path $repo $smokeRelative) -Directory | Where-Object { $before -notcontains $_.Name }) | Select-Object -Last 1
    $smokeJson = if ($run -and (Test-Path (Join-Path $run.FullName 'smoke.json'))) { Get-Content -Raw (Join-Path $run.FullName 'smoke.json') | ConvertFrom-Json } else { $null }
    $runner = if ($run -and (Test-Path (Join-Path $run.FullName 'runner.json'))) { Get-Content -Raw (Join-Path $run.FullName 'runner.json') | ConvertFrom-Json } else { $null }
    $classification = if ($smokeJson -and @($smokeJson.scenarios).Count -gt 0) { @($smokeJson.scenarios)[0].classification } else { $null }
    $leftover = @(Get-NewMvm $since)
    [pscustomobject]@{ exit_code = $code; run = $(if ($run) { $run.FullName } else { $null }); classification = $classification
        runner = $runner; smoke = $smokeJson; mvm_left = $leftover.Count; locks_free = (Test-LocksFree)
    }
}

function Test-Case([string]$Name, [string[]]$Extra, [string]$Expected, [int]$ExpectedExit = 1, [scriptblock]$More = $null) {
    if ($Case.Count -gt 0 -and $Case -notcontains $Name) { return }
    $r = Invoke-Smoke $Name $Extra
    $clean = $r.mvm_left -eq 0 -and $r.locks_free -and $r.runner -and $r.runner.all_ended -and @($r.runner.left_after_exit).Count -eq 0
    $extraOk = $true; $extraDetail = ''
    if ($More) { $extraOk, $extraDetail = & $More $r }
    Add-Result $Name ($r.exit_code -eq $ExpectedExit -and $r.classification -eq $Expected -and $clean -and $extraOk) (
        "exit=$($r.exit_code) 分類=$($r.classification) (期待 $Expected) mvm 残り=$($r.mvm_left) lock 空き=$($r.locks_free) job 空=$(if ($r.runner) { $r.runner.all_ended }) $extraDetail") $r
}

# --- 注入なしと、session の中の失敗 ---
Test-Case 'normal' @() 'passed' 0
Test-Case 'driver-exit' @('-Fault', 'driver-exit') 'driver-failure'
Test-Case 'product-crash' @('-Fault', 'product-crash') 'product-crash'
Test-Case 'exit-during-export' @('-Fault', 'exit-during-export') 'unexpected-exit-during-export'
Test-Case 'missing-element' @('-Fault', 'missing-element') 'driver-step-failed'
Test-Case 'unobservable-completion' @('-Fault', 'unobservable-completion', '-ExportWaitSeconds', '45') 'completion-not-observed' 1 {
    param($r)
    # 製品の書き出しは成功し、driver の観測には、GUI が応答し、結果の文が見えていた記録が残る。
    $sessionDir = Get-ChildItem $r.run -Directory -Filter '*-session-*' | Select-Object -First 1
    $session = Get-Content -Raw (Join-Path $sessionDir.FullName 'session.json') | ConvertFrom-Json
    $observation = @($session.drive.observations | Where-Object { $_.label -eq 'B3' }) | Select-Object -First 1
    $last = if ($observation) { @($observation.samples)[-1] } else { $null }
    $productDone = @($session.exports | Where-Object { $_.record.result -eq '成功' }).Count -eq 1
    # 別の新しい UI Automation の client も、状態の文を読める (この診断の記録が働くことの確認)。
    $fresh = if ($observation -and @($observation.PSObject.Properties.Name) -contains 'fresh_client') { @($observation.fresh_client.status_like_texts) -join ' / ' } else { '' }
    $ok = $productDone -and $observation -and $observation.outcome -eq 'not-observed' -and $last -and $last.gui_responds -and
        $last.window_process_matches -and $last.window_on_driver_desktop -and -not $last.progress_visible -and $last.status -match '書き出しました: ' -and
        $fresh -match '書き出しました: ' -and @($observation.text_inventory).Count -gt 0
    , @($ok, "製品の書き出し=$productDone 観測=$(if ($observation) { "$($observation.outcome) 標本 $(@($observation.samples).Count)" }) 最後の状態='$(if ($last) { $last.status })' 応答=$(if ($last) { $last.gui_responds }) 別の client='$fresh'")
}
# 場面 B の session は約 27 秒 (書き出しは 20〜23 秒)。15 秒の上限は、書き出しの前に来る。
Test-Case 'session-limit' @('-SessionLimitSeconds', '15') 'session-timeout'

# --- runner の時間切れ: job ごと止める ---
if ($Case.Count -eq 0 -or $Case -contains 'runner-timeout') {
    $sentinel = Start-Process -FilePath $pwshPath -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep -Seconds 600') -PassThru -WindowStyle Hidden
    try {
        # 準備 (freeze・reference) に約 100 秒かかる。driver が結果を待ち続ける (300 秒) 間に、runner の上限
        # (180 秒) が来るようにして、mvm.exe が動いている最中に job ごと止める。
        $r = Invoke-Smoke 'runner-timeout' @('-RunnerTimeoutSeconds', '180', '-Fault', 'unobservable-completion', '-ExportWaitSeconds', '300')
        $stopped = if ($r.runner) { @($r.runner.at_timeout) } else { @() }
        $sentinelAlive = -not $sentinel.HasExited
        Add-Result 'runner-timeout' ($r.exit_code -eq 3 -and $r.runner -and $r.runner.timed_out -and $r.runner.all_ended -and $r.mvm_left -eq 0 -and
            $r.locks_free -and $sentinelAlive -and @($stopped | Where-Object { $_ -match ' mvm$' }).Count -eq 1) (
            "exit=$($r.exit_code) 止めた=[$($stopped -join ', ')] mvm 残り=$($r.mvm_left) lock 空き=$($r.locks_free) job の外の見張り=$(if ($sentinelAlive) { '生きている' } else { '止まった' })") $r
    } finally { if (-not $sentinel.HasExited) { $sentinel.Kill() } }
}

# --- runner の process を外から止める: KILL_ON_JOB_CLOSE で子孫が止まる ---
if ($Case.Count -eq 0 -or $Case -contains 'runner-killed') {
    $since = Get-Date
    $runner = Start-Process -FilePath $pwshPath -ArgumentList @('-NoProfile', '-File', $smoke, '-OutputDirectory', $smokeRelative, '-Scenario', 'B') `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $root 'runner-killed.log') -RedirectStandardError (Join-Path $root 'runner-killed-error.log')
    $deadline = (Get-Date).AddSeconds(180)
    while ((Get-Date) -lt $deadline -and @(Get-NewMvm $since).Count -eq 0) { Start-Sleep -Milliseconds 500 }
    $descendants = @(Get-Descendants $runner.Id)
    $sawMvm = @($descendants | Where-Object { $_.name -eq 'mvm.exe' }).Count -gt 0
    Stop-Process -Id $runner.Id -Force
    $gone = $false
    foreach ($attempt in 1..30) { if (Test-Gone $descendants) { $gone = $true; break }; Start-Sleep -Milliseconds 500 }
    $locksFree = Test-LocksFree
    Add-Result 'runner-killed' ($sawMvm -and $gone -and $locksFree -and @(Get-NewMvm $since).Count -eq 0) (
        "止める前の子孫 $($descendants.Count) (mvm を含む=$sawMvm)、止めた後に全部終わった=$gone、lock 空き=$locksFree") ([pscustomobject]@{ descendants = $descendants })
}

# --- Project lock ---
$lock = Join-Path $fixtures 'short-math-only-eligible.mvm.lock'
if ($Case.Count -eq 0 -or $Case -contains 'stale-lock-file') {
    # 持ち主の無い lock の file (異常終了した別の mvm の跡を模す)。mvm は OPEN_ALWAYS で開くので影響しない。
    if (-not (Test-Path -LiteralPath $lock)) { Set-Content -LiteralPath $lock -Value 'stale' }
    Test-Case 'stale-lock-file' @() 'passed' 0
    if (Test-Path -LiteralPath $lock) { Remove-Item -LiteralPath $lock }
}
if ($Case.Count -eq 0 -or $Case -contains 'held-lock') {
    # job の外のこの process が lock を持つ (利用者の mvm.exe が同じ Project を開いている状態を模す)。参照の
    # 書き出し (reference) も fixture を開くので、lock は reference.json ができた後 (session の前) に取る。
    $before = @(Get-ChildItem (Join-Path $repo $smokeRelative) -Directory -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    $since = Get-Date
    $smokeProcess = Start-Process -FilePath $pwshPath -ArgumentList @('-NoProfile', '-File', $smoke, '-OutputDirectory', $smokeRelative, '-Scenario', 'B') `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $root 'held-lock.log') -RedirectStandardError (Join-Path $root 'held-lock-error.log')
    $handle = $null; $run = $null
    try {
        $deadline = (Get-Date).AddSeconds(300)
        while ((Get-Date) -lt $deadline -and -not $smokeProcess.HasExited -and -not $handle) {
            $run = @(Get-ChildItem (Join-Path $repo $smokeRelative) -Directory -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Name }) | Select-Object -Last 1
            if ($run -and (Test-Path -LiteralPath (Join-Path $run.FullName 'reference.json'))) { $handle = [IO.File]::Open($lock, 'OpenOrCreate', 'ReadWrite', 'None') }
            else { Start-Sleep -Milliseconds 100 }
        }
        $smokeProcess.WaitForExit()
        $usable = $false
        if ($handle) { try { [void]$handle.Seek(0, 'Begin'); $usable = $true } catch { $usable = $false } }
        $smokeJson = if ($run -and (Test-Path (Join-Path $run.FullName 'smoke.json'))) { Get-Content -Raw (Join-Path $run.FullName 'smoke.json') | ConvertFrom-Json } else { $null }
        $runnerJson = if ($run -and (Test-Path (Join-Path $run.FullName 'runner.json'))) { Get-Content -Raw (Join-Path $run.FullName 'runner.json') | ConvertFrom-Json } else { $null }
        $classification = if ($smokeJson -and @($smokeJson.scenarios).Count -gt 0) { @($smokeJson.scenarios)[0].classification } else { $null }
        $mvmLeft = @(Get-NewMvm $since).Count
        Add-Result 'held-lock' ($smokeProcess.ExitCode -eq 1 -and $classification -eq 'project-locked' -and $usable -and $mvmLeft -eq 0 -and
            $runnerJson -and $runnerJson.all_ended -and @($runnerJson.left_after_exit).Count -eq 0) (
            "exit=$($smokeProcess.ExitCode) 分類=$classification (期待 project-locked) 外の持ち主の handle は有効=$usable mvm 残り=$mvmLeft") (
            [pscustomobject]@{ run = $(if ($run) { $run.FullName }); smoke = $smokeJson; runner = $runnerJson })
    } finally {
        if ($handle) { $handle.Dispose() }
        if (-not $smokeProcess.HasExited) { $smokeProcess.Kill($true) }
        if (Test-Path -LiteralPath $lock) { Remove-Item -LiteralPath $lock }
    }
    Add-Result 'held-lock-released' (Test-LocksFree) '持ち主が手放した後、lock は空いている'
}

$failures = @($results | Where-Object { -not $_.ok }).Count
[ordered]@{ created = (Get-Date -Format 'o'); failures = $failures; results = $results
    note = 'P3-7B.1 の失敗の注入。人の操作なし。全部を見えない desktop で行った (runner-killed も含む)。'
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $root 'faults.json') -Encoding utf8
Write-Host "結果: $root ($($results.Count) 件中 不成立 $failures)"
if ($failures -gt 0) { exit 1 }
exit 0
