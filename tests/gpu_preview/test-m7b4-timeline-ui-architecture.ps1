$ErrorActionPreference = 'Stop'
$qmlPath = Join-Path $PSScriptRoot '..\..\apps\mvm\Main.qml'
$controllerHeaderPath = Join-Path $PSScriptRoot '..\..\apps\mvm\mvm_controller.h'
$mainPath = Join-Path $PSScriptRoot '..\..\apps\mvm\main.cpp'
$previewItemPath = Join-Path $PSScriptRoot '..\..\src\app\preview\preview_engine_rhi_item.cpp'
$compositorPath = Join-Path $PSScriptRoot '..\..\src\media\gpu_preview\gpu_compositor.cpp'
$qml = Get-Content -LiteralPath $qmlPath -Raw
$projectPanel = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\ProjectPanel.qml') -Raw
$compactMenu = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\CompactMenu.qml') -Raw
$compactItem = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\CompactMenuItem.qml') -Raw
$compactSeparator = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\CompactMenuSeparator.qml') -Raw
# 分割後の実装も契約の検査対象にする。欠損したファイルは読み込みで失敗する。
$controller = (@('mvm_controller.cpp', 'mvm_controller_export.cpp',
                  'mvm_controller_effects.cpp', 'mvm_controller_media.cpp',
                  'mvm_controller_timeline_edit.cpp', 'mvm_controller_detail.cpp') | ForEach-Object {
    Get-Content -LiteralPath (Join-Path $PSScriptRoot "../../apps/mvm/$_") -Raw
}) -join "`n"
$controllerHeader = Get-Content -LiteralPath $controllerHeaderPath -Raw
$main = Get-Content -LiteralPath $mainPath -Raw
$previewItem = Get-Content -LiteralPath $previewItemPath -Raw
$compositor = Get-Content -LiteralPath $compositorPath -Raw
$waveformView = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\waveform_view.h') -Raw
$previewSurface = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\preview_surface_qml.h') -Raw

function Test-SpeedHoldUiContract([string]$qmlSource, [string]$controllerSource,
                                  [string]$headerSource) {
    return ($qmlSource -match '(?s)Action\s*\{\s*id:\s*speedDurationAction\b.*?shortcut:\s*"Ctrl\+R".*?onTriggered:\s*root\.openSpeedDurationDialog\(""\)') -and
           $qmlSource.Contains('action: speedDurationAction') -and
           $qmlSource.Contains('root.openSpeedDurationDialog(clipItem.clipId)') -and
           $qmlSource.Contains('enabled: clipMenu.visible') -and
           $qmlSource.Contains('&& root.mvmController.playheadFrame > clipItem.timelineStartFrame') -and
           $qmlSource.Contains('root.mvmController.insertFrameHoldAtPlayhead(clipItem.clipId)') -and
           $qmlSource.Contains('root.mvmController.applyClipSpeedDuration(') -and
           $controllerSource.Contains('MvmController::insertFrameHoldAtPlayhead(') -and
           $controllerSource.Contains('MvmController::applyClipSpeedDuration(') -and
           $headerSource.Contains('insertFrameHoldAtPlayhead(') -and
           $headerSource.Contains('applyClipSpeedDuration(')
}
if (-not (Test-SpeedHoldUiContract $qml $controller $controllerHeader) -or
    (Test-SpeedHoldUiContract $qml ($controller.Replace('MvmController::insertFrameHoldAtPlayhead(', 'removedFrameHold(')) $controllerHeader) -or
    (Test-SpeedHoldUiContract $qml ($controller.Replace('MvmController::applyClipSpeedDuration(', 'removedSpeedDuration(')) $controllerHeader) -or
    (Test-SpeedHoldUiContract ($qml.Replace('shortcut: "Ctrl+R"', 'shortcut: "Ctrl+Alt+R"')) $controller $controllerHeader) -or
    (Test-SpeedHoldUiContract ($qml.Replace('root.mvmController.insertFrameHoldAtPlayhead(clipItem.clipId)', '')) $controller $controllerHeader) -or
    (Test-SpeedHoldUiContract ($qml.Replace('&& root.mvmController.playheadFrame > clipItem.timelineStartFrame', '')) $controller $controllerHeader) -or
    (Test-SpeedHoldUiContract $qml $controller ($controllerHeader.Replace('applyClipSpeedDuration(', 'removedSpeedDuration(')))) {
    throw '速度・デュレーションとフレーム保持の UI 契約が崩れています'
}

function Test-CompactMenuStyle([string]$itemSource) {
    return $itemSource.Contains('implicitHeight: 27') -and
           $itemSource.Contains('item.highlighted && item.enabled') -and
           $itemSource.Contains('shortcutLabel')
}
if (-not (Test-CompactMenuStyle $compactItem) -or
    (Test-CompactMenuStyle $compactItem.Replace('implicitHeight: 27', 'implicitHeight: 44')) -or
    $compactMenu -notmatch 'background:\s*Rectangle' -or
    $compactSeparator -notmatch 'implicitHeight:\s*9') {
    throw 'コンパクトメニューの行高・選択色・区切り線を確認できません'
}
foreach ($source in @($qml, $projectPanel)) {
    if ($source -match '(?<!Compact)MenuItem\s*\{' -or
        $source -match '(?<!Compact)MenuSeparator\s*\{' -or
        $source -match '(?<!Compact)Menu\s*\{') {
        throw '標準スタイルのメニューが残っています'
    }
}

# メニューの表示、ショートカット、実行先を同じ項目内で検査する。
function Test-ProjectMenuContract([string]$source) {
    if ($source -notmatch 'menuBar:\s*MenuBar\s*\{') { return $false }
    foreach ($item in @(
        @{ Id = 'openProjectAction'; Text = 'プロジェクトを開く'; Key = 'Ctrl+O'; Action = 'root.requestProjectAction("open")' },
        @{ Id = 'closeProjectAction'; Text = 'プロジェクトを閉じる'; Key = 'Ctrl+Shift+W'; Action = 'root.requestProjectAction("close")' },
        @{ Id = 'saveProjectAction'; Text = '保存'; Key = 'Ctrl+S'; Action = 'root.mvmController.saveProject()' },
        @{ Id = 'saveProjectAsAction'; Text = '名前を付けて保存'; Key = 'Ctrl+Shift+S'; Action = 'saveProjectDialog.open()' },
        @{ Id = 'exportMediaAction'; Text = 'メディアを書き出し'; Key = 'Ctrl+M'; Action = 'exportDialog.open()' }
    )) {
        $pattern = 'Action\s*\{[^{}]*id:\s*' + [regex]::Escape($item.Id) +
                   '[^{}]*text:\s*"' + [regex]::Escape($item.Text) +
                   '"[^{}]*shortcut:\s*"' + [regex]::Escape($item.Key) +
                   '"[^{}]*onTriggered:\s*' + [regex]::Escape($item.Action)
        if ($source -notmatch $pattern -or
            $source -notmatch ('CompactMenuItem\s*\{\s*action:\s*' + [regex]::Escape($item.Id))) {
            return $false
        }
    }
    return $true
}
if (-not (Test-ProjectMenuContract $qml)) {
    throw 'プロジェクト操作のメニューとショートカットが一致しません'
}
# いずれか一つの指定が壊れた場合、上の検査が必ず落ちることを確かめる。
foreach ($key in @('Ctrl+O', 'Ctrl+Shift+W', 'Ctrl+S', 'Ctrl+Shift+S', 'Ctrl+M')) {
    $broken = $qml.Replace('shortcut: "' + $key + '"', 'shortcut: "Ctrl+Alt+X"')
    if ($broken -eq $qml -or (Test-ProjectMenuContract $broken)) {
        throw "メニュー検査の負例が効いていません: $key"
    }
}
if ($qml -match '// --- ツールバー' -or $qml -match 'Shortcut\s*\{\s*sequence:\s*"Ctrl\+S"') {
    throw '旧ツールバーまたは保存ショートカットの重複が残っています'
}

function Test-NavigationShortcuts([string]$source) {
    foreach ($entry in @(
        @{ Key = 'J'; Action = 'root.mvmController.shuttleLeft()' },
        @{ Key = 'K'; Action = 'root.mvmController.pauseTimeline()' },
        @{ Key = 'L'; Action = 'root.mvmController.shuttleRight()' },
        @{ Key = 'Left'; Action = 'root.mvmController.stepTimelineFrames(-1)' },
        @{ Key = 'Right'; Action = 'root.mvmController.stepTimelineFrames(1)' },
        @{ Key = 'Shift+Left'; Action = 'root.mvmController.stepTimelineFrames(-5)' },
        @{ Key = 'Shift+Right'; Action = 'root.mvmController.stepTimelineFrames(5)' },
        @{ Key = 'Up'; Action = 'root.mvmController.jumpToEditPoint(-1)' },
        @{ Key = 'Down'; Action = 'root.mvmController.jumpToEditPoint(1)' }
    )) {
        $pattern = 'Shortcut\s*\{[^{}]*sequence:\s*"' + [regex]::Escape($entry.Key) +
                   '"[^{}]*onActivated:\s*' + [regex]::Escape($entry.Action)
        if ($source -notmatch $pattern) { return $false }
    }
    return $true
}
if (-not (Test-NavigationShortcuts $qml) -or
    (Test-NavigationShortcuts $qml.Replace('sequence: "Shift+Right"',
                                           'sequence: "Ctrl+Shift+Right"'))) {
    throw 'タイムライン移動ショートカットの契約が崩れています'
}

# window 全体の単一キー / 矢印 shortcut は、文字入力や popup に focus があれば無効にする。
function Test-TextInputGuard([string]$source) {
    if ($source -notmatch 'readonly property bool timelineShortcutsEnabled:[^\n]*(\n\s*&&[^\n]*)*\n\s*&& !root\.keyboardFocusTakesKeys') {
        return $false
    }
    foreach ($key in @('J', 'K', 'L', 'Left', 'Right', 'Shift+Left', 'Shift+Right', 'Up', 'Down',
                       'Space', 'Delete')) {
        $pattern = 'Shortcut\s*\{[^{}]*sequence:\s*"' + [regex]::Escape($key) +
                   '"[^{}]*enabled:[^{}]*?(root\.timelineShortcutsEnabled|!root\.keyboardFocusTakesKeys)'
        if ($source -notmatch $pattern) { return $false }
    }
    return $true
}
if (-not (Test-TextInputGuard $qml) -or
    (Test-TextInputGuard $qml.Replace('enabled: root.mvmController.playing && !root.keyboardFocusTakesKeys',
                                      'enabled: root.mvmController.playing'))) {
    throw 'transport shortcutが文字入力中のfocusを除外していません'
}

# 編集ショートカットは Action として定義してメニューに出し、文字入力中は無効にする。
# Ctrl+A などを text field から奪わないため、enabled に keyboardFocusTakesKeys を必ず含める。
$editActions = @(
    @{ Id = 'selectAllClipsAction'; Key = 'Ctrl+A'; Call = 'root.mvmController.selectAllClips()' },
    @{ Id = 'splitAtPlayheadAction'; Key = 'Ctrl+K'; Call = 'root.mvmController.splitSelectionAtPlayhead()' },
    @{ Id = 'splitAllTracksAction'; Key = 'Ctrl+Shift+K'; Call = 'root.mvmController.splitClipAt("", root.mvmController.playheadFrame, true, true)' },
    @{ Id = 'volumeUpAction'; Key = '['; Call = 'root.mvmController.stepSelectedClipVolume(1)' },
    @{ Id = 'volumeDownAction'; Key = ']'; Call = 'root.mvmController.stepSelectedClipVolume(-1)' },
    @{ Id = 'toggleClipEnabledAction'; Key = 'Shift+E'; Call = 'root.mvmController.toggleSelectedClipsEnabled()' },
    @{ Id = 'defaultTransitionAction'; Key = 'Shift+D'; Call = 'root.mvmController.applyDefaultTransition()' }
)
function Test-EditActionGuard([string]$source) {
    foreach ($entry in $editActions) {
        $pattern = 'Action\s*\{\s*id:\s*' + [regex]::Escape($entry.Id) + '\b[^{}]*shortcut:\s*"' +
                   [regex]::Escape($entry.Key) + '"[^{}]*enabled:[^\n]*!root\.keyboardFocusTakesKeys' +
                   '[^{}]*onTriggered:\s*' + [regex]::Escape($entry.Call)
        if ($source -notmatch $pattern) { return $false }
        # メニューのアクセスキー (mnemonic) は付いていてもよい。
        $menuItem = 'CompactMenuItem \{ action: ' + [regex]::Escape($entry.Id) + '(; mnemonic: "[A-Z]")? \}'
        if ($source -notmatch $menuItem) { return $false }
    }
    return $true
}
if (-not (Test-EditActionGuard $qml) -or
    (Test-EditActionGuard ($qml -replace '(?s)(id: selectAllClipsAction.*?enabled: [^\n]*?) && !root\.keyboardFocusTakesKeys', '$1')) -or
    (Test-EditActionGuard ($qml -replace 'CompactMenuItem \{ action: splitAtPlayheadAction(; mnemonic: "[A-Z]")? \}', ''))) {
    throw '編集ショートカットの Action が文字入力中の focus を除外していないか、メニューに出ていません'
}

# タイムラインツールのキーと有効/無効は TimelineToolPanel.tools だけが決め、
# Main.qml のショートカットはその配列から生成する。キー割り当てを 2 箇所に書かない。
$toolPanel = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\TimelineToolPanel.qml') -Raw
function Test-TimelineToolContract([string]$panelSource, [string]$mainSource) {
    foreach ($entry in @(
        @{ Tool = 'select'; Key = 'V'; Available = 'true' },
        @{ Tool = 'trackForward'; Key = 'A'; Available = 'true' },
        @{ Tool = 'trackBackward'; Key = 'Shift+A'; Available = 'true' },
        @{ Tool = 'razor'; Key = 'C'; Available = 'true' },
        @{ Tool = 'ripple'; Key = 'B'; Available = 'true' },
        @{ Tool = 'rolling'; Key = 'N'; Available = 'true' },
        @{ Tool = 'rate'; Key = 'R'; Available = 'true' },
        @{ Tool = 'slip'; Key = 'Y'; Available = 'true' },
        @{ Tool = 'slide'; Key = 'U'; Available = 'true' },
        @{ Tool = 'pen'; Key = 'P'; Available = 'true' },
        @{ Tool = 'hand'; Key = 'H'; Available = 'true' },
        @{ Tool = 'zoom'; Key = 'Z'; Available = 'true' },
        @{ Tool = 'text'; Key = 'T'; Available = 'true' }
    )) {
        $pattern = '\{\s*tool:\s*"' + [regex]::Escape($entry.Tool) + '"[^{}]*key:\s*"' +
                   [regex]::Escape($entry.Key) + '"[^{}]*available:\s*' + $entry.Available + '\b'
        if ($panelSource -notmatch $pattern) { return $false }
    }
    $shortcut = 'Instantiator\s*\{\s*model:\s*timelineToolPanel\.tools\s*delegate:\s*Shortcut\s*\{' +
                '[^{}]*sequence:\s*modelData\.key' +
                '[^{}]*enabled:\s*modelData\.available && !root\.keyboardFocusTakesKeys' +
                '[^{}]*onActivated:\s*timelineToolPanel\.requestTool\(modelData\.tool\)'
    if ($mainSource -notmatch $shortcut) { return $false }
    foreach ($needle in @('TimelineToolPanel {',
                          'root.mvmController.splitClipAt(',
                          'root.mvmController.rippleTrimClip(',
                          'root.mvmController.rollClipEdge(',
                          'root.mvmController.rateStretchClip(',
                          'root.mvmController.slipClip(',
                          'root.mvmController.slideClip(',
                          'root.mvmController.selectClipsFromFrame(',
                          'id: viewToolArea')) {
        if (-not $mainSource.Contains($needle)) { return $false }
    }
    return $true
}
if (-not (Test-TimelineToolContract $toolPanel $qml)) {
    throw 'タイムラインツールのキー割り当てまたは実行先が崩れています'
}
# レート調整の drag 中の表示は、確定と同じ Project の計算 (previewRateStretch) を
# clip ごとに使う。リンク相手の尺が違うと、共有の端の移動量では相手の表示が確定と食い違う。
#   端ハンドル   -> previewRateStretch(..., "left"/"right", ...) の clips を ratePreviewClips へ
#   clip の表示  -> ratePreview の startDelta / endDelta / speed
#   波形         -> shownSpeed で縮尺を決める
#   終了        -> endLinkedEdit で ratePreviewClips を空にする
function Test-RatePreviewContract([string]$mainSource) {
    foreach ($needle in @(
        'readonly property var ratePreview: timelinePanel.ratePreviewClips[clipId]',
        'ratePreview !== undefined ? ratePreview.startDelta',
        'ratePreview !== undefined ? ratePreview.endDelta',
        'ratePreview !== undefined ? ratePreview.speed : speed',
        'secondsPerPixel: timelineSecondsPerFrame * clipItem.shownSpeed',
        'root.mvmController.rateStretchClip(')) {
        if (-not $mainSource.Contains($needle)) { return $false }
    }
    foreach ($edge in @('left', 'right')) {
        $call = 'root\.mvmController\.previewRateStretch\(\s*clipItem\.clipId,\s*"' + $edge +
                '",\s*requested,\s*clipItem\.editLinked\)'
        if ($mainSource -notmatch $call) { return $false }
    }
    if (([regex]::Matches($mainSource, 'timelinePanel\.ratePreviewClips = preview\.clips')).Count -ne 2) {
        return $false
    }
    if ($mainSource -notmatch 'function endLinkedEdit\(\) \{\s*timelinePanel\.ratePreviewClips = \(\{\}\);') {
        return $false
    }
    return $true
}
if (-not (Test-RatePreviewContract $qml)) {
    throw 'レート調整の drag 中の表示が確定と同じ計算 (previewRateStretch) を使っていません'
}
foreach ($brokenRate in @(
    $qml.Replace('ratePreview !== undefined ? ratePreview.endDelta', 'ratePreview !== undefined ? rightPreviewDelta'),
    $qml.Replace('ratePreview !== undefined ? ratePreview.speed : speed', 'speed'),
    $qml.Replace('secondsPerPixel: timelineSecondsPerFrame * clipItem.shownSpeed', 'secondsPerPixel: timelineSecondsPerFrame * clipItem.speed'),
    $qml.Replace('timelinePanel.ratePreviewClips = ({});', ''),
    ($qml -replace 'timelinePanel\.ratePreviewClips = preview\.clips;', ''),
    ($qml -replace 'previewRateStretch\(\s*clipItem\.clipId,\s*"right"', 'clampEdgeDrag(clipItem.clipId, "right"')
)) {
    if ($brokenRate -eq $qml -or (Test-RatePreviewContract $brokenRate)) {
        throw 'レート調整の表示の検査の負例が効いていません'
    }
}

# どれか 1 つを壊すと上の検査が落ちることを確かめる。
foreach ($broken in @(
    @{ Panel = $toolPanel.Replace('key: "C"', 'key: "X"'); Main = $qml },
    @{ Panel = $toolPanel -replace '(tool:\s*"rate"[^{}]*available:\s*)true', '${1}false'; Main = $qml },
    @{ Panel = $toolPanel; Main = $qml.Replace('root.mvmController.rateStretchClip(', 'root.mvmController.trimClip(') },
    @{ Panel = $toolPanel; Main = $qml.Replace('enabled: modelData.available && !root.keyboardFocusTakesKeys',
                                                'enabled: modelData.available') }
)) {
    if (($broken.Panel -eq $toolPanel -and $broken.Main -eq $qml) -or
        (Test-TimelineToolContract $broken.Panel $broken.Main)) {
        throw 'タイムラインツール検査の負例が効いていません'
    }
}

# timeline UI は clip の配置を track/start から引く。vector 順を authority にしない。
$requiredQml = @(
    'x: timelineStartFrame * timelinePanel.pixelsPerFrame',
    'y: timelinePanel.rowY(trackKind, trackIndex)',
    'function trackAtY(y)',
    'root.mvmController.selectTimelineClip(clipItem.clipId, clipItem.editLinked)',
    'import "TimelineGestures.js" as Gestures',
    'Gestures.bodyPress(tool, mouse.modifiers, pressFrame)',
    'Gestures.bodyRelease(clipItem.gestureState, clipItem.bodyMoved,',
    'Gestures.edgeRelease(timelinePanel.tool, edge, delta,',
    'Gestures.linkedFor(modifiers)',
    'root.mvmController.moveTimelineClip(',
    'releasedClipId, destinationKind, destinationIndex',
    'root.mvmController.selectTimelineClips(selectedIds)',
    'required property bool selected'
)
# track 数は固定しない。model から引き、行位置は rowY() だけが決める。
$forbiddenQml = @(
    'timelineList.indexAt',
    'mvmController.reorderClip',
    'model: ["V2", "V1"]',
    'videoTrack === 1'
)
# premiere 相当の操作。どれか 1 つでも消えたら UI 契約が崩れている。
$requiredInteractions = @(
    'root.mvmController.beginScrub()',
    'root.mvmController.scrubToFrame(',
    'root.mvmController.endScrub()',
    'height: timelineFlick.contentHeight',
    'y: timelineFlick.contentY',
    'id: ruler',
    'z: 200',
    'function handleNativeAltWheel(',
    'function handleNativeCtrlWheel(',
    'function handleNativeShiftWheel(',
    'function handleNativePlainWheel(',
    'setZoom(wheelDelta > 0 ? 1 : -1,',
    'readonly property var zoomLevels:',
    'property int zoomIndex:',
    'readonly property real fitPixelsPerFrame:',
    'Math.min(zoomLevels[0], requestedFitPixelsPerFrame)',
    'readonly property int minimumZoomIndex:',
    'timelineFlick.width * 0.7 / root.mvmController.totalTimelineFrames',
    'zoomIndex === minimumZoomIndex ? fitPixelsPerFrame : zoomLevels[zoomIndex]',
    'onObservedTimelineFramesChanged:',
    'zoomIndex = Math.max(minimumZoomIndex,',
    'Math.max(minimumZoomIndex, Math.min(zoomLevels.length - 1,',
    'const anchorFrame = (timelineFlick.contentX + anchorItemX) / pixelsPerFrame;',
    'const desiredContentX = anchorFrame * pixelsPerFrame - anchorItemX;',
    'Math.min(nextMaxContentX, desiredContentX)',
    'timelinePanel.activeDragOffsetX = clipItem.bodyDragOffsetX',
    # drag 中の 0 frame / track 範囲の丸めは、選択群全体の端で行う (tst_timeline_gestures.qml)。
    'clipItem.bodyDragBounds = root.mvmController.timelineDragBounds(clipItem.clipId);',
    'clipItem.bodyDragOffsetX = Gestures.groupDragOffsetX(',
    'snapped.index = Gestures.groupDragTrackIndex(',
    'clipItem.linkGroupId === timelinePanel.activeDragLinkGroup',
    '(mouse.modifiers & Qt.ShiftModifier) !== 0',
    'root.mvmController.toggleTimelineClipSelection(',
    'timelineFlick.contentY',
    'id: timelineHorizontalScrollBar',
    'policy: ScrollBar.AlwaysOn',
    'ScrollBar.vertical:',
    'id: timelineVerticalScrollBar',
    'active: timelineFlick.contentHeight > timelineFlick.height',
    'interactive: true',
    'const maxY = Math.max(0, timelineFlick.contentHeight - timelineFlick.height);',
    'function trackForDrag(kind, trackAreaY)',
    'preventStealing: true',
    'acceptedButtons: Qt.LeftButton',
    'root.mvmController.hasGapAt(',
    'root.mvmController.hasClipAt(',
    'root.mvmController.rippleDeleteGap(',
    'root.mvmController.deleteTimelineClip(',
    'root.mvmController.unlinkTimelineClip(',
    'root.mvmController.setTrackMuted(',
    # 目玉のドラッグ塗りは離したときにまとめて確定する (tst_track_eye_paint.qml)。
    'import "TrackEyePaint.js" as EyePaint',
    'root.mvmController.setTracksMuted("video", indices, muted)',
    'root.mvmController.setTrackSolo(',
    'root.mvmController.addTrack(',
    'root.mvmController.videoTrackModel',
    'root.mvmController.audioTrackModel'
)

$requiredShortcuts = @(
    'sequence: "Delete"',
    'sequence: "Space"',
    'autoRepeat: false',
    'shortcut: "Ctrl+Z"',
    'enabled: root.mvmController.canUndo',
    'onTriggered: root.mvmController.undoLastEdit()',
    'shortcut: "Ctrl+Shift+Z"',
    'enabled: root.mvmController.canRedo',
    'onTriggered: root.mvmController.redoLastEdit()',
    'sequence: "Ctrl+Y"',
    'onActivated: redoAction.trigger()',
    'action: redoAction'
)

$requiredVideoDrop = @(
    'id: videoDropArea',
    'drag.hasUrls',
    'root.isLocalFileUrl(drag.urls[index])',
    'root.mvmController.addMediaFilesToTimelineAt(urls, target.kind, target.index',
    'projectPanel.importUrls(urls)',
    'drop.accept(Qt.CopyAction)',
    'drag.accept(Qt.CopyAction)',
    '(drag.supportedActions & Qt.CopyAction) !== 0',
    'videoDropArea.acceptingVideoDrag'
)

# drop は参照登録であり移動ではない。source が提示した action (Move を含む) を
# そのまま受理しない。
$forbiddenVideoDrop = @(
    'acceptProposedAction',
    'isSupportedVideoUrl',
    '/\.(mp4|mov|mkv|ts)$/i'
)

$requiredExportProgress = @(
    'visible: root.mvmController.exporting',
    'value: root.mvmController.exportProgress',
    'text: root.mvmController.exportProgressText',
    'root.mvmController.cancelTimelineExport()'
)

$requiredExportFailure = @(
    'id: exportFailureDialog',
    'title: "書き出しに失敗しました"',
    'function onExportFailed(message)',
    'exportFailureDialog.message = message',
    'exportFailureDialog.open()'
)

$requiredExportSettings = @(
    'id: exportSettingsDialog',
    'id: qualityCombo',
    'key: "high"',
    'key: "standard"',
    'key: "compact"',
    'root.mvmController.exportSettingsSummary()',
    'root.mvmController.exportTimelineWithQuality(',
    '品質は圧縮率だけを変更します。出力の解像度とfpsはプロジェクト設定のままです。'
)

$requiredProjectSettings = @(
    'id: projectSettingsDialog',
    'id: matchClipSettingsDialog',
    'プロジェクト設定をこの素材に合わせる',
    'root.mvmController.projectSettingsForClip(clipId)',
    'root.mvmController.setProjectVideoSettings(',
    '既存クリップの開始位置は秒位置を維持して換算します。素材のin/outは変更しません。続行しますか？'
)

foreach ($needle in @('id: rectangleSelectionArea')) {
    if (-not $qml.Contains($needle)) {
        throw "矩形選択の契約がありません: $needle"
    }
}

# audio clip の波形。可視範囲だけに置かないと、長い clip の高倍率表示で巨大な texture を作る。
foreach ($needle in @('WaveformView {', 'cache: root.waveformCache',
                      'mediaPath: clipItem.clipKind === "audio" ? clipItem.mediaPath : ""',
                      'Math.max(0, timelineFlick.contentX - clipContentX)',
                      'x: clipItem.renderOffsetX')) {
    if (-not $qml.Contains($needle)) {
        throw "audio波形の契約がありません: $needle"
    }
}
# 型は QML module へ宣言的に登録し、cache は root の required property として注入する。
if (-not $waveformView.Contains('QML_NAMED_ELEMENT(WaveformView)') -or
    -not $main.Contains('{QStringLiteral("waveformCache"), QVariant::fromValue(&waveformCache)}') -or
    -not $qml.Contains('required property WaveformCache waveformCache')) {
    throw 'audio波形の登録がありません'
}

foreach ($removed in @('spaceMoveToolActive', 'id: moveToolArea', 'sequence: "Ctrl+Space"',
                       'TimelineSpaceMoveState', 'acceptsTextInput(QGuiApplication::focusObject())')) {
    if ($qml.Contains($removed) -or $main.Contains($removed)) {
        throw "削除したSpace move toolの契約が残っています: $removed"
    }
}

foreach ($needle in ($requiredQml + $requiredInteractions + $requiredShortcuts +
                     $requiredVideoDrop + $requiredExportProgress + $requiredExportFailure +
                     $requiredExportSettings + $requiredProjectSettings)) {
    if (-not $qml.Contains($needle)) {
        throw "timeline UI contractがありません: $needle"
    }
}
foreach ($needle in $forbiddenVideoDrop) {
    if ($qml.Contains($needle)) {
        throw "動画dropの旧契約 (拡張子による制限 / proposed action の受理) が残っています: $needle"
    }
}
foreach ($needle in $forbiddenQml) {
    if ($qml.Contains($needle)) {
        throw "track数を固定する旧timeline UIが残っています: $needle"
    }
}

# zoom式はQMLがauthority。ここは同じ式を独立に評価し、短いtimelineと長いtimelineの
# 契約がソース上の式と一致することを確認する。
$zoomMatch = [regex]::Match($qml, 'readonly property var zoomLevels:\s*\[(?<levels>[\s\S]*?)\]')
if (-not $zoomMatch.Success) {
    throw 'zoomLevelsを読み取れません'
}
$zoomLevels = @()
foreach ($token in ($zoomMatch.Groups['levels'].Value -split ',')) {
    $trimmed = $token.Trim()
    if ($trimmed.Length -eq 0) { continue }
    $zoomLevels += [double]::Parse($trimmed, [cultureinfo]::InvariantCulture)
}
if ($zoomLevels.Count -lt 2) {
    throw 'zoom段階が複数ありません'
}
function Get-FitPixelsPerFrame([double]$viewportWidth, [double]$totalFrames) {
    $rawFit = $viewportWidth * 0.7 / $totalFrames
    return [Math]::Min($zoomLevels[0], $rawFit)
}
$shortFit = Get-FitPixelsPerFrame 1000 10
if ([Math]::Abs($shortFit - $zoomLevels[0]) -gt 0.0000001) {
    throw "10 frameの最小zoomが通常presetから外れています: $shortFit"
}
if ($zoomLevels[-1] -le $zoomLevels[0]) {
    throw '10 frameで複数段階のzoomへ進めません'
}
$longFrames = 1000000
$longViewport = 1000
$longFit = Get-FitPixelsPerFrame $longViewport $longFrames
if ($longFit -ge $zoomLevels[0]) {
    throw '非常に長いtimelineでも通常presetより広くzoom outできません'
}
if ($longFit * $longFrames -gt $longViewport * 0.7 + 0.001) {
    throw '最大zoom-outでtimeline全体がviewportの70%に収まりません'
}
if ($qml -notmatch 'const anchorFrame = \(timelineFlick\.contentX \+ anchorItemX\) / pixelsPerFrame;' -or
    $qml -notmatch 'const desiredContentX = anchorFrame \* pixelsPerFrame - anchorItemX;') {
    throw 'zoomのcursor anchorが維持されません'
}

# moveTimelineClipは同期的にmodelを更新してdelegateを破棄し得る。
# 呼び出し後にdelegate contextのtimelinePanelを参照するとReferenceErrorになる。
$bodyAreaIndex = $qml.IndexOf('id: bodyArea')
$bodyReleaseIndex = $qml.IndexOf('onReleased: mouse => {', $bodyAreaIndex)
$bodyMoveIndex = $qml.IndexOf('root.mvmController.moveTimelineClip(', $bodyReleaseIndex)
$bodyDragResetIndex = $qml.IndexOf('timelinePanel.activeDragLinkGroup = "";', $bodyReleaseIndex)
if ($bodyAreaIndex -lt 0 -or $bodyReleaseIndex -lt 0 -or $bodyMoveIndex -lt 0 -or
    $bodyDragResetIndex -lt 0 -or $bodyDragResetIndex -gt $bodyMoveIndex) {
    throw 'delegateを破棄し得るmoveTimelineClipより前にdrag状態をresetしていません'
}

# filter の本体は字幕ダイアログの試験からも使うため timeline_wheel_filter.h へ分けた。
# 設置 (installEventFilter) は main.cpp に残る。
$wheelFilter = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\timeline_wheel_filter.h') -Raw
if (-not $wheelFilter.Contains('class TimelineWheelEventFilter final') -or
    -not $main.Contains('window->installEventFilter(&timelineWheelFilter)') -or
    -not $wheelFilter.Contains('testFlag(Qt::AltModifier)') -or
    -not $wheelFilter.Contains('testFlag(Qt::ControlModifier)') -or
    -not $wheelFilter.Contains('testFlag(Qt::ShiftModifier)') -or
    -not $wheelFilter.Contains('method = "handleNativePlainWheel"') -or
    -not $wheelFilter.Contains('angleDelta.x()') -or
    -not $wheelFilter.Contains('pixelDelta.x()') -or
    -not $wheelFilter.Contains('if (delta == 0)') -or
    -not $qml.Contains('if (wheelDelta === 0)')) {
    throw 'modifier付きwheelがQQuickWindowのevent filterで先取りされていません'
}

# native surfaceはQML scene graphへ宣言し、C++からwindow表示後に動的追加しない。
# 型は QML module の静的登録なので engine.load より必ず前に登録済みになる。
if (-not $qml.Contains('PreviewSurface {') -or
    -not $previewSurface.Contains('QML_NAMED_ELEMENT(PreviewSurface)') -or
    -not $previewSurface.Contains('class PreviewSurfaceQml : public PreviewEngineRhiItem') -or
    $main.Contains('new mvm::app::PreviewEngineRhiItem') -or
    $main.Contains('new mvm::app::PreviewSurfaceQml')) {
    throw 'product GUIのnative preview surfaceがQML scene graphへ事前登録されていません'
}

if ($controller.Contains('recomputeTimelineStarts(candidate)')) {
    throw 'controller編集経路がrecomputeTimelineStartsに依存しています'
}
if (-not $controller.Contains('QString::number(selectedClipIds_.size())')) {
    throw 'linked clip展開後の実選択数をstatusへ表示していません'
}
foreach ($needle in @('setTimelineSelection({clipId.toStdString()});',
                      'UndoEntry undo{project_, selectedClipIds_, currentClipId(), playheadFrame_, currentRevision_};',
                      'std::vector<std::string> deletedIds = selectedClipIds_;',
                      'for (const auto& id : deletedIds)',
                      'project_ = std::move(entry.project);',
                      'scheduleRecoveryAutosave();',
                      'from.pop_back();',
                      'to.push_back(std::move(current));',
                      'return stepEditHistory(undoHistory_, redoHistory_, false);',
                      'return stepEditHistory(redoHistory_, undoHistory_, true);',
                      'redoHistory_.clear();')) {
    if (-not $controller.Contains($needle)) {
        throw "audio/video選択同期またはUndoの契約がありません: $needle"
    }
}
# エフェクト対象は再生位置ではなく選択から決める。描画対象との一致を要求しない。
# 実動作は test_text_ui_input.cpp で文字の区間外へシークして検査する。
function Test-SelectedEffectTarget([string]$source) {
    $seekBody = [regex]::Match($source,
        '(?s)bool MvmController::seekTimelineFrame.*?(?=bool MvmController::prepareTimelineFrameForPlayback)').Value
    return $seekBody.Contains('if (!selectedClipIds_.empty()) {') -and
           $seekBody -match 'std::find\(selectedClipIds_\.begin\(\),\s*selectedClipIds_\.end\(\),\s*current\)'  -and
           $seekBody.Contains('selectedClipIds_.front()') -and
           -not $seekBody.Contains('project::timelineClipIndexAt(project_, current.track, clamped)')
}
if (-not (Test-SelectedEffectTarget $controller) -or
    (Test-SelectedEffectTarget $controller.Replace('if (!selectedClipIds_.empty()) {', 'if (false) {')) -or
    (Test-SelectedEffectTarget $controller.Replace('selectedClipIds_.front()', 'removedSelection')) -or
    (Test-SelectedEffectTarget $controller.Replace('bool MvmController::seekTimelineFrame(qint64 frame) {',
        'bool MvmController::seekTimelineFrame(qint64 frame) { project::timelineClipIndexAt(project_, current.track, clamped);'))) {
    throw '選択中のクリップをエフェクト対象に維持する契約がありません'
}

foreach ($needle in @('FILE_FLAG_DELETE_ON_CLOSE',
                      'ERROR_SHARING_VIOLATION',
                      'RecoveryWriter(project::saveProjectRecovery)',
                      'project::classifyRecovery(',
                      'savedCanonicalSha256_',
                      'canonicalBaseMatchesDisk(')) {
    if (-not $controller.Contains($needle)) {
        throw "Project lockまたはrecovery照合の契約がありません: $needle"
    }
}
foreach ($needle in @('currentRevision_ = nextRevision_++;',
                      'savedRevision_ = currentRevision_;')) {
    if (-not $controller.Contains($needle)) {
        throw "未保存変更のcontroller契約がありません: $needle"
    }
}
foreach ($needle in @('Q_PROPERTY(bool dirty READ dirty NOTIFY stateChanged)',
                      'Q_PROPERTY(bool recoveryAvailable READ recoveryAvailable NOTIFY stateChanged)',
                      'Q_INVOKABLE bool saveProject();',
                      'Q_INVOKABLE bool saveProjectOverwritingExternalChange();',
                      'Q_PROPERTY(bool recoveryForeign READ recoveryForeign NOTIFY stateChanged)',
                      'Q_INVOKABLE bool discardUnsavedChanges();',
                      'Q_INVOKABLE bool restoreRecovery();',
                      'Q_INVOKABLE bool discardRecovery();',
                      'Q_INVOKABLE bool dismissRecovery();',
                      'Q_PROPERTY(bool recoveryCanonicalChanged READ recoveryCanonicalChanged NOTIFY stateChanged)',
                      'Q_PROPERTY(bool recoveryCorrupt READ recoveryCorrupt NOTIFY stateChanged)')) {
    if (-not $controllerHeader.Contains($needle)) {
        throw "未保存変更のcontroller公開契約がありません: $needle"
    }
}
foreach ($needle in @('onClosing: close => {',
                      'if (!closeConfirmed && root.mvmController.dirty)',
                      'root.requestProjectAction("new")',
                      'root.requestProjectAction("open")',
                      'root.mvmController.saveProject()',
                      'root.mvmController.discardUnsavedChanges()',
                      'id: unsavedChangesDialog',
                      'id: recoveryDialog',
                      'id: externalSaveDialog',
                      'root.mvmController.saveProjectOverwritingExternalChange()',
                      'root.mvmController.recoveryForeign',
                      'function onExternalCanonicalChangeOnSave()',
                      'root.mvmController.recoveryCanonicalChanged',
                      'root.mvmController.dismissRecovery()',
                      'root.mvmController.recoveryCorrupt')) {
    if (-not $qml.Contains($needle)) {
        throw "未保存変更の終了確認UIがありません: $needle"
    }
}
foreach ($needle in @('exportThreadFactory_([this,',
                      'exportCancelRequested_.store(true, std::memory_order_release)',
                      'exportCancelling_ = true;',
                      'finishTimelineExport(std::move(exported))',
                      'Qt::QueuedConnection')) {
    if (-not $controller.Contains($needle)) {
        throw "非同期書き出しまたはキャンセル伝播の契約がありません: $needle"
    }
}
foreach ($needle in @('previewStatus.state == preview::PreviewEngineState::Error',
                      'else if (previewEngine_->status().state == preview::PreviewEngineState::Error)',
                      'scrubTimer_.stop();')) {
    if (-not $controller.Contains($needle)) {
        throw "Preview error時のscrub停止契約がありません: $needle"
    }
}
# preview の layer 構成は mapTimelinePreviewFrame に一本化する。描画区間は Project ごとに
# 1 度作った plan (previewPlan) を渡す。
if (-not $controller.Contains('mapTimelinePreviewFrame(project_, previewPlan(), timelineFrame)')) {
    throw 'controllerがpreview layer mappingを経由していません'
}
if (-not $controller.Contains('project::placeLinkedAvPairAt(')) {
    throw 'linked video/audioを単一transactionで配置していません'
}

if (-not $previewItem.Contains('setMirrorVertically(false)') -or
    $previewItem.Contains('setMirrorVertically(true)')) {
    throw '製品previewの上下方向がD3D11出力と一致していません'
}
if (-not $previewItem.Contains('PreviewRenderPort::renderFrameDue(*engine_)')) {
    throw '新しいoutput frameがない周期にもrender targetを黒でclearしています'
}
# seek 中も、decode の完了を回収できるまでは render pass を始めない (黒い frame を提示しない)。
# 完了の回収は一度きりなので、renderFrameDue で見つけた失敗は renderFrame へ引き継ぐ。
$engineSource = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\src\preview_engine\preview_engine.cpp') -Raw
function Test-SeekPresentsOnlyWhenReady([string]$source) {
    return $source -match 'if \(!pending\.decodeReady && !pending\.dueFailure\)\s*pending\.dueFailure = engine\.impl_->advanceSeekLocked\(' -and
           $source.Contains('return pending.decodeReady || pending.dueFailure.has_value();') -and
           $source.Contains('dueFailure ? std::exchange(dueFailure, std::nullopt)')
}
if (-not (Test-SeekPresentsOnlyWhenReady $engineSource) -or
    (Test-SeekPresentsOnlyWhenReady $engineSource.Replace('return pending.decodeReady || pending.dueFailure.has_value();', 'return pending.active;')) -or
    (Test-SeekPresentsOnlyWhenReady $engineSource.Replace('dueFailure ? std::exchange(dueFailure, std::nullopt)', 'false ? std::nullopt'))) {
    throw 'seek中にdecode完了前のrender targetを黒でclearして提示しています'
}
if (-not $qml.Contains('root.mvmController.outputWidth') -or
    -not $qml.Contains('root.mvmController.outputHeight')) {
    throw 'previewがProject output sizeの縦横比を使っていません'
}
if (-not $compositor.Contains('aspectFit(croppedWidth, croppedHeight, destinationBox.width,')) {
    throw '製品compositorが素材の縦横比を保持していません'
}

function Test-RulerMarkMenu([string]$source) {
    return $source.Contains('acceptedButtons: Qt.LeftButton | Qt.RightButton') -and
           $source.Contains('menuMarkerFrame = Gestures.markerNearRulerX(') -and
           $source.Contains('rulerMarkMenu.popup();') -and
           $source.Contains('visible: rulerArea.menuMarkerFrame >= 0') -and
           $source.Contains('height: visible ? implicitHeight : 0') -and
           $source.Contains('text: "インを消去"') -and
           $source.Contains('text: "アウトを消去"') -and
           $source.Contains('text: "イン・アウトを消去"') -and
           $source.Contains('onTriggered: root.mvmController.clearInOut()')
}
if (-not (Test-RulerMarkMenu $qml) -or
    (Test-RulerMarkMenu $qml.Replace('rulerMarkMenu.popup();', '')) -or
    (Test-RulerMarkMenu $qml.Replace('height: visible ? implicitHeight : 0', ''))) {
    throw 'ルーラーの右クリックメニュー契約が崩れています'
}

function Test-DuplicatePreview([string]$source) {
    return $source.Contains('timelinePanel.activeDragDuplicate ? 0 : (bodyMoved') -and
           $source.Contains('y: timelinePanel.activeDragDuplicate ? 0 : (clipItem.bodyMoved') -and
           $source.Contains('visible: timelinePanel.activeDragDuplicate && timelinePanel.activeDragMoved') -and
           $source.Contains('opacity: 0.55')
}
if (-not (Test-DuplicatePreview $qml) -or
    (Test-DuplicatePreview $qml.Replace('opacity: 0.55', 'opacity: 1'))) {
    throw 'Alt+ドラッグ複製の元clipと半透明previewの契約が崩れています'
}

# 再生とクリップ削除はメニューとショートカットで行い、タイムライン上部の大きなボタンは置かない。
# メニュー項目は表示だけで sequence を持たない (Shortcut と二重に発火させない)。
function Test-TransportMenuContract([string]$source) {
    $playItem = 'CompactMenuItem\s*\{\s*text:\s*\(root\.mvmController\.playing \? "一時停止" : "再生"\) \+ "\\tSpace"[^{}]*onTriggered:\s*\{[^{}]*root\.mvmController\.playTimeline\(\)'
    $deleteItem = 'CompactMenuItem\s*\{\s*text:\s*\(root\.mvmController\.selectedTransitionId !== "" \? "トランジションを削除"\s*:\s*"クリップを削除"\) \+ "\\tDelete"[^{}]*onTriggered:\s*root\.mvmController\.deleteSelection\(\)'
    if ($source -notmatch $playItem -or $source -notmatch $deleteItem) { return $false }
    foreach ($item in [regex]::Matches($source, 'CompactMenuItem\s*\{\s*text:\s*[^\n]*(\n[^\n]*)?\\t(Space|Delete)"[^{}]*')) {
        if ($item.Value -match 'shortcut:|sequence:') { return $false }
    }
    if ($source -match 'Button\s*\{\s*text:\s*"クリップ削除"' -or
        $source -match 'Button\s*\{\s*text:\s*root\.mvmController\.playing \? "一時停止" : "再生"') {
        return $false
    }
    return $true
}
$transportButtons = $qml.Replace('        // --- タイムライン ---', "            Button {`n                text: `"クリップ削除`"`n            }`n        // --- タイムライン ---")
if (-not (Test-TransportMenuContract $qml) -or
    (Test-TransportMenuContract $transportButtons) -or
    (Test-TransportMenuContract $qml.Replace('+ "\tDelete"', '+ "\tDelete"; shortcut: "Delete"')) -or
    (Test-TransportMenuContract $qml.Replace('+ "\tSpace"', ''))) {
    throw '再生・クリップ削除のメニュー契約が崩れています'
}

# 素材はドロップした位置へ置く。外部ファイルもプロジェクトパネルからの素材も、
# タイムライン上なら位置 (track と frame) を渡し、それ以外はパネルへの登録だけにする。
function Test-MediaDropContract([string]$source) {
    $external = 'const target = root\.timelineDropTarget\(videoDropArea, drop\.x, drop\.y\);[\s\S]*?if \(target\)\s*root\.mvmController\.addMediaFilesToTimelineAt\(urls, target\.kind, target\.index,\s*target\.frame\);\s*else\s*projectPanel\.importUrls\(urls\);'
    $bin = 'DropArea\s*\{\s*id:\s*mediaBinDropArea[\s\S]*?keys:\s*\["mvm-media-bin"\][\s\S]*?const ids = projectPanel\.dragIds\.slice\(\);[\s\S]*?Qt\.callLater\(\(\) => root\.mvmController\.addMediaItemsToTimelineAt\(\s*ids, target\.kind, target\.index, target\.frame\)\)'
    return $source -match $external -and $source -match $bin -and
           $source.Contains('function timelineDropTarget(item, x, y)') -and
           $source -notmatch 'addMediaFileToTimeline\(url\)'
}
if (-not (Test-MediaDropContract $qml) -or
    (Test-MediaDropContract $qml.Replace('keys: ["mvm-media-bin"]', 'keys: ["other"]')) -or
    (Test-MediaDropContract $qml.Replace('const ids = projectPanel.dragIds.slice();', 'const ids = projectPanel.dragIds;')) -or
    (Test-MediaDropContract ($qml -replace '\s*else\s*projectPanel\.importUrls\(urls\);', '')) -or
    (Test-MediaDropContract ($qml + "`nroot.mvmController.addMediaFileToTimeline(url)"))) {
    throw '素材ドロップの配置契約が崩れています'
}
# プレビュー上の枠: 画像・動画は枠とハンドルで動かし、文字も同じ吸着を使う。
# 吸着は Ctrl で切り、ドラッグ中は preview だけを更新して離したときに 1 つの undo で確定する。
$overlaySource = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\PreviewTransformOverlay.qml') -Raw
function Test-PreviewTransformContract([string]$mainSource, [string]$overlay) {
    return $mainSource.Contains('import "PreviewTransform.js" as Transform') -and
           $mainSource -match 'PreviewTransformOverlay\s*\{\s*id:\s*previewTransform' -and
           $mainSource -match 'Transform\.snapMove\(\s*textLayer\.bounds[\s\S]*?!\(mouse\.modifiers & Qt\.ControlModifier\)\)' -and
           $overlay.Contains('import "PreviewTransform.js" as Transform') -and
           $overlay -match 'Transform\.snapMove\([\s\S]*?!\(mouse\.modifiers & Qt\.ControlModifier\)\)' -and
           $overlay.Contains('"keepAspect": (mouse.modifiers & Qt.ControlModifier) !== 0') -and
           $overlay.Contains('"fromCenter": (mouse.modifiers & Qt.AltModifier) !== 0') -and
           $overlay -match 'mvmController\.setClipEffectValues\(dragClipId, values, false\)' -and
           $overlay -match 'if \(commit\)\s*mvmController\.setClipEffectValues\(dragClipId, lastValues, true\);' -and
           $overlay -notmatch 'mvmController\.setEffectValues\(' -and
           # 回転した素材もハンドルで拡縮する (回転を理由にハンドルを隠さない)。
           $overlay.Contains('Transform.resizeRotatedRect(') -and
           $overlay -notmatch 'rotation\s*===\s*0'
}
if (-not (Test-PreviewTransformContract $qml $overlaySource) -or
    (Test-PreviewTransformContract $qml ($overlaySource + "`n    readonly property bool resizable: shown && geometry.rotation === 0")) -or
    (Test-PreviewTransformContract $qml $overlaySource.Replace('!(mouse.modifiers & Qt.ControlModifier)', 'true')) -or
    (Test-PreviewTransformContract $qml $overlaySource.Replace('setClipEffectValues(dragClipId, values, false)', 'setEffectValues(values, false)')) -or
    (Test-PreviewTransformContract $qml $overlaySource.Replace('Qt.AltModifier', 'Qt.ShiftModifier')) -or
    (Test-PreviewTransformContract $qml.Replace('!(mouse.modifiers & Qt.ControlModifier)', 'true') $overlaySource)) {
    throw 'プレビュー上の枠 (移動・拡縮・吸着) の契約が崩れています'
}

$panelDrag = 'id:\s*dragProxy\s*//[^\n]*\n\s*parent:\s*Overlay\.overlay'
if ($projectPanel -notmatch $panelDrag -or
    $projectPanel.Replace('parent: Overlay.overlay', '') -match $panelDrag) {
    throw 'プロジェクトパネルのドラッグ表示がタイムラインまで届きません'
}

Write-Output 'timeline UI architecture: PASS'
