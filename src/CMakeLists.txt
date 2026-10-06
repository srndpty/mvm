# Phase 0: 依存が正しく解決され、実行時にも動作することを確認するための
# 最小ターゲットのみ。製品コードはまだ無い。

# --- pure core --------------------------------------------------------------
add_library(mvm_core STATIC
    core/clip_fade.cpp
    core/checked_output_timebase.cpp
    core/export_eta.cpp
    core/layer_placement.cpp
    core/presentation_opportunity_mapper.cpp
    core/source_frame_mapping.cpp
    core/timecode.cpp
    core/text_offsets.cpp
    core/waveform_peaks.cpp
)
target_include_directories(mvm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_core PRIVATE mvm::warnings)
add_library(mvm::core ALIAS mvm_core)

# --- minimal product Project Model -----------------------------------------
# Qt / MLT / Manim に依存しない純粋な値型と schema 固定 JSON のみ。
add_library(mvm_project STATIC
    project/audio_adjustment.cpp
    project/clip_effects.cpp
    project/math_clip.cpp
    project/equation_sequence.cpp
    project/equation_sequence_edit.cpp
    project/equation_binding_edit.cpp
    project/media_bin.cpp
    project/path_identity.cpp
    project/subtitles.cpp
    project/project.cpp
    project/project_json.cpp
    project/timeline_edit.cpp
    project/timeline_render.cpp
)
target_include_directories(mvm_project PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# output frame rate の表 (measured / configurable) は mvm::core に一本化している
# ため PUBLIC で持つ。
target_link_libraries(mvm_project PUBLIC mvm::core PRIVATE mvm::warnings)
add_library(mvm::project ALIAS mvm_project)

# --- Windows / UTF-8 ヘルパ -------------------------------------------------
# Phase 0 スパイク用の小さなヘルパ。製品用の platform 層ではない。
add_library(mvm_util STATIC
    util/mvm_atomic_write.c
    util/mvm_win_utf8.c
    util/mvm_long_path.c
    util/mvm_reveal_in_explorer.c
    util/mvm_file_identity.c
    util/mvm_process.c
    util/mvm_sha256.c
)
target_include_directories(mvm_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_util PRIVATE shell32 ole32 bcrypt)
add_library(mvm::util ALIAS mvm_util)
# Project JSONのatomic置換はWin32 helperに閉じる。QtやMLTはここへ入れない。
target_link_libraries(mvm_project PRIVATE mvm::util)

# --- 数式 renderer の中立な契約 ---------------------------------------------
# backend (Manim など) の型も Project の型も含めない。cache key と出力 raster への配置。
add_library(mvm_math STATIC
    media/math/equation_sequence_render.cpp
    media/math/math_render.cpp
    media/math/math_raster_layout.cpp
    media/math/math_tex_segments.cpp
    media/math/math_transform.cpp
)
target_include_directories(mvm_math PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_math PUBLIC mvm::util PRIVATE mvm::warnings)
add_library(mvm::math ALIAS mvm_math)

# Project の binding と P2 の純粋な計算を結ぶ。描画 backend / Qt は参照しない。
add_library(mvm_equation_sequence_compile STATIC app/equation_sequence_compile.cpp
    app/equation_sequence_render.cpp app/equation_sequence_preview.cpp)
target_include_directories(mvm_equation_sequence_compile PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_equation_sequence_compile PUBLIC mvm::project mvm::math PRIVATE mvm::warnings)
add_library(mvm::equation_sequence_compile ALIAS mvm_equation_sequence_compile)

# --- Manim external renderer ------------------------------------------------
# Python を埋め込まず、明示された Manim CLI を外部プロセスとして実行する。
# 依存の向きは manim -> math (中立な契約は manim を知らない)。
add_library(mvm_manim STATIC
    media/manim/manim_fingerprint.cpp
    media/manim/manim_equation_sequence.cpp
    media/manim/manim_math_tex.cpp
    media/manim/manim_renderer.cpp
)
target_include_directories(mvm_manim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_manim PUBLIC mvm::util mvm::math PRIVATE mvm::warnings ole32)
add_library(mvm::manim ALIAS mvm_manim)

# --- M2: Manim clip orchestration ------------------------------------------
# QtやPreviewEngineを含めず、render・fingerprint・Project保存の順序だけを担う。
add_library(mvm_manim_clip_workflow STATIC
    app/manim_clip_workflow.cpp
)
target_include_directories(mvm_manim_clip_workflow PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_manim_clip_workflow
    PUBLIC mvm::project mvm::manim
    PRIVATE mvm::warnings)
add_library(mvm::manim_clip_workflow ALIAS mvm_manim_clip_workflow)

# --- M6b: controller clockによる単一track timeline再生 ---------------------
# QtやPreviewEngineには依存せず、経過時間のframe変換とclip境界判断だけを担う。
add_library(mvm_timeline_playback STATIC
    app/timeline_playback.cpp
)
target_include_directories(mvm_timeline_playback PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_timeline_playback PUBLIC mvm::project PRIVATE mvm::warnings)
add_library(mvm::timeline_playback ALIAS mvm_timeline_playback)

add_library(mvm_timeline_preview_mapping STATIC
    app/timeline_preview_mapping.cpp
)
target_include_directories(mvm_timeline_preview_mapping PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_timeline_preview_mapping PUBLIC mvm::project PRIVATE mvm::warnings)
add_library(mvm::timeline_preview_mapping ALIAS mvm_timeline_preview_mapping)

add_library(mvm_text_raster STATIC app/text_raster.cpp)
target_include_directories(mvm_text_raster PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_text_raster PUBLIC mvm::project Qt6::Gui PRIVATE mvm::warnings)
add_library(mvm::text_raster ALIAS mvm_text_raster)

# 数式 clip (Project) と中立な数式描画をつなぐ。preview と書き出しが共有する。Qt に依存しない。
add_library(mvm_math_clip_render STATIC app/math_clip_render.cpp)
target_include_directories(mvm_math_clip_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_math_clip_render
    PUBLIC mvm::project mvm::math mvm_still_image
    PRIVATE mvm::warnings)
add_library(mvm::math_clip_render ALIAS mvm_math_clip_render)

# --- M4: timeline export orchestration --------------------------------------
# Qt に依存しない。Project の clip 列を解決し、tmp へ書き出して検証してから
# 正規名へ rename する順序だけを担う。MLT の呼び出しは mvm::mlt に閉じる。
add_library(mvm_timeline_export STATIC
    app/timeline_export.cpp
)
target_include_directories(mvm_timeline_export PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_timeline_export
    PUBLIC mvm::project
    PRIVATE mvm::core mvm::mlt mvm::text_raster mvm::math_clip_render mvm_still_image
        mvm::warnings)
add_library(mvm::timeline_export ALIAS mvm_timeline_export)

# --- Phase 5 / P5-B: PreviewEngine product foundation -----------------------
# public boundaryは標準C++の値型だけを保ち、private実装でGPU backendへ接続する。
add_library(mvm_preview_engine STATIC
    preview_engine/preview_engine.cpp
)
target_include_directories(mvm_preview_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# P5-D2: audio-master transportをproduct engineへ接続する。public headerには
# WASAPI/FFmpeg/D3D11の型を出さないため、依存はすべてPRIVATEで閉じる。
target_link_libraries(mvm_preview_engine PRIVATE
    mvm_gpu_preview mvm::audio_preview mvm::core mvm::warnings)
add_library(mvm::preview_engine ALIAS mvm_preview_engine)

# --- MLT adapter ------------------------------------------------------------
# MLT のヘッダを include してよいのはこのターゲットのソースだけ。
# 上位 (mvm_bench の CLI 部分など) は MLT の型を一切知らない。
add_library(mvm_mlt STATIC
    media/mlt/mvm_mlt_runtime.c
    media/mlt/mvm_mlt_probe.c
    media/mlt/mvm_mlt_explore.c
    media/mlt/mvm_mlt_compose.c
    media/mlt/mvm_mlt_export.c
    media/mlt/mvm_mlt_effects_p0.c
    media/mlt/mvm_mlt_overlay_p0.c
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
    media/gpu_preview/still_image_frame.cpp
    media/gpu_preview/composition_display_ledger.cpp
    media/gpu_preview/transition_probe.cpp
    media/gpu_preview/validation_reference.cpp
    media/gpu_preview/transition_probe_reference.cpp
    media/gpu_preview/visible_uv.cpp
    media/gpu_preview/output_scheduler.cpp
    media/gpu_preview/scheduler_phase_attribution.cpp
    media/gpu_preview/presentation_opportunity_attribution.cpp
    media/gpu_preview/presentation_opportunity_scheduler.cpp
    media/gpu_preview/qualified_present_commit_join.cpp
    media/gpu_preview/required_intent_queue.cpp
    media/gpu_preview/preroll_transition_handshake.cpp
    media/gpu_preview/presentation_refresh_authority.cpp
    media/gpu_preview/window_output_vblank_authority.cpp
    media/gpu_preview/window_output_vblank_observer.cpp
    media/gpu_preview/physical_vblank_domain.cpp
)
target_include_directories(mvm_gpu_preview PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_gpu_preview
    PUBLIC  PkgConfig::MVM_FFMPEG d3d11 dxgi
    PRIVATE d3dcompiler mvm::core mvm::warnings
)
add_library(mvm::gpu_preview ALIAS mvm_gpu_preview)

# --- Phase 3 / A: audio pipeline --------------------------------------------
# FFmpeg audio decode / bounded PCM queue / WASAPI / IAudioClock。Qt および
# video scheduler へ依存させず、P2 runtime path を変更しない。
add_library(mvm_audio_preview STATIC
    media/audio_preview/audio_frame_queue.cpp
    media/audio_preview/audio_clock.cpp
    media/audio_preview/audio_video_scheduler.cpp
    media/audio_preview/checked_audio_video_scheduler.cpp
    media/audio_preview/audio_decode_worker.cpp
    media/audio_preview/pitch_preserving_stretcher.cpp
    media/audio_preview/wasapi_audio_sink.cpp
)
target_include_directories(mvm_audio_preview PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_audio_preview
    PUBLIC PkgConfig::MVM_FFMPEG mvm::core
    PRIVATE PkgConfig::MVM_RUBBERBAND ole32 uuid avrt mvm::warnings
)
add_library(mvm::audio_preview ALIAS mvm_audio_preview)

# --- timeline 波形 -----------------------------------------------------------
# 素材の音声を元の channel 数のまま decode し、表示用 peak を作る。
# preview の再生経路 (mvm_audio_preview) とは独立させ、Qt にも依存しない。
add_library(mvm_audio_waveform STATIC
    media/audio_waveform/audio_waveform_decoder.cpp
)
target_include_directories(mvm_audio_waveform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_audio_waveform
    PUBLIC mvm::core
    PRIVATE PkgConfig::MVM_FFMPEG mvm::warnings
)
add_library(mvm::audio_waveform ALIAS mvm_audio_waveform)

# ラウドネス解析。PCM の測定のみを担当し、Qt と MLT に依存しない。
add_library(mvm_audio_analysis STATIC media/audio_analysis/loudness_meter.cpp)
target_include_directories(mvm_audio_analysis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_audio_analysis PRIVATE PkgConfig::MVM_FFMPEG PkgConfig::MVM_AVFILTER mvm::warnings)
add_library(mvm::audio_analysis ALIAS mvm_audio_analysis)

# --- 画像素材 -----------------------------------------------------------------
# 素材の stream 構成の判定と、静止画の decode。preview と書き出しの両方がここを通り、
# 同じ画素を使う。Qt にも MLT にも依存しない (lint が Qt の include を検査する)。
add_library(mvm_still_image STATIC
    media/still_image/ffmpeg_input.cpp
    media/still_image/media_stream_facts.cpp
    media/still_image/still_image_decoder.cpp
    media/still_image/static_image.cpp
)
target_include_directories(mvm_still_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_still_image
    PRIVATE PkgConfig::MVM_FFMPEG PkgConfig::MVM_SWSCALE mvm::warnings
)
add_library(mvm::still_image ALIAS mvm_still_image)

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
        app/preview/native_present_hook.cpp
        app/preview/presentation_eligibility_preflight.cpp
        app/preview/preview_rhi_item.cpp
        app/preview/compositor_rhi_item.cpp
        app/preview/preview_engine_rhi_item.cpp
        app/preview/test_window_mode.cpp
    )
    set_target_properties(mvm_preview_qt PROPERTIES AUTOMOC ON)
    target_include_directories(mvm_preview_qt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mvm_preview_qt
        PUBLIC  mvm::gpu_preview mvm::audio_preview mvm::preview_engine Qt6::Quick Qt6::Gui
        PRIVATE Qt6::GuiPrivate mvm::warnings dwmapi
    )
    # QML module 側 (apps/mvm の PreviewSurface) がこの層の型を基底に持つため、
    # metatypes を出して qmltyperegistrar / qmllint から基底型が見えるようにする。
    qt_extract_metatypes(mvm_preview_qt)
    add_library(mvm::preview_qt ALIAS mvm_preview_qt)
endif()

# ローカル文字起こし。公開ヘッダにFFmpeg・whisper・Qtの型を出さない。
add_library(mvm_transcribe STATIC media/transcribe/transcribe.cpp media/transcribe/audio_prepare.cpp)
target_include_directories(mvm_transcribe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mvm_transcribe PRIVATE PkgConfig::MVM_FFMPEG whisper mvm::warnings)
add_library(mvm::transcribe ALIAS mvm_transcribe)

target_compile_definitions(mvm_transcribe PRIVATE MVM_WHISPER_BIN_DIR="${WHISPER_BIN_DIR}")
# 指定したWhisperとggmlのDLLをexeと同じ場所へ置き、PATH上の別構成を拾わせない。
file(GLOB MVM_WHISPER_RUNTIME_DLLS "${WHISPER_BIN_DIR}/*whisper*.dll" "${WHISPER_BIN_DIR}/ggml*.dll")
if(NOT MVM_WHISPER_RUNTIME_DLLS)
    message(FATAL_ERROR "指定したWhisperの実行時DLLがありません")
endif()
add_custom_target(mvm_whisper_runtime
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MVM_WHISPER_RUNTIME_DLLS} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    VERBATIM)
add_dependencies(mvm_transcribe mvm_whisper_runtime)
