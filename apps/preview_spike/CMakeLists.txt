# Phase 1 / P1 の検証アプリ。**製品アプリではない。**
# decode -> D3D11 -> Qt Quick の縦切りを動かし、計測 JSON を出す。

qt_add_executable(mvm_preview_spike
    main.cpp
    spike_controller.cpp
)

qt_add_qml_module(mvm_preview_spike
    URI mvm.preview_spike
    VERSION 1.0
    RESOURCE_PREFIX /
    QML_FILES Main.qml
)

target_include_directories(mvm_preview_spike PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(mvm_preview_spike PRIVATE
    mvm::gpu_preview
    mvm::preview_qt
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    psapi
    mvm::warnings
)

# Windows の GUI アプリだが、計測モードの出力を親シェルへ出したいので
# コンソールを外さない。CTest から終了コードと標準出力を見るためである。
set_target_properties(mvm_preview_spike PROPERTIES WIN32_EXECUTABLE OFF)
