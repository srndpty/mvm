qt_add_executable(mvm_p5e_preview_smoke main.cpp)
target_link_libraries(mvm_p5e_preview_smoke PRIVATE
    mvm::preview_engine
    mvm::preview_qt
    Qt6::Gui
    Qt6::Quick
    mvm::warnings
)
set_target_properties(mvm_p5e_preview_smoke PROPERTIES WIN32_EXECUTABLE OFF)
