qt_add_executable(mvm_p2_vblank_authority_probe
    main.cpp
)

qt_add_qml_module(mvm_p2_vblank_authority_probe
    URI mvm.p2_vblank_authority_probe
    VERSION 1.0
    RESOURCE_PREFIX /
    SOURCES ${CMAKE_SOURCE_DIR}/src/app/preview/qml/compositor_surface_qml.h
    QML_FILES Main.qml
)

# CompositorSurface の QML 登録用ヘッダを basename で include させる。
target_include_directories(mvm_p2_vblank_authority_probe PRIVATE ${CMAKE_SOURCE_DIR}/src/app/preview/qml)

target_link_libraries(mvm_p2_vblank_authority_probe PRIVATE
    mvm::preview_qt
    mvm::gpu_preview
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    dwmapi
    mvm::warnings
)
set_target_properties(mvm_p2_vblank_authority_probe PROPERTIES WIN32_EXECUTABLE OFF)
