# 終了/New/Openの保存が外部変更で止まったあと、上書きまたはSave Asの成功で
# 元の操作が再開されることを、QMLの制御順と独立した状態遷移の両方で固定する。
$ErrorActionPreference = 'Stop'
$qmlPath = Join-Path $PSScriptRoot '..\..\apps\mvm\Main.qml'
$qml = Get-Content -LiteralPath $qmlPath -Raw

function Get-BracedBlock([string]$text, [int]$openIndex) {
    if ($openIndex -lt 0 -or $openIndex -ge $text.Length -or $text[$openIndex] -ne '{') {
        throw 'block開始が { ではありません'
    }
    $depth = 0
    for ($index = $openIndex; $index -lt $text.Length; $index++) {
        $char = $text[$index]
        if ($char -eq '{') {
            $depth++
        } elseif ($char -eq '}') {
            $depth--
            if ($depth -eq 0) {
                return $text.Substring($openIndex, $index - $openIndex + 1)
            }
        }
    }
    throw 'blockが閉じていません'
}

function Get-FunctionBody([string]$text, [string]$name) {
    $marker = "function $name"
    $start = $text.IndexOf($marker)
    if ($start -lt 0) {
        throw "関数がありません: $name"
    }
    $open = $text.IndexOf('{', $start)
    return Get-BracedBlock $text $open
}

function Get-IdBlock([string]$text, [string]$id) {
    $marker = "id: $id"
    $start = $text.IndexOf($marker)
    if ($start -lt 0) {
        throw "idがありません: $id"
    }
    $open = $text.LastIndexOf('{', $start)
    return Get-BracedBlock $text $open
}

function Assert-Order([string]$body, [string[]]$markers, [string]$label) {
    $previous = -1
    foreach ($marker in $markers) {
        $found = $body.IndexOf($marker, $previous + 1)
        if ($found -lt 0) {
            throw "$label に必要な手順がありません: $marker"
        }
        if ($found -le $previous) {
            throw "$label の手順順が違います: $marker"
        }
        $previous = $found
    }
}

$complete = Get-FunctionBody $qml 'completeExternalSave'
Assert-Order $complete @(
    'if (!saved)',
    'return;',
    'externalSaveDialog.close();',
    'if (!pendingSaveContinuation)',
    'return;',
    'pendingSaveContinuation = false;',
    'unsavedChangesDialog.close();',
    'continuePendingProjectAction();'
) 'completeExternalSave'

$note = Get-FunctionBody $qml 'noteExternalSaveDuringPendingAction'
if ($note -notmatch 'pendingSaveContinuation = pendingProjectAction !== ""') {
    throw '外部変更のcontinuationが、進行中の終了/New/Openに結び付いていません'
}
$signal = Get-FunctionBody $qml 'onExternalCanonicalChangeOnSave'
Assert-Order $signal @(
    'root.noteExternalSaveDuringPendingAction();',
    'externalSaveDialog.open();'
) 'onExternalCanonicalChangeOnSave'

$abandon = Get-FunctionBody $qml 'abandonExternalSaveContinuation'
if ($abandon -notmatch 'pendingSaveContinuation = false;' -or
    $abandon.Contains('continuePendingProjectAction')) {
    throw '外部変更のキャンセルが、元の操作を再開するかcontinuationを残します'
}

$external = Get-IdBlock $qml 'externalSaveDialog'
$overwriteAt = $external.IndexOf('text: "上書きする"')
$saveAsAt = $external.IndexOf('text: "名前を付けて保存"')
$cancelAt = $external.IndexOf('text: "キャンセル"')
if ($overwriteAt -lt 0 -or $saveAsAt -le $overwriteAt -or $cancelAt -le $saveAsAt) {
    throw '外部変更dialogのボタン順を読めません'
}
$overwrite = $external.Substring($overwriteAt, $saveAsAt - $overwriteAt)
$saveAsChoice = $external.Substring($saveAsAt, $cancelAt - $saveAsAt)
$cancel = $external.Substring($cancelAt)
if ($overwrite -notmatch 'root\.completeExternalSave\(\s*root\.mvmController\.saveProjectOverwritingExternalChange\(\)\)') {
    throw '上書き成功がpending actionの再開へ接続されていません'
}
if ($overwrite.Contains('continuePendingProjectAction')) {
    throw '上書きボタンがcontinuation判定を迂回しています'
}
Assert-Order $saveAsChoice @(
    'externalSaveDialog.close();',
    'saveProjectDialog.open();'
) '外部変更からのSave As'
if ($saveAsChoice.Contains('abandonExternalSaveContinuation') -or
    $saveAsChoice.Contains('continuePendingProjectAction')) {
    throw 'Save Asを選んだ時点でcontinuationを消すか、保存前に操作を再開しています'
}
if (-not $cancel.Contains('root.abandonExternalSaveContinuation();') -or
    $cancel.Contains('continuePendingProjectAction')) {
    throw '外部変更のキャンセル後も終了/New/Openが再開されます'
}

$saveDialog = Get-IdBlock $qml 'saveProjectDialog'
Assert-Order $saveDialog @(
    'onAccepted: root.completeExternalSave(root.mvmController.saveProjectAs(selectedFile))',
    'onRejected: root.abandonExternalSaveContinuation()'
) 'saveProjectDialog'

$unsaved = Get-IdBlock $qml 'unsavedChangesDialog'
$unsavedCancel = $unsaved.Substring($unsaved.LastIndexOf('text: "キャンセル"'))
Assert-Order $unsavedCancel @(
    'root.pendingProjectAction = "";',
    'root.pendingSaveContinuation = false;',
    'unsavedChangesDialog.close();'
) '未保存確認のキャンセル'
if ($unsavedCancel.Contains('continuePendingProjectAction')) {
    throw '未保存確認のキャンセルが元の操作を再開します'
}

# QMLとは別の遷移表。成功時だけ元のactionを実行し、失敗とキャンセルはdialogを残す。
function Invoke-ExternalSaveContinuation([string]$pendingAction, [string]$outcome) {
    $state = [ordered]@{
        Pending = $pendingAction
        Continuation = $false
        UnsavedOpen = $pendingAction -ne ''
        ExternalOpen = $false
        Performed = ''
    }
    $state.Continuation = $state.Pending -ne ''
    $state.ExternalOpen = $true
    switch ($outcome) {
        'overwrite-ok' {
            $state.ExternalOpen = $false
            if ($state.Continuation) {
                $state.Continuation = $false
                $state.UnsavedOpen = $false
                $state.Performed = $state.Pending
                $state.Pending = ''
            }
        }
        'overwrite-fail' { }
        'saveas-ok' {
            $state.ExternalOpen = $false
            if ($state.Continuation) {
                $state.Continuation = $false
                $state.UnsavedOpen = $false
                $state.Performed = $state.Pending
                $state.Pending = ''
            }
        }
        'saveas-reject' {
            $state.ExternalOpen = $false
            $state.Continuation = $false
        }
        'cancel' {
            $state.ExternalOpen = $false
            $state.Continuation = $false
        }
        default { throw "未知の操作です: $outcome" }
    }
    return $state
}

$cases = @(
    @{ Name = 'close-overwrite'; Pending = 'close'; Outcome = 'overwrite-ok'; Performed = 'close'; PendingLeft = ''; Unsaved = $false; External = $false; Continuation = $false },
    @{ Name = 'new-saveas'; Pending = 'new'; Outcome = 'saveas-ok'; Performed = 'new'; PendingLeft = ''; Unsaved = $false; External = $false; Continuation = $false },
    @{ Name = 'open-cancel'; Pending = 'open'; Outcome = 'cancel'; Performed = ''; PendingLeft = 'open'; Unsaved = $true; External = $false; Continuation = $false },
    @{ Name = 'close-overwrite-fail'; Pending = 'close'; Outcome = 'overwrite-fail'; Performed = ''; PendingLeft = 'close'; Unsaved = $true; External = $true; Continuation = $true },
    @{ Name = 'close-saveas-reject'; Pending = 'close'; Outcome = 'saveas-reject'; Performed = ''; PendingLeft = 'close'; Unsaved = $true; External = $false; Continuation = $false },
    @{ Name = 'direct-overwrite'; Pending = ''; Outcome = 'overwrite-ok'; Performed = ''; PendingLeft = ''; Unsaved = $false; External = $false; Continuation = $false },
    @{ Name = 'direct-saveas'; Pending = ''; Outcome = 'saveas-ok'; Performed = ''; PendingLeft = ''; Unsaved = $false; External = $false; Continuation = $false }
)

foreach ($case in $cases) {
    $result = Invoke-ExternalSaveContinuation $case.Pending $case.Outcome
    $matches = $result.Performed -eq $case.Performed -and
        $result.Pending -eq $case.PendingLeft -and
        $result.UnsavedOpen -eq $case.Unsaved -and
        $result.ExternalOpen -eq $case.External -and
        $result.Continuation -eq $case.Continuation
    if (-not $matches) {
        throw "外部保存の継続契約が崩れています: $($case.Name)"
    }
}
