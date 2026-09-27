$ErrorActionPreference = 'Stop'
$qmlPath = Join-Path $PSScriptRoot '..\..\apps\mvm\Main.qml'
$controllerPath = Join-Path $PSScriptRoot '..\..\apps\mvm\mvm_controller.cpp'
$controllerHeaderPath = Join-Path $PSScriptRoot '..\..\apps\mvm\mvm_controller.h'
$mainPath = Join-Path $PSScriptRoot '..\..\apps\mvm\main.cpp'
$previewItemPath = Join-Path $PSScriptRoot '..\..\src\app\preview\preview_engine_rhi_item.cpp'
$compositorPath = Join-Path $PSScriptRoot '..\..\src\media\gpu_preview\gpu_compositor.cpp'
$qml = Get-Content -LiteralPath $qmlPath -Raw
$projectPanel = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\ProjectPanel.qml') -Raw
$compactMenu = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\CompactMenu.qml') -Raw
$compactItem = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\CompactMenuItem.qml') -Raw
$compactSeparator = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\CompactMenuSeparator.qml') -Raw
$controller = Get-Content -LiteralPath $controllerPath -Raw
$controllerHeader = Get-Content -LiteralPath $controllerHeaderPath -Raw
$main = Get-Content -LiteralPath $mainPath -Raw
$previewItem = Get-Content -LiteralPath $previewItemPath -Raw
$compositor = Get-Content -LiteralPath $compositorPath -Raw
$waveformView = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\waveform_view.h') -Raw
$previewSurface = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\apps\mvm\preview_surface_qml.h') -Raw

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
        @{ Tool = 'rate'; Key = 'R'; Available = 'false' },
        @{ Tool = 'slip'; Key = 'Y'; Available = 'true' },
        @{ Tool = 'slide'; Key = 'U'; Available = 'true' },
        @{ Tool = 'pen'; Key = 'P'; Available = 'true' },
        @{ Tool = 'hand'; Key = 'H'; Available = 'true' },
        @{ Tool = 'zoom'; Key = 'Z'; Available = 'true' },
        @{ Tool = 'text'; Key = 'T'; Available = 'false' }
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
# どれか 1 つを壊すと上の検査が落ちることを確かめる。
foreach ($broken in @(
    @{ Panel = $toolPanel.Replace('key: "C"', 'key: "X"'); Main = $qml },
    @{ Panel = $toolPanel -replace '(tool:\s*"rate"[^{}]*available:\s*)false', '${1}true'; Main = $qml },
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
    'root.mvmController.selectTimelineClip(clipItem.clipId, frame, clipItem.editLinked)',
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
    '-clipItem.timelineStartFrame * timelinePanel.pixelsPerFrame',
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
    'root.mvmController.addVideoClip(url)',
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

if (-not $main.Contains('class TimelineWheelEventFilter final') -or
    -not $main.Contains('window->installEventFilter(&timelineWheelFilter)') -or
    -not $main.Contains('testFlag(Qt::AltModifier)') -or
    -not $main.Contains('testFlag(Qt::ControlModifier)') -or
    -not $main.Contains('testFlag(Qt::ShiftModifier)') -or
    -not $main.Contains('method = "handleNativePlainWheel"') -or
    -not $main.Contains('angleDelta.x()') -or
    -not $main.Contains('pixelDelta.x()') -or
    -not $main.Contains('if (delta == 0)') -or
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
foreach ($needle in @('project::timelineClipIndexAt(project_, current.track, clamped)',
                      'setTimelineSelection({clipId.toStdString()});',
                      'UndoEntry undo{project_, selectedClipIds_, currentClipId(), playheadFrame_, currentRevision_};',
                      'std::vector<std::string> deletedIds = selectedClipIds_;',
                      'for (const auto& id : deletedIds)',
                      'project_ = entry.project;',
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
foreach ($needle in @('FILE_FLAG_DELETE_ON_CLOSE',
                      'ERROR_SHARING_VIOLATION',
                      'project::saveProjectRecovery(',
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
# preview の layer 構成は mapTimelinePreviewFrame に一本化する。
if (-not $controller.Contains('mapTimelinePreviewFrame(project_, timelineFrame)')) {
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
if (-not $qml.Contains('root.mvmController.outputWidth') -or
    -not $qml.Contains('root.mvmController.outputHeight')) {
    throw 'previewがProject output sizeの縦横比を使っていません'
}
if (-not $compositor.Contains('aspectFit(croppedWidth, croppedHeight, destinationBox.width,')) {
    throw '製品compositorが素材の縦横比を保持していません'
}

Write-Output 'timeline UI architecture: PASS'
