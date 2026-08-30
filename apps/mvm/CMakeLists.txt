qt_add_executable(mvm
    main.cpp
    mvm_controller.cpp
)

qt_add_qml_module(mvm
    URI mvm.app
    VERSION 1.0
    RESOURCE_PREFIX /
    QML_FILES Main.qml
)

target_include_directories(mvm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm PRIVATE
    mvm::manim_clip_workflow
    mvm::preview_engine
    mvm::preview_qt
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    mvm::warnings
)

set_target_properties(mvm PROPERTIES WIN32_EXECUTABLE OFF)
