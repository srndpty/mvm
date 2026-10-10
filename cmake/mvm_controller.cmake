# 製品と controller 試験で同じ実装と依存を使う。
set(MVM_CONTROLLER_SOURCES
    "${CMAKE_SOURCE_DIR}/apps/mvm/image_raster_cache.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/media_bin_model.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/media_import.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/subtitle_controller.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/mvm_controller.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/mvm_controller_detail.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/mvm_controller_export.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/mvm_controller_effects.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/mvm_controller_media.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/mvm_controller_project_io.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/mvm_controller_timeline_edit.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/equation_sequence_editor.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/shuttle_audio_mix.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/shuttle_audio_playback.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/clip_sample_reader.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/scrub_audio_grain.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/scrub_audio_playback.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/timeline_clip_model.cpp"
    "${CMAKE_SOURCE_DIR}/apps/mvm/track_model.cpp"
)
set(MVM_CONTROLLER_LIBRARIES
    mvm::audio_adjustment mvm::core mvm::project mvm::util
    mvm::transcribe mvm::manim_clip_workflow mvm::math_raster_cache mvm::math_clip_render
    mvm::equation_sequence_compile mvm::timeline_playback mvm::timeline_preview_mapping
    mvm::timeline_export mvm::mlt mvm::still_image mvm::preview_engine mvm::audio_preview
    mvm::preview_qt Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 mvm::warnings
)
