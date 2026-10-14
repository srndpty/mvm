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
      B  数式だけの Project (short-math-only-eligible): 起動し直した後の既定 off、「入力映像: 仕様を取得
         できません」が出ていても書き出せる → on にして書き出し → 「数式の高速化を使いました（数式 clip 16 本）」
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
    [int]$ExportTimeoutSeconds = 1500
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

function Write-Result([string]$Status) {
    [ordered]@{ scenario = $Scenario; status = $Status; process_id = $ProcessId; seconds = $clock.Elapsed.TotalSeconds
        all_ok = (-not $failed -and $Status -eq 'completed'); steps = $steps
        note = 'mouse と keyboard は使っていない。QML の部品は UI Automation、保存 dialog は Windows の message で操作した。'
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ResultPath -Encoding utf8
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
    try { $tops = @($A::RootElement.FindAll($Scope::Children, $condition)) } catch { return $null }
    @($tops | Where-Object { try { $_.Current.ClassName -like 'Main_QMLTYPE*' } catch { $false } }) | Select-Object -First 1
}

# 見えている (画面の外でない) 部品を名前で探す。prefix なら前方一致。
function Find-Element([string]$Name, [System.Windows.Automation.ControlType]$Type = $null, [switch]$Prefix) {
    $window = Get-MainWindow
    if (-not $window) { return $null }
    $condition = if ($Type) { [System.Windows.Automation.PropertyCondition]::new($A::ControlTypeProperty, $Type) } else { [System.Windows.Automation.Condition]::TrueCondition }
    # tree が変わっている最中 (dialog が開く・閉じる) は、走査が error を返すことがある。見つからなかった
    # ものとして扱い、呼び出し側の次の poll で探し直す。
    try { $elements = $window.FindAll($Scope::Descendants, $condition) } catch { return $null }
    foreach ($element in $elements) {
        try {
            $current = $element.Current
            if ($current.IsOffscreen) { continue }
            if (($Prefix -and $current.Name.StartsWith($Name)) -or (-not $Prefix -and $current.Name -eq $Name)) { return $element }
        } catch { Write-Verbose "$_" }
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

function Wait-ExportEnd([double]$Seconds) {
    # 進行の dialog が消え、状態の表示が結果になるまで待つ。
    $started = Wait-Until { Find-Element '動画を書き出しています' } 30
    $null = $started
    Wait-Until {
        $failure = Find-Element '書き出しに失敗しました: ' ([System.Windows.Automation.ControlType]::Text) -Prefix
        if ($failure) { return [pscustomobject]@{ kind = 'failure'; text = $failure.Current.Name } }
        if (-not (Find-Element '動画を書き出しています')) {
            $status = Get-Status
            if ($status -match '書き出しました: |書き出しをキャンセルしました') { return [pscustomobject]@{ kind = 'status'; text = $status } }
        }
        $null
    } $Seconds 500
}

# 書き出しの後に、出力の file を Explorer で表示する処理が失敗していないこと (P3-7A)。
function Test-RevealNotFailed([string]$Name, $End) {
    Add-Step $Name ($End -and $End.text -and -not $End.text.Contains('Explorerで表示できません')) $(if ($End) { $End.text })
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

    if ($Scenario -eq 'A') {
        $expected = '数式の高速化は使いませんでした（一番下の映像 track の clip に拡大・回転などの変形があるため）。従来の方法で書き出しました: '
        # A1〜A5: 既定 off、on にして書き出し、従来の方法の文。
        if (Open-ExportSettings 'a1-fallback.mp4') {
            [void](Test-CheckboxOff 'A2: 書き出し設定を開いた直後の checkbox は off')
            $hint = Find-Element 'この書き出しだけに使います。' ([System.Windows.Automation.ControlType]::Text) -Prefix
            Add-Step 'A2: 説明の文が出ている' ([bool]$hint -and $hint.Current.Name -match '従来の方法で書き出します' -and $hint.Current.Name -match '自動ではやり直しません')
            if (Start-Export $true) {
                $end = Wait-ExportEnd 300
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
                $end = Wait-ExportEnd 120
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
                        $end = Wait-ExportEnd 300
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
            $warning = Find-Element '入力映像: 仕様を取得できません' ([System.Windows.Automation.ControlType]::Text)
            Add-Step 'B1: 「入力映像: 仕様を取得できません」が出ている (書き出しは止めない)' ([bool]$warning)
            if (Start-Export $true) {
                $end = Wait-ExportEnd 300
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
                $end = Wait-ExportEnd $ExportTimeoutSeconds
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
