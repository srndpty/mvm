# 自動調整のジョブと GUI 連携を全 controller 試験でも同じ実装で使う。
add_library(mvm_audio_adjustment STATIC audio_adjustment_job.cpp audio_adjustment_controller.cpp)
set_target_properties(mvm_audio_adjustment PROPERTIES AUTOMOC OFF)
target_include_directories(mvm_audio_adjustment PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_audio_adjustment PUBLIC mvm::audio_analysis mvm::project
    mvm::audio_preview mvm::timeline_preview_mapping Qt6::Core Qt6::Quick Qt6::Qml
    PRIVATE mvm::warnings)
add_library(mvm::audio_adjustment ALIAS mvm_audio_adjustment)

# 数式 clip の描画結果の cache。backend は preflight 関数で注入する (試験は偽の backend)。
add_library(mvm_math_raster_cache STATIC math_raster_cache.cpp math_equation_sequence_artifact.cpp)
set_target_properties(mvm_math_raster_cache PROPERTIES AUTOMOC ON)
target_include_directories(mvm_math_raster_cache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_math_raster_cache
    PUBLIC mvm::math mvm::still_image Qt6::Core
    # 変形の backend の PNG は静止の mask と同じ decoder (loadMathCoverage) で読む。
    PRIVATE mvm::math_clip_render mvm::util mvm::warnings)
add_library(mvm::math_raster_cache ALIAS mvm_math_raster_cache)

qt_add_executable(mvm
    main.cpp
    image_raster_cache.cpp
    media_bin_model.cpp
    media_import.cpp
    press_focus.cpp
    subtitle_controller.cpp
    mvm_controller.cpp
    shuttle_audio_mix.cpp
    shuttle_audio_playback.cpp
    clip_sample_reader.cpp
    scrub_audio_grain.cpp
    scrub_audio_playback.cpp
    timeline_clip_model.cpp
    track_model.cpp
    trim_cursor.cpp
    waveform_cache.cpp
    waveform_view.cpp
)

# 状態を持たない JS の関数集。QML からは相対パスで import する (`import "TimelineGestures.js" as
# Gestures`) ので、module の qmldir には載せない。載せると Qt は `.pragma library` の無い JS を
# 各 QML 文書で評価し直すと警告する。`.pragma library` は VS Code の JavaScript 検査が構文エラーに
# するため書かない (docs/premiere-like-editing.md §19.8)。下の mvm_test_text_ui_input も同じ。
set_source_files_properties(TimelineGestures.js PreviewTransform.js TransitionEditorMath.js
    TrackEyePaint.js SubtitleTime.js
    PROPERTIES QT_QML_SKIP_QMLDIR_ENTRY TRUE)

qt_add_qml_module(mvm
    URI mvm.app
    VERSION 1.0
    RESOURCE_PREFIX /
    SOURCES
        preview_surface_qml.h
    QML_FILES
        Main.qml
        CompactMenu.qml
        CompactMenuItem.qml
        CompactMenuSeparator.qml
        BoundedScrollView.qml
        BoundedListView.qml
        BoundedFlickable.qml
        ModernDialog.qml
        ModernDialogFooter.qml
        ModernDialogButton.qml
        ModernDialogTextArea.qml
        ModernDialogCheckBox.qml
        ModernDialogField.qml
        ModernDialogOption.qml
        ModernDialogComboBox.qml
        ModernDialogProgressBar.qml
        ChevronIcon.qml
        StyleNumberField.qml
        TextStyleEditor.qml
        DragNumberField.qml
        AudioMeter.qml
        AudioValueField.qml
        AudioFader.qml
        AudioPanKnob.qml
        AudioMixerStrip.qml
        AudioMixerPanel.qml
        AutoAudioDialog.qml
        MediaKindIcon.qml
        TimelineGestures.js
        PreviewTransform.js
        PreviewTransformOverlay.qml
        TimelineToolIcon.qml
        TrackEyeIcon.qml
        TimelineToolPanel.qml
        SubtitlePanel.qml
        TranscribeDialog.qml
        TextClipInspector.qml
        MathClipInspector.qml
        MathDependencyGuidance.qml
        KeyframeInspector.qml
        TransitionInspector.qml
        TransitionEditorMath.js
        TrackEyePaint.js
        SubtitleTime.js
        ModernColorPicker.qml
        ProjectPanel.qml
)

target_include_directories(mvm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm PRIVATE
    mvm::core
    mvm::audio_adjustment
    mvm::project
    mvm::util
    mvm::transcribe mvm::manim_clip_workflow mvm::math_raster_cache mvm::math_clip_render
    mvm::equation_sequence_compile
    mvm::timeline_playback
    mvm::timeline_preview_mapping
    mvm::timeline_export
    mvm::text_raster
    mvm::audio_waveform
    mvm::still_image
    mvm::mlt
    mvm::preview_engine
    mvm::audio_preview
    mvm::preview_qt
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    mvm::warnings
)

# MLT の module / data directory は推測させず build 時に確定した値を埋め込む。
target_compile_definitions(mvm PRIVATE ${MVM_MLT_PATH_DEFS})

set_target_properties(mvm PROPERTIES WIN32_EXECUTABLE OFF)

# 横書き文字ツールの直接入力の検査 (tests/harness/test_text_ui_input.cpp)。
# 製品の Main.qml を同じ QML module として組み込むため、QML と同じ directory で定義する。
# D3D11 の window へ QTest で key / mouse event を送る。
# test の登録は tests/CMakeLists.txt で行う (このファイルは include(CTest) より前に読まれる)。
# IME の変換確定は扱わない (手動確認)。
find_package(Qt6 REQUIRED COMPONENTS Test)
qt_add_executable(mvm_test_text_ui_input
    ${CMAKE_SOURCE_DIR}/tests/harness/test_text_ui_input.cpp
    ${CMAKE_SOURCE_DIR}/tests/harness/math_p28_acceptance.cpp
    image_raster_cache.cpp
    media_bin_model.cpp
    media_import.cpp
    press_focus.cpp
    subtitle_controller.cpp
    mvm_controller.cpp
    shuttle_audio_mix.cpp
    shuttle_audio_playback.cpp
    clip_sample_reader.cpp
    scrub_audio_grain.cpp
    scrub_audio_playback.cpp
    timeline_clip_model.cpp
    track_model.cpp
    trim_cursor.cpp
    waveform_cache.cpp
    waveform_view.cpp
)
qt_add_qml_module(mvm_test_text_ui_input
    URI mvm.app
    VERSION 1.0
    RESOURCE_PREFIX /
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/text_ui_qml/mvm/app
    SOURCES
        preview_surface_qml.h
    QML_FILES
        Main.qml
        CompactMenu.qml
        CompactMenuItem.qml
        CompactMenuSeparator.qml
        BoundedScrollView.qml
        BoundedListView.qml
        BoundedFlickable.qml
        ModernDialog.qml
        ModernDialogFooter.qml
        ModernDialogButton.qml
        ModernDialogTextArea.qml
        ModernDialogCheckBox.qml
        ModernDialogField.qml
        ModernDialogOption.qml
        ModernDialogComboBox.qml
        ModernDialogProgressBar.qml
        ChevronIcon.qml
        StyleNumberField.qml
        TextStyleEditor.qml
        DragNumberField.qml
        AudioMeter.qml
        AudioValueField.qml
        AudioFader.qml
        AudioPanKnob.qml
        AudioMixerStrip.qml
        AudioMixerPanel.qml
        AutoAudioDialog.qml
        MediaKindIcon.qml
        TimelineGestures.js
        PreviewTransform.js
        PreviewTransformOverlay.qml
        TimelineToolIcon.qml
        TrackEyeIcon.qml
        TimelineToolPanel.qml
        SubtitlePanel.qml
        TranscribeDialog.qml
        TextClipInspector.qml
        MathClipInspector.qml
        MathDependencyGuidance.qml
        KeyframeInspector.qml
        TransitionInspector.qml
        TransitionEditorMath.js
        TrackEyePaint.js
        SubtitleTime.js
        ModernColorPicker.qml
        ProjectPanel.qml
)
target_include_directories(mvm_test_text_ui_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_test_text_ui_input PRIVATE
    mvm::audio_adjustment
    mvm::core mvm::project mvm::util mvm::transcribe mvm::manim_clip_workflow mvm::math_raster_cache mvm::math_clip_render
    mvm::equation_sequence_compile
    mvm::timeline_playback mvm::timeline_preview_mapping mvm::timeline_export
    mvm::text_raster mvm::audio_waveform mvm::still_image mvm::mlt mvm::preview_engine
    mvm::audio_preview
    mvm::preview_qt Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test
    mvm::warnings)
# 試験の window を OS の前面にせずにフォーカスを持たせるため、QPA の
# QWindowSystemInterface を使う (tests/harness/test_window_focus.h)。この試験だけ。
target_link_libraries(mvm_test_text_ui_input PRIVATE Qt6::GuiPrivate)
target_include_directories(mvm_test_text_ui_input PRIVATE ${CMAKE_SOURCE_DIR}/tests/harness)
target_compile_definitions(mvm_test_text_ui_input PRIVATE ${MVM_MLT_PATH_DEFS}
    MVM_TEXT_TEST_VIDEO="${CMAKE_SOURCE_DIR}/tests/assets/smoke/v1080p60_h264.mp4")
