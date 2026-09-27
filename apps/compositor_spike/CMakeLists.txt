qt_add_executable(mvm_compositor_spike
    main.cpp
    compositor_spike_controller.cpp
)

qt_add_qml_module(mvm_compositor_spike
    URI mvm.compositor_spike
    VERSION 1.0
    RESOURCE_PREFIX /
    SOURCES ${CMAKE_SOURCE_DIR}/src/app/preview/qml/compositor_surface_qml.h
    QML_FILES Main.qml
)

target_include_directories(mvm_compositor_spike PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/app/preview/qml)
target_link_libraries(mvm_compositor_spike PRIVATE
    mvm::gpu_preview
    mvm::preview_qt
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    dwmapi
    mvm::warnings
)
set_target_properties(mvm_compositor_spike PROPERTIES WIN32_EXECUTABLE OFF)
