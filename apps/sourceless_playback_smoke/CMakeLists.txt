qt_add_executable(mvm_sourceless_playback_smoke main.cpp)
target_link_libraries(mvm_sourceless_playback_smoke PRIVATE
    mvm::preview_engine
    mvm::preview_qt
    Qt6::Gui
    Qt6::Quick
    mvm::warnings
)
set_target_properties(mvm_sourceless_playback_smoke PROPERTIES WIN32_EXECUTABLE OFF)
