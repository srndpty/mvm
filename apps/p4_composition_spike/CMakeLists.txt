qt_add_executable(mvm_p4_composition_spike
    main.cpp
    p4_composition_controller.cpp
)

qt_add_qml_module(mvm_p4_composition_spike
    URI mvm.p4_composition_spike
    VERSION 1.0
    RESOURCE_PREFIX /
    QML_FILES Main.qml
)

target_include_directories(mvm_p4_composition_spike PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_p4_composition_spike PRIVATE
    mvm::gpu_preview
    mvm::audio_preview
    mvm::preview_qt
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    mvm::warnings
)
set_target_properties(mvm_p4_composition_spike PROPERTIES WIN32_EXECUTABLE OFF)
