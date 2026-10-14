<#
.SYNOPSIS
    P3-6C.2: 実際の mvm.exe を、人の代わりに操作する (UI Automation と Windows の message)。
    scripts/math-group-product-p36c2-accept.ps1 の session (-Drive) が起動する。単独では使わない。

.DESCRIPTION
    mouse と keyboard は使わない (SendInput を使わない)。QML の部品は UI Automation の Invoke・Toggle で、
    Windows 標準の保存 dialog は WM_SETTEXT と BM_CLICK で操作する。利用者が同時に PC を使っていても、
    操作が横取りされることは無い (書き出しの時間には影響しうる)。

    場面 (-Scenario):
      A  対象外の Project (short-fallback-unknown): 既定 off → on にして書き出し → 「従来の方法」の文 →
         開き直すと off → on にして書き出し、進行の dialog の「キャンセル」→ 書き出し先を掴んだまま、off で
         同じ名前へ書き出し → 失敗の dialog → 元の file が残り、一時 file が無い
      B  数式だけの Project (short-math-only-eligible): 起動し直した後の既定 off、入力映像が「なし（数式 …件を
         出力の仕様で描きます）」で、仕様を確認できない入力の警告が無い (P3-7B) → on にして書き出し →
         「数式の高速化を使いました（数式 clip 16 本）」
      C  v4 全体: 既定 off、出力 1920×1080 / 60 fps、品質「標準」→ on にして書き出し (6840 frame) →
         「数式の高速化を使いました（数式 clip 97 本）… (6840 frame / 114.00 秒)」→ 開き直すと off → 閉じる

    各段の結果 (確かめたことと、そのときの部品の値) を -ResultPath の JSON に書く。最後に mvm の window を
    閉じる (WindowPattern.Close)。
#>
param(
    [Parameter(Mandatory)][int]$ProcessId,
    [Parameter(Mandatory)][ValidateSet('A', 'B', 'C')][string]$Scenario,
    [Parameter(Mandatory)][string]$ManualOut,
    [Parameter(Mandatory)][string]$ResultPath,
    [int]$ExportTimeoutSeconds = 1500,
    # 短い場面 (A・B) で、1 回の書き出しの終わりを待つ秒数。
    [int]$ExportWaitSeconds = 300,
    # 失敗の注入 (scripts/math-group-gui-smoke-faults.ps1 だけが使う。場面 B で行う):
    #   driver-exit              window を見つけた後、記録も後始末もせずに driver が終わる
    #   product-crash            書き出しの前に、mvm.exe を 0xC0000005 で止める
    #   exit-during-export       書き出しの最中に、mvm.exe を終了コード 1 で止める
    #   missing-element          無い部品と、閉じた dialog の古い部品を操作しようとする
    #   unobservable-completion  書き出しの結果の文を、決して一致しない形で探す (製品の書き出しは終わる)
    [ValidateSet('none', 'driver-exit', 'product-crash', 'exit-during-export', 'missing-element', 'unobservable-completion')]
    [string]$InjectFault = 'none'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($ProcessId -le 0) { throw "process の指定が不正です: $ProcessId" }
if (-not (Test-Path -LiteralPath (Split-Path -Parent $ResultPath))) { throw "結果の置き場がありません: $ResultPath" }
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
if (-not ('Mvm.Drive.Win32' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace Mvm.Drive {
public static class Win32 {
    private delegate bool EnumProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc proc, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder name, int size);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder text, int size);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll")] private static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetDlgItem(IntPtr dialog, int id);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, string lParam);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")] private static extern IntPtr GetThreadDesktop(uint threadId);
    [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool GetUserObjectInformation(IntPtr handle, int index, StringBuilder information, int length, out int needed);

    // この thread の desktop の名前 (利用者の画面は "Default")。UOI_NAME = 2。
    public static string DesktopName() {
        StringBuilder b = new StringBuilder(256); int needed;
        return GetUserObjectInformation(GetThreadDesktop(GetCurrentThreadId()), 2, b, b.Capacity * 2, out needed) ? b.ToString() : "";
    }

    [DllImport("user32.dll")] private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] private static extern bool IsHungAppWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern bool EnumDesktopWindows(IntPtr desktop, EnumProc proc, IntPtr parameter);
    [DllImport("kernel32.dll")] private static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
    [DllImport("kernel32.dll")] private static extern bool TerminateProcess(IntPtr process, uint code);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);

    // window の thread が message に応えるか (WM_NULL を timeoutMs まで待つ。SMTO_ABORTIFHUNG)。
    public static bool Responds(IntPtr window, uint timeoutMs) {
        IntPtr result;
        return window != IntPtr.Zero && SendMessageTimeout(window, 0, IntPtr.Zero, IntPtr.Zero, 0x0002, timeoutMs, out result) != IntPtr.Zero;
    }
    public static bool Hung(IntPtr window) { return window != IntPtr.Zero && IsHungAppWindow(window); }
    // window が、この thread (driver) と同じ desktop にあるか。
    public static bool OnThisDesktop(IntPtr window) {
        bool found = false;
        EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()), delegate (IntPtr candidate, IntPtr parameter) {
            if (candidate == window) { found = true; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static uint OwnerProcess(IntPtr window) { uint processId; GetWindowThreadProcessId(window, out processId); return processId; }
    // 失敗の注入 (試験だけ): process を指定の終了コードで止める (PROCESS_TERMINATE)。
    public static bool Kill(uint processId, uint code) {
        IntPtr handle = OpenProcess(0x0001, false, processId);
        if (handle == IntPtr.Zero) return false;
        try { return TerminateProcess(handle, code); } finally { CloseHandle(handle); }
    }

    private static string ClassOf(IntPtr window) { StringBuilder b = new StringBuilder(256); GetClassName(window, b, b.Capacity); return b.ToString(); }
    public static string TextOf(IntPtr window) { StringBuilder b = new StringBuilder(512); GetWindowText(window, b, b.Capacity); return b.ToString(); }

    // 指定の process の、見えている dialog (#32770) の一覧。
    public static IntPtr[] Dialogs(uint processId) {
        List<IntPtr> found = new List<IntPtr>();
        EnumWindows(delegate (IntPtr window, IntPtr parameter) {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == processId && IsWindowVisible(window) && ClassOf(window) == "#32770") found.Add(window);
            return true;
        }, IntPtr.Zero);
        return found.ToArray();
    }
    // 保存 dialog の「ファイル名」の欄 (ComboBox の中の、id 1001 の Edit)。
    public static IntPtr FileNameEdit(IntPtr dialog) {
        IntPtr result = IntPtr.Zero;
        EnumChildWindows(dialog, delegate (IntPtr window, IntPtr parameter) {
            IntPtr parent = GetParent(window);
            if (ClassOf(window) == "Edit" && GetDlgCtrlID(window) == 1001 && ClassOf(parent) == "ComboBox") { result = window; return false; }
            return true;
        }, IntPtr.Zero);
        return result;
    }
    public static bool SetText(IntPtr window, string text) { return SendMessage(window, 0x000C, IntPtr.Zero, text) != IntPtr.Zero; }
    // dialog の button を押す (BM_CLICK)。dialog が次の dialog を開いても待たないように post する。
    public static bool Click(IntPtr dialog, int id) {
        IntPtr button = GetDlgItem(dialog, id);
        return button != IntPtr.Zero && PostMessage(button, 0x00F5, IntPtr.Zero, IntPtr.Zero);
    }
    // task dialog (上書きの確認など) の button を押す (TDM_CLICK_BUTTON)。
    public static bool ClickTaskDialogButton(IntPtr dialog, int id) {
        return PostMessage(dialog, 0x0466, new IntPtr(id), IntPtr.Zero);
    }
}
}
'@
}

$A = [System.Windows.Automation.AutomationElement]
$Scope = [System.Windows.Automation.TreeScope]
$steps = [System.Collections.Generic.List[object]]::new()
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$failed = $false
# 書き出しの終わりを待つ間の観測 (Wait-ExportEnd)。完了を確かめられなかったときの調べに使う。
$observations = [System.Collections.Generic.List[object]]::new()
$lastUiaError = $null
$driverStartedWall = (Get-Date).AddSeconds(-$clock.Elapsed.TotalSeconds)

function Write-Result([string]$Status) {
    [ordered]@{ scenario = $Scenario; status = $Status; process_id = $ProcessId; seconds = $clock.Elapsed.TotalSeconds
        started_wall = $driverStartedWall.ToString('o'); desktop = [Mvm.Drive.Win32]::DesktopName(); injected_fault = $InjectFault
        all_ok = (-not $failed -and $Status -eq 'completed'); steps = $steps; observations = $observations
        note = 'mouse と keyboard は使っていない。QML の部品は UI Automation、保存 dialog は Windows の message で操作した。'
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ResultPath -Encoding utf8
}

function Add-Step([string]$Name, [bool]$Ok, $Observed = $null) {
    $steps.Add([pscustomobject]@{ step = $Name; ok = $Ok; seconds = [math]::Round($clock.Elapsed.TotalSeconds, 1); observed = $Observed })
    if (-not $Ok) { $script:failed = $true }
    Write-Host ("[drive {0,6:n1} s] {1} {2} {3}" -f $clock.Elapsed.TotalSeconds, $(if ($Ok) { 'ok  ' } else { 'FAIL' }), $Name, $(if ($null -ne $Observed) { "($Observed)" } else { '' }))
}

function Wait-Until([scriptblock]$Condition, [double]$Seconds, [int]$PollMilliseconds = 200) {
    $deadline = $clock.Elapsed.TotalSeconds + $Seconds
    while ($clock.Elapsed.TotalSeconds -lt $deadline) {
        $value = & $Condition
        if ($value) { return $value }
        Start-Sleep -Milliseconds $PollMilliseconds
    }
    & $Condition
}

function Get-MainWindow {
    $condition = [System.Windows.Automation.PropertyCondition]::new($A::ProcessIdProperty, $ProcessId)
    try { $tops = @($A::RootElement.FindAll($Scope::Children, $condition)) } catch { $script:lastUiaError = "$($_.Exception.GetType().Name): $($_.Exception.Message)"; return $null }
    @($tops | Where-Object { try { $_.Current.ClassName -like 'Main_QMLTYPE*' } catch { $false } }) | Select-Object -First 1
}

# 見えている (画面の外でない) 部品を名前で探す。prefix なら前方一致、contains なら部分一致。
function Find-Element([string]$Name, [System.Windows.Automation.ControlType]$Type = $null, [switch]$Prefix, [switch]$Contains) {
    $window = Get-MainWindow
    if (-not $window) { return $null }
    $condition = if ($Type) { [System.Windows.Automation.PropertyCondition]::new($A::ControlTypeProperty, $Type) } else { [System.Windows.Automation.Condition]::TrueCondition }
    # tree が変わっている最中 (dialog が開く・閉じる) は、走査が error を返すことがある。見つからなかった
    # ものとして扱い、呼び出し側の次の poll で探し直す。
    try { $elements = $window.FindAll($Scope::Descendants, $condition) } catch { $script:lastUiaError = "$($_.Exception.GetType().Name): $($_.Exception.Message)"; return $null }
    foreach ($element in $elements) {
        try {
            $current = $element.Current
            if ($current.IsOffscreen) { continue }
            $match = if ($Contains) { $current.Name.Contains($Name) } elseif ($Prefix) { $current.Name.StartsWith($Name) } else { $current.Name -eq $Name }
            if ($match) { return $element }
        } catch { $script:lastUiaError = "$($_.Exception.GetType().Name): $($_.Exception.Message)" }
    }
    $null
}

function Invoke-Element($Element) {
    $pattern = $null
    if ($Element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) { $pattern.Invoke(); return $true }
    $false
}

function Get-Toggle($Element) {
    $pattern = $null
    if ($Element -and $Element.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pattern)) { return "$($pattern.Current.ToggleState)" }
    'unknown'
}

function Set-Toggle($Element) {
    $pattern = $null
    if ($Element.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pattern)) { $pattern.Toggle(); return $true }
    Invoke-Element $Element
}

function Get-Status {
    $text = Find-Element '書き出し' ([System.Windows.Automation.ControlType]::Text) -Prefix
    $math = Find-Element '数式の高速化' ([System.Windows.Automation.ControlType]::Text) -Prefix
    foreach ($candidate in @($math, $text)) { if ($candidate) { return $candidate.Current.Name } }
    ''
}

$checkboxName = '数式の書き出しを高速化（試験的）'

# 「ファイル」→「メディアを書き出し」→ 保存 dialog に file 名 → 「保存」(→ 上書きの確認) → 書き出し設定。
function Open-ExportSettings([string]$FileName, [switch]$ExpectOverwrite) {
    $target = Join-Path $ManualOut $FileName
    # menu が開かないこと (開く前の Invoke が無視される・開いた menu が閉じる) があるので、開き直す。
    $item = $null
    for ($attempt = 0; $attempt -lt 4 -and -not $item; ++$attempt) {
        $menu = Wait-Until { Find-Element 'ファイル(F)' } 30
        if (-not $menu -or -not (Invoke-Element $menu)) { Add-Step "「ファイル」の menu を開く ($FileName)" $false; return $false }
        $item = Wait-Until { Find-Element 'メディアを書き出し' } 3
    }
    if (-not $item -or -not (Invoke-Element $item)) { Add-Step "「メディアを書き出し」を選ぶ ($FileName)" $false; return $false }
    $dialog = Wait-Until { @([Mvm.Drive.Win32]::Dialogs([uint32]$ProcessId) | Where-Object { [Mvm.Drive.Win32]::FileNameEdit($_) -ne [IntPtr]::Zero }) | Select-Object -First 1 } 15
    if (-not $dialog) { Add-Step "保存 dialog が開く ($FileName)" $false; return $false }
    $edit = [Mvm.Drive.Win32]::FileNameEdit($dialog)
    [void][Mvm.Drive.Win32]::SetText($edit, $target)
    $clicked = [Mvm.Drive.Win32]::Click($dialog, 1)
    Add-Step "保存 dialog で書き出し先を指定する" $clicked $target
    if ($ExpectOverwrite) {
        # 上書きの確認 (task dialog。保存 dialog とは別の #32770)。「はい」(IDYES = 6) を押す。
        $confirm = Wait-Until { @([Mvm.Drive.Win32]::Dialogs([uint32]$ProcessId) | Where-Object { [Mvm.Drive.Win32]::FileNameEdit($_) -eq [IntPtr]::Zero }) | Select-Object -First 1 } 10
        $confirmText = if ($confirm) { [Mvm.Drive.Win32]::TextOf($confirm) } else { '' }
        Add-Step '上書きの確認で「はい」を押す' ([bool]$confirm -and [Mvm.Drive.Win32]::ClickTaskDialogButton($confirm, 6)) $confirmText
    }
    $accept = Wait-Until { Find-Element '書き出す' ([System.Windows.Automation.ControlType]::Button) } 15
    Add-Step "書き出し設定が開く ($FileName)" ([bool]$accept)
    [bool]$accept
}

function Test-CheckboxOff([string]$Name) {
    $box = Wait-Until { Find-Element $checkboxName ([System.Windows.Automation.ControlType]::CheckBox) } 5
    $state = Get-Toggle $box
    Add-Step $Name ([bool]$box -and $state -eq 'Off') "checkbox=$state"
    $box
}

function Start-Export([bool]$Accelerate) {
    if ($Accelerate) {
        $box = Find-Element $checkboxName ([System.Windows.Automation.ControlType]::CheckBox)
        [void](Set-Toggle $box)
        $state = Wait-Until { $s = Get-Toggle (Find-Element $checkboxName ([System.Windows.Automation.ControlType]::CheckBox)); if ($s -eq 'On') { $s } } 3
        Add-Step 'checkbox を on にすると、書き出す前に on と表示される' ($state -eq 'On') "checkbox=$state"
    }
    $accept = Find-Element '書き出す' ([System.Windows.Automation.ControlType]::Button)
    $ok = [bool]$accept -and (Invoke-Element $accept)
    Add-Step "「書き出す」を押す (高速化 $(if ($Accelerate) { 'on' } else { 'off' }))" $ok
    $ok
}

# 書き出しの結果の文。失敗の注入 unobservable-completion では、決して一致しない形にする (製品の書き出しは
# 終わるが、driver は GUI で結果を確かめられない、という状態を作る)。
$statusPattern = if ($InjectFault -eq 'unobservable-completion') { '(?!)' } else { '書き出しました: |書き出しをキャンセルしました' }

# 観測の 1 回分: mvm の window (UI Automation の要素・その process・desktop)、GUI thread の応答、進行の
# dialog、状態の文、失敗の文、UI Automation の error。どれも見るだけで、操作はしない。
function Get-ObservationSample {
    $script:lastUiaError = $null
    $window = Get-MainWindow
    $handle = [IntPtr]::Zero
    $runtimeId = $null
    if ($window) {
        try { $handle = [IntPtr]$window.Current.NativeWindowHandle; $runtimeId = ($window.GetRuntimeId() -join '.') } catch { $script:lastUiaError = "$($_.Exception.GetType().Name): $($_.Exception.Message)" }
    }
    $owner = if ($handle -ne [IntPtr]::Zero) { [Mvm.Drive.Win32]::OwnerProcess($handle) } else { 0 }
    $progress = [bool](Find-Element '動画を書き出しています')
    $failure = Find-Element '書き出しに失敗しました: ' ([System.Windows.Automation.ControlType]::Text) -Prefix
    $status = Get-Status
    [pscustomobject][ordered]@{
        t = [math]::Round($clock.Elapsed.TotalSeconds, 1); wall = (Get-Date).ToString('o')
        window_found = [bool]$window; window_runtime_id = $runtimeId; window_handle = ('0x{0:X}' -f $handle.ToInt64())
        window_process = $owner; window_process_matches = ($owner -eq $ProcessId)
        window_on_driver_desktop = ($handle -ne [IntPtr]::Zero -and [Mvm.Drive.Win32]::OnThisDesktop($handle))
        gui_responds = [Mvm.Drive.Win32]::Responds($handle, 2000); gui_hung = [Mvm.Drive.Win32]::Hung($handle)
        progress_visible = $progress; status = $status; failure = $(if ($failure) { $failure.Current.Name } else { $null })
        uia_error = $script:lastUiaError
    }
}

function Wait-ExportEnd([double]$Seconds, [string]$Label = '') {
    # 進行の dialog が消え、状態の表示が結果になるまで待つ。待つ間の観測を記録する (状態が変わったときと、
    # 10 秒ごと)。時間は延ばさない。
    $record = [ordered]@{ label = $Label; started = [math]::Round($clock.Elapsed.TotalSeconds, 1); wait_seconds = $Seconds
        status_pattern = $statusPattern; progress_seen = $false; polls = 0; outcome = $null; samples = [System.Collections.Generic.List[object]]::new()
        last_visible_texts = @()
    }
    $record.progress_seen = [bool](Wait-Until { Find-Element '動画を書き出しています' } 30)
    $deadline = $clock.Elapsed.TotalSeconds + $Seconds
    $lastKey = $null; $lastSampleAt = -100.0; $end = $null
    while ($true) {
        $sample = Get-ObservationSample
        ++$record.polls
        $key = "$($sample.window_found)|$($sample.gui_responds)|$($sample.progress_visible)|$($sample.status)|$($sample.failure)|$($sample.uia_error)"
        if ($key -ne $lastKey -or $sample.t - $lastSampleAt -ge 10) { $record.samples.Add($sample); $lastKey = $key; $lastSampleAt = $sample.t }
        if ($sample.failure) { $end = [pscustomobject]@{ kind = 'failure'; text = $sample.failure }; break }
        if (-not $sample.progress_visible -and $sample.status -match $statusPattern) { $end = [pscustomobject]@{ kind = 'status'; text = $sample.status }; break }
        if ($clock.Elapsed.TotalSeconds -ge $deadline) { break }
        Start-Sleep -Milliseconds 500
    }
    $record.ended = [math]::Round($clock.Elapsed.TotalSeconds, 1)
    $record.outcome = if ($end) { $end.kind } else { 'not-observed' }
    if (-not $end) {
        # 時間切れ: そのとき見えている文を記録する (原因を後から調べられるように)。
        $record.samples.Add($sample)
        $record.last_visible_texts = @(Get-VisibleTexts)
        # 画面の外 (IsOffscreen) のものも含めた、全部の文の部品 (状態の表示の部品が無いのか、見えないのか、
        # 別の名前なのかを後から区別するため)。
        $record.text_inventory = @(Get-TextInventory)
        # 別の新しい UI Automation の client (同じ desktop の別 process) からも、状態らしい文を探す。driver の
        # client の状態の問題か、製品の側の問題かを分けるための記録で、判定には使わない。
        $record.fresh_client = Get-FreshClientStatus
        Add-Step '書き出しの終わりを確かめられない (観測は drive.json の observations)' $false (
            "見えている文 $($record.last_visible_texts.Count)、文の部品 $($record.text_inventory.Count)")
    }
    $observations.Add([pscustomobject]$record)
    $end
}

function Get-FreshClientStatus {
    $probe = @'
param([int]$ProcessId)
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$A = [System.Windows.Automation.AutomationElement]
$Scope = [System.Windows.Automation.TreeScope]
$top = @($A::RootElement.FindAll($Scope::Children, [System.Windows.Automation.PropertyCondition]::new($A::ProcessIdProperty, $ProcessId))) |
    Where-Object { $_.Current.ClassName -like 'Main_QMLTYPE*' } | Select-Object -First 1
if (-not $top) { '(window なし)'; exit 0 }
$texts = $top.FindAll($Scope::Descendants, [System.Windows.Automation.PropertyCondition]::new($A::ControlTypeProperty, [System.Windows.Automation.ControlType]::Text))
foreach ($t in $texts) { $n = "$($t.Current.Name)"; if ($n -match '^(書き出し|数式の高速化)') { "offscreen=$($t.Current.IsOffscreen) $n" } }
'@
    $file = Join-Path ([IO.Path]::GetTempPath()) "mvm-fresh-uia-$PID.ps1"
    try {
        Set-Content -LiteralPath $file -Value $probe -Encoding utf8
        $output = & (Get-Process -Id $PID).Path -NoProfile -File $file -ProcessId $ProcessId 2>&1 | ForEach-Object { "$_" }
        [pscustomobject]@{ exit_code = $LASTEXITCODE; status_like_texts = @($output) }
    } catch {
        [pscustomobject]@{ error = "$($_.Exception.GetType().Name): $($_.Exception.Message)" }
    } finally { Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue }
}

function Get-TextInventory {
    $window = Get-MainWindow
    if (-not $window) { return @() }
    $condition = [System.Windows.Automation.PropertyCondition]::new($A::ControlTypeProperty, [System.Windows.Automation.ControlType]::Text)
    try { $elements = $window.FindAll($Scope::Descendants, $condition) } catch { return @([pscustomobject]@{ error = "$($_.Exception.GetType().Name): $($_.Exception.Message)" }) }
    foreach ($element in $elements) {
        try {
            $current = $element.Current
            $name = "$($current.Name)"
            [pscustomobject]@{ name = $(if ($name.Length -gt 120) { $name.Substring(0, 120) + '…' } else { $name }); offscreen = $current.IsOffscreen
                automation_id = $current.AutomationId; rect = "$($current.BoundingRectangle)"
            }
        } catch { [pscustomobject]@{ error = "$($_.Exception.GetType().Name)" } }
    }
}

function Get-VisibleTexts {
    $window = Get-MainWindow
    if (-not $window) { return @('(mvm の window が見つからない)') }
    $condition = [System.Windows.Automation.PropertyCondition]::new($A::ControlTypeProperty, [System.Windows.Automation.ControlType]::Text)
    try { $elements = $window.FindAll($Scope::Descendants, $condition) } catch { return @("(走査の error: $_)") }
    foreach ($element in $elements) {
        try { if (-not $element.Current.IsOffscreen -and $element.Current.Name) { $element.Current.Name } } catch { Write-Verbose "$_" }
    }
}

# 書き出しの後に、出力の file を Explorer で表示する処理が失敗していないこと (P3-7A)。
# 見えない desktop (math-group-gui-smoke.ps1 の既定) では、mvm.exe は MVM_TEST_REVEAL_WITHOUT_EXPLORER で
# 起動され、path の正規化と実際の SHParseDisplayName (P3-7A の原因の箇所) までを通し、Explorer は開かない。
# どちらの desktop でも、状態の表示に失敗の文が無いことを求める。
$hiddenDesktop = [Mvm.Drive.Win32]::DesktopName() -ne 'Default'
Add-Step '操作する desktop を記録する (利用者の画面は Default)' $true ([Mvm.Drive.Win32]::DesktopName())
function Test-RevealNotFailed([string]$Name, $End) {
    $text = if ($End -and $End.text) { $End.text } else { '' }
    $label = if ($hiddenDesktop) { "$Name [見えない desktop: path の解釈まで。Explorer は開かない]" } else { $Name }
    Add-Step $label ([bool]($text -and -not $text.Contains('Explorerで表示できません'))) $text
}

function Close-Settings {
    $cancel = Find-Element 'キャンセル' ([System.Windows.Automation.ControlType]::Button)
    $ok = [bool]$cancel -and (Invoke-Element $cancel)
    $closed = Wait-Until { -not (Find-Element '書き出す' ([System.Windows.Automation.ControlType]::Button)) } 5
    Add-Step '書き出し設定を「キャンセル」で閉じる' ($ok -and [bool]$closed)
}

try {
    New-Item -ItemType Directory -Force $ManualOut | Out-Null
    $window = Wait-Until { Get-MainWindow } 60
    Add-Step 'mvm の window が開く' ([bool]$window) $(if ($window) { $window.Current.Name })
    if (-not $window) { throw 'window がありません' }
    # Project を開いた直後の処理 (数式の成果物の確認) を待つ。
    Start-Sleep -Seconds 5

    # 失敗の注入 (scripts/math-group-gui-smoke-faults.ps1 だけ)。注入の段は、成立にしないよう必ず失敗にする。
    if ($InjectFault -eq 'driver-exit') {
        # 記録 (drive.json) も、mvm の window を閉じる後始末もせずに終わる (driver の異常終了の代わり)。
        Write-Host '注入: driver が記録も後始末もせずに終わります (終了コード 9)'
        [Environment]::Exit(9)
    }
    if ($InjectFault -eq 'product-crash') {
        Add-Step '注入: 書き出しの前に mvm.exe を 0xC0000005 で止める' $false ([Mvm.Drive.Win32]::Kill([uint32]$ProcessId, [uint32]3221225477))
        throw '注入: mvm.exe が異常終了した'
    }
    if ($InjectFault -eq 'missing-element') {
        $ghost = Find-Element '存在しない部品（注入）' ([System.Windows.Automation.ControlType]::Button)
        Add-Step '注入: 無い部品を探す' $false "found=$([bool]$ghost)"
        # 閉じた dialog の古い部品 (「キャンセル」。操作が通っても害が無いものを選ぶ)。
        if (Open-ExportSettings 'b-stale-element.mp4') {
            $stale = Find-Element 'キャンセル' ([System.Windows.Automation.ControlType]::Button)
            Close-Settings
            $invoked = $null; $staleError = $null
            try { $invoked = Invoke-Element $stale } catch { $staleError = $_.Exception.GetType().Name }
            Add-Step '注入: 閉じた dialog の古い部品を操作する' $false "invoked=$invoked error=$staleError"
        }
        throw '注入: 部品を操作できない'
    }

    if ($Scenario -eq 'A') {
        $expected = '数式の高速化は使いませんでした（一番下の映像 track の clip に拡大・回転などの変形があるため）。従来の方法で書き出しました: '
        # A1〜A5: 既定 off、on にして書き出し、従来の方法の文。
        if (Open-ExportSettings 'a1-fallback.mp4') {
            [void](Test-CheckboxOff 'A2: 書き出し設定を開いた直後の checkbox は off')
            $hint = Find-Element 'この書き出しだけに使います。' ([System.Windows.Automation.ControlType]::Text) -Prefix
            Add-Step 'A2: 説明の文が出ている' ([bool]$hint -and $hint.Current.Name -match '従来の方法で書き出します' -and $hint.Current.Name -match '自動ではやり直しません')
            if (Start-Export $true) {
                $end = Wait-ExportEnd $ExportWaitSeconds 'A5'
                Add-Step 'A5: 状態の表示が「従来の方法」の文で始まる' ($end -and $end.kind -eq 'status' -and $end.text.StartsWith($expected)) $(if ($end) { $end.text })
                Test-RevealNotFailed 'A5: 出力の file を Explorer で表示できた (状態に失敗の文が無い)' $end
            }
        }
        # A6〜A7: 開き直すと off、on にして書き出し、キャンセル。
        if (Open-ExportSettings 'a6-cancel.mp4') {
            [void](Test-CheckboxOff 'A6: 開き直すと checkbox は off に戻っている')
            if (Start-Export $true) {
                $cancel = Wait-Until { Find-Element 'キャンセル' ([System.Windows.Automation.ControlType]::Button) } 10
                $null = Wait-Until { Find-Element '動画を書き出しています' } 10
                Start-Sleep -Seconds 2
                $cancel = Find-Element 'キャンセル' ([System.Windows.Automation.ControlType]::Button)
                Add-Step 'A7: 進行の dialog の「キャンセル」を押す' ([bool]$cancel -and (Invoke-Element $cancel))
                $end = Wait-ExportEnd 120 'A7'
                $left = @(Get-ChildItem $ManualOut -File -Filter 'a6-cancel.mp4*' | ForEach-Object { $_.Name })
                Add-Step 'A7: 取消で終わり、最終 file も一時 file も無い' ($end -and $end.text -eq '書き出しをキャンセルしました' -and $left.Count -eq 0) "status='$(if ($end) { $end.text })' files=$($left -join ',')"
            }
        }
        # A8〜A9: 書き出し先を掴んだまま、off で同じ名前へ書き出す → 失敗。
        $locked = Join-Path $ManualOut 'a1-fallback.mp4'
        if (Test-Path -LiteralPath $locked) {
            $before = (Get-FileHash -Algorithm SHA256 -LiteralPath $locked).Hash
            $handle = [System.IO.File]::Open($locked, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::None)
            try {
                if (Open-ExportSettings 'a1-fallback.mp4' -ExpectOverwrite) {
                    [void](Test-CheckboxOff 'A8: checkbox は off')
                    if (Start-Export $false) {
                        $end = Wait-ExportEnd $ExportWaitSeconds 'A8'
                        Add-Step 'A8: 失敗の dialog が出る (最終の名前へ置けない)' ($end -and $end.kind -eq 'failure' -and $end.text -match 'rename') $(if ($end) { $end.text })
                        $ok = Find-Element 'OK' ([System.Windows.Automation.ControlType]::Button)
                        Add-Step 'A9: 失敗の dialog を「OK」で閉じる' ([bool]$ok -and (Invoke-Element $ok))
                    }
                }
            } finally { $handle.Dispose() }
            $after = (Get-FileHash -Algorithm SHA256 -LiteralPath $locked).Hash
            Add-Step 'A9: 元の file は変わらず、一時 file は残らない' ($before -eq $after -and -not (Test-Path -LiteralPath "$locked.mvmtmp"))
        } else {
            Add-Step 'A8: 掴む file (A1 の出力) がある' $false
        }
    } elseif ($Scenario -eq 'B') {
        if (Open-ExportSettings 'b1-math-only.mp4') {
            [void](Test-CheckboxOff 'B1: 起動し直した後の checkbox は off')
            # P3-7B: 数式の clip は素材の file を持たないので、「仕様を取得できない入力」と数えない。
            $textType = [System.Windows.Automation.ControlType]::Text
            $inputSpec = Find-Element '入力映像: なし（' $textType -Prefix
            $unreadable = Find-Element '入力映像: 仕様を取得できません' $textType
            $failedProbe = Find-Element '件の入力ファイルは仕様を確認できませんでした' $textType -Contains
            Add-Step 'B1: 入力映像は「なし（数式 …件を出力の仕様で描きます）」で、仕様を確認できない入力の警告が無い' (
                [bool]$inputSpec -and $inputSpec.Current.Name.Contains('数式') -and -not $unreadable -and -not $failedProbe) $(if ($inputSpec) { $inputSpec.Current.Name })
            if (Start-Export $true) {
                if ($InjectFault -eq 'exit-during-export') {
                    $progress = Wait-Until { Find-Element '動画を書き出しています' } 30
                    Add-Step '注入: 書き出しの最中に mvm.exe を終了コード 1 で止める' $false "progress=$([bool]$progress) killed=$([Mvm.Drive.Win32]::Kill([uint32]$ProcessId, 1))"
                    throw '注入: 書き出しの最中に mvm.exe が終わった'
                }
                $end = Wait-ExportEnd $ExportWaitSeconds 'B3'
                Add-Step 'B3: 状態の表示が「数式の高速化を使いました（数式 clip 16 本）。書き出しました: 」で始まる' (
                    $end -and $end.kind -eq 'status' -and $end.text.StartsWith('数式の高速化を使いました（数式 clip 16 本）。書き出しました: ')) $(if ($end) { $end.text })
                Test-RevealNotFailed 'B3: 出力の file を Explorer で表示できた (状態に失敗の文が無い)' $end
            }
        }
    } else {
        # 数式の描画が揃うまで待つ (cache から読むので、通常は数秒)。
        Start-Sleep -Seconds 10
        if (Open-ExportSettings 'c-v4-full.mp4') {
            [void](Test-CheckboxOff 'C2: 書き出し設定を開いた直後の checkbox は off')
            $output = Find-Element '出力: ' ([System.Windows.Automation.ControlType]::Text) -Prefix
            Add-Step 'C2: 出力は 1920×1080 / 60 fps' ([bool]$output -and $output.Current.Name -eq '出力: 1920×1080 / 60 fps') $(if ($output) { $output.Current.Name })
            $quality = Find-Element '標準（CRF 23）' -Prefix
            Add-Step 'C2: 品質は「標準（CRF 23）」' ([bool]$quality)
            if (Start-Export $true) {
                $end = Wait-ExportEnd $ExportTimeoutSeconds 'C5'
                $expected = '数式の高速化を使いました（数式 clip 97 本）。書き出しました: '
                Add-Step 'C5: 状態の表示が「数式の高速化を使いました（数式 clip 97 本）」で始まり、6840 frame / 114.00 秒' (
                    $end -and $end.kind -eq 'status' -and $end.text.StartsWith($expected) -and $end.text -match 'c-v4-full\.mp4 \(6840 frame / 114\.00 秒\)') $(if ($end) { $end.text })
                Test-RevealNotFailed 'C5: 出力の file を Explorer で表示できた (状態に失敗の文が無い)' $end
            }
        }
        if (Open-ExportSettings 'c6-reopen-not-exported.mp4') {
            [void](Test-CheckboxOff 'C6: 開き直すと checkbox は off に戻っている')
            Close-Settings
        }
    }
    $status = 'completed'
} catch {
    Add-Step "操作を続けられない: $($_.Exception.Message)" $false
    $status = 'aborted'
} finally {
    # mvm の window を閉じる (保存は聞かれないはず。聞かれたら記録して、保存しない)。
    $window = Get-MainWindow
    $pattern = $null
    if ($window -and $window.TryGetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern, [ref]$pattern)) { $pattern.Close() }
    $exited = Wait-Until { (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) -eq $null } 20
    if (-not $exited) {
        $discard = Find-Element '保存しない' ([System.Windows.Automation.ControlType]::Button)
        Add-Step 'Project の保存を聞かれた (保存しない)' $false
        if ($discard) { [void](Invoke-Element $discard); $exited = Wait-Until { (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) -eq $null } 20 }
    }
    Add-Step 'mvm の window を閉じる (保存を聞かれずに終わる)' ([bool]$exited)
    Write-Result $status
}
