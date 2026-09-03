qt_add_executable(mvm
    main.cpp
    mvm_controller.cpp
    timeline_clip_model.cpp
    track_model.cpp
)

qt_add_qml_module(mvm
    URI mvm.app
    VERSION 1.0
    RESOURCE_PREFIX /
    QML_FILES
        Main.qml
        DragNumberField.qml
        AudioMeter.qml
)

target_include_directories(mvm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm PRIVATE
    mvm::core
    mvm::project
    mvm::manim_clip_workflow
    mvm::timeline_playback
    mvm::timeline_preview_mapping
    mvm::timeline_export
    mvm::mlt
    mvm::preview_engine
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
