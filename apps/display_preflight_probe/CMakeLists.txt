qt_add_executable(mvm_display_preflight_probe
    main.cpp
)

qt_add_qml_module(mvm_display_preflight_probe
    URI mvm.display_preflight_probe
    VERSION 1.0
    RESOURCE_PREFIX /
    QML_FILES Main.qml
)

target_link_libraries(mvm_display_preflight_probe PRIVATE
    mvm::preview_qt
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    mvm::warnings
)
set_target_properties(mvm_display_preflight_probe PROPERTIES WIN32_EXECUTABLE OFF)
