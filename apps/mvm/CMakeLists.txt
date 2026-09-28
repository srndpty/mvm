qt_add_executable(mvm
    main.cpp
    media_bin_model.cpp
    media_import.cpp
    mvm_controller.cpp
    shuttle_audio_mix.cpp
    shuttle_audio_playback.cpp
    timeline_clip_model.cpp
    track_model.cpp
    waveform_cache.cpp
    waveform_view.cpp
)

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
        ModernDialog.qml
        ModernDialogFooter.qml
        ModernDialogButton.qml
        ModernDialogField.qml
        ModernDialogOption.qml
        ModernDialogComboBox.qml
        ModernDialogProgressBar.qml
        DragNumberField.qml
        AudioMeter.qml
        MediaKindIcon.qml
        TimelineGestures.js
        TimelineToolIcon.qml
        TimelineToolPanel.qml
        TextClipInspector.qml
        ModernColorPicker.qml
        ProjectPanel.qml
)

target_include_directories(mvm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm PRIVATE
    mvm::core
    mvm::project
    mvm::util
    mvm::manim_clip_workflow
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
    media_bin_model.cpp
    media_import.cpp
    mvm_controller.cpp
    shuttle_audio_mix.cpp
    shuttle_audio_playback.cpp
    timeline_clip_model.cpp
    track_model.cpp
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
        ModernDialog.qml
        ModernDialogFooter.qml
        ModernDialogButton.qml
        ModernDialogField.qml
        ModernDialogOption.qml
        ModernDialogComboBox.qml
        ModernDialogProgressBar.qml
        DragNumberField.qml
        AudioMeter.qml
        MediaKindIcon.qml
        TimelineGestures.js
        TimelineToolIcon.qml
        TimelineToolPanel.qml
        TextClipInspector.qml
        ModernColorPicker.qml
        ProjectPanel.qml
)
target_include_directories(mvm_test_text_ui_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_test_text_ui_input PRIVATE
    mvm::core mvm::project mvm::util mvm::manim_clip_workflow
    mvm::timeline_playback mvm::timeline_preview_mapping mvm::timeline_export
    mvm::text_raster mvm::audio_waveform mvm::still_image mvm::mlt mvm::preview_engine
    mvm::audio_preview
    mvm::preview_qt Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Test
    mvm::warnings)
target_compile_definitions(mvm_test_text_ui_input PRIVATE ${MVM_MLT_PATH_DEFS}
    MVM_TEXT_TEST_VIDEO="${CMAKE_SOURCE_DIR}/tests/assets/smoke/v1080p60_h264.mp4")
