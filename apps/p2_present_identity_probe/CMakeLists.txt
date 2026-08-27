add_executable(mvm_p2_present_identity_probe main.cpp)
target_link_libraries(mvm_p2_present_identity_probe PRIVATE
    mvm::gpu_preview
    d3d11
    dxgi
    dwmapi
    mvm::warnings
)
