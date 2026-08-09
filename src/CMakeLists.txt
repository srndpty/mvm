# Phase 0: 依存が正しく解決され、実行時にも動作することを確認するための
# 最小ターゲットのみ。製品コードはまだ無い。

# --- Windows / UTF-8 ヘルパ -------------------------------------------------
# Phase 0 スパイク用の小さなヘルパ。製品用の platform 層ではない。
add_library(mvm_util STATIC
    util/mvm_win_utf8.c
)
target_include_directories(mvm_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
add_library(mvm::util ALIAS mvm_util)

# --- MLT adapter ------------------------------------------------------------
# MLT のヘッダを include してよいのはこのターゲットのソースだけ。
# 上位 (mvm_bench の CLI 部分など) は MLT の型を一切知らない。
add_library(mvm_mlt STATIC
    media/mlt/mvm_mlt_runtime.c
    media/mlt/mvm_mlt_probe.c
    media/mlt/mvm_mlt_explore.c
    media/mlt/mvm_mlt_compose.c
    media/mlt/mvm_mlt_audiograph.c
)
target_include_directories(mvm_mlt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_mlt PUBLIC mvm::util PRIVATE MLT::MLT)
target_compile_definitions(mvm_mlt PRIVATE
    MVM_MLT_VERSION_STRING="${MLT_VERSION}"
)
add_library(mvm::mlt ALIAS mvm_mlt)

# --- MLT hello world --------------------------------------------------------
# MVM_MLT_PATH_DEFS はトップレベルの CMakeLists.txt で定義している
# (src/ と tests/ の両方から使うため)。
add_executable(mvm_mlt_hello media/mlt/mlt_hello.c)
target_link_libraries(mvm_mlt_hello PRIVATE mvm::mlt)
target_compile_definitions(mvm_mlt_hello PRIVATE
    MVM_MLT_VERSION_STRING="${MLT_VERSION}"
    ${MVM_MLT_PATH_DEFS}
)

# --- Phase 1 / P1: GPU preview スパイク --------------------------------------
# FFmpeg (D3D11VA) + D3D11。**Qt には依存しない。**
# QRhi / QQuickRhiItem を触るコードは src/app/preview/ にだけ置く
# (docs/phase1-plan.md §7)。MLT にも依存しない。Phase 0 のコードを流用しない。
add_library(mvm_gpu_preview STATIC
    media/gpu_preview/gpu_frame.cpp
    media/gpu_preview/timebase.cpp
    media/gpu_preview/color_metadata.cpp
    media/gpu_preview/d3d11_shared_device.cpp
    media/gpu_preview/ffmpeg_d3d11_decoder.cpp
    media/gpu_preview/nv12_converter.cpp
    media/gpu_preview/frame_queue.cpp
    media/gpu_preview/gpu_completion.cpp
    media/gpu_preview/display_ledger.cpp
    media/gpu_preview/device_change.cpp
    media/gpu_preview/lifecycle.cpp
    media/gpu_preview/decode_worker.cpp
    media/gpu_preview/source_registry.cpp
    media/gpu_preview/source_frame_buffer.cpp
    media/gpu_preview/composition_schedule.cpp
    media/gpu_preview/compositor_coordinator.cpp
    media/gpu_preview/phase4_composition_catalog.cpp
    media/gpu_preview/phase4_composition_driver.cpp
    media/gpu_preview/shutdown_sequence.cpp
    media/gpu_preview/exact_frame_pairer.cpp
    media/gpu_preview/source_decode_worker.cpp
    media/gpu_preview/gpu_compositor.cpp
    media/gpu_preview/composition_display_ledger.cpp
    media/gpu_preview/transition_probe.cpp
    media/gpu_preview/transition_probe_reference.cpp
    media/gpu_preview/output_scheduler.cpp
)
target_include_directories(mvm_gpu_preview PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_gpu_preview
    PUBLIC  PkgConfig::MVM_FFMPEG d3d11 dxgi
    PRIVATE d3dcompiler mvm::warnings
)
add_library(mvm::gpu_preview ALIAS mvm_gpu_preview)

# --- Phase 3 / A: audio pipeline --------------------------------------------
# FFmpeg audio decode / bounded PCM queue / WASAPI / IAudioClock。Qt および
# video scheduler へ依存させず、P2 runtime path を変更しない。
add_library(mvm_audio_preview STATIC
    media/audio_preview/audio_frame_queue.cpp
    media/audio_preview/audio_clock.cpp
    media/audio_preview/audio_video_scheduler.cpp
    media/audio_preview/audio_decode_worker.cpp
    media/audio_preview/wasapi_audio_sink.cpp
)
target_include_directories(mvm_audio_preview PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_audio_preview
    PUBLIC PkgConfig::MVM_FFMPEG
    PRIVATE ole32 uuid avrt mvm::warnings
)
add_library(mvm::audio_preview ALIAS mvm_audio_preview)

# --- Qt probe ---------------------------------------------------------------
# Qt が UCRT64 版であり、実際にリンク・起動できることを確認する。
if(MVM_ENABLE_QT)
    add_executable(mvm_qt_probe app/qt_probe.cpp)
    target_link_libraries(mvm_qt_probe PRIVATE Qt6::Core mvm::warnings)

    # --- Phase 1 / P1: Qt 隔離層 --------------------------------------------
    # **QRhi (Qt の private API) を include してよいのはこのターゲットだけ。**
    # Phase 0 で「MLT のヘッダを include してよいのは src/media/mlt/ だけ」と
    # 決めたのと同じ規則を QRhi に対して適用する (docs/phase1-plan.md §7)。
    add_library(mvm_preview_qt STATIC
        app/preview/display_target_contract.cpp
        app/preview/preview_rhi_item.cpp
        app/preview/compositor_rhi_item.cpp
    )
    set_target_properties(mvm_preview_qt PROPERTIES AUTOMOC ON)
    target_include_directories(mvm_preview_qt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mvm_preview_qt
        PUBLIC  mvm::gpu_preview mvm::audio_preview Qt6::Quick Qt6::Gui
        PRIVATE Qt6::GuiPrivate mvm::warnings
    )
    add_library(mvm::preview_qt ALIAS mvm_preview_qt)
endif()
