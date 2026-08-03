# Phase 0 の検証ハーネスとテスト。
# 性能最適化はここでは行わない (S7 以降)。

find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

# --- mvm_bench --------------------------------------------------------------
# Qt に依存しない C++20 CLI。MLT のヘッダは include しない。
add_executable(mvm_bench harness/mvm_bench.cpp)

target_link_libraries(mvm_bench PRIVATE
    mvm::mlt
    mvm::util
    PkgConfig::PNG
    mvm::warnings
)

# ffprobe は必ず UCRT64 版を使う。PATH 上の別 FFmpeg
# (C:\tools や winget 版) へフォールバックしない。
target_compile_definitions(mvm_bench PRIVATE
    ${MVM_MLT_PATH_DEFS}
    MVM_FFPROBE_EXE="${MVM_UCRT64_ROOT}/bin/ffprobe.exe"
)

# --- テスト -----------------------------------------------------------------

set(MVM_SMOKE_DIR "${CMAKE_SOURCE_DIR}/tests/assets/smoke")
set(MVM_SMOKE_MANIFEST "${MVM_SMOKE_DIR}/manifest.json")
set(MVM_JP_DIR "${MVM_SMOKE_DIR}/素材/日本語 テスト/第1回　微分積分＆演習")

# 素材が無いときに「原因不明の失敗」にせず、実行すべきコマンドを案内する。
set(MVM_MAKE_MEDIA_HINT "pwsh scripts/make-testmedia.ps1 -Mode Smoke")

function(mvm_add_test name)
    cmake_parse_arguments(T "" "" "COMMAND;PROPERTIES" ${ARGN})
    add_test(NAME ${name} COMMAND ${T_COMMAND})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
        ${T_PROPERTIES}
    )
endfunction()

# --- doctor -----------------------------------------------------------------

mvm_add_test(doctor COMMAND mvm_bench doctor)

# negative: 存在しない module dir を成功扱いしない
mvm_add_test(doctor_missing_module_dir
    COMMAND mvm_bench doctor --module-dir "${CMAKE_CURRENT_BINARY_DIR}/no-such-module-dir"
    PROPERTIES WILL_FAIL TRUE)

# negative: 空の module dir (MLT が最も静かに縮退するケース) を成功扱いしない
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/empty-modules")
mvm_add_test(doctor_empty_module_dir
    COMMAND mvm_bench doctor --module-dir "${CMAKE_CURRENT_BINARY_DIR}/empty-modules"
    PROPERTIES WILL_FAIL TRUE)

# negative: data dir が空だと profile が既定値へ黙って落ちる。これを検出する。
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/empty-data")
mvm_add_test(doctor_empty_data_dir
    COMMAND mvm_bench doctor --data-dir "${CMAKE_CURRENT_BINARY_DIR}/empty-data"
    PROPERTIES WILL_FAIL TRUE)

# negative: 存在しない profile 名を成功扱いしない
mvm_add_test(doctor_missing_profile
    COMMAND mvm_bench doctor --profile mvm_no_such_profile
    PROPERTIES WILL_FAIL TRUE)

# negative: 期待値が誤っていれば失敗する (profile 値の照合が効いていること)
mvm_add_test(doctor_wrong_profile_expectation
    COMMAND mvm_bench doctor --expect-width 1280 --expect-height 720
    PROPERTIES WILL_FAIL TRUE)

# --- 素材の検証 -------------------------------------------------------------

mvm_add_test(verify_media_smoke
    COMMAND mvm_bench verify-media "${MVM_SMOKE_MANIFEST}"
    PROPERTIES
        REQUIRED_FILES "${MVM_SMOKE_MANIFEST}"
        FAIL_REGULAR_EXPRESSION "FAIL"
)

# --- probe (V2) -------------------------------------------------------------

mvm_add_test(probe_h264
    COMMAND mvm_bench probe "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4")

mvm_add_test(probe_hevc
    COMMAND mvm_bench probe "${MVM_SMOKE_DIR}/v1080p60_hevc.mp4"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_hevc.mp4")

mvm_add_test(probe_png_alpha
    COMMAND mvm_bench probe "${MVM_SMOKE_DIR}/png_alpha.png"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/png_alpha.png")

mvm_add_test(probe_wav
    COMMAND mvm_bench probe "${MVM_SMOKE_DIR}/wav_48k.wav"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/wav_48k.wav")

mvm_add_test(probe_4k_h264
    COMMAND mvm_bench probe "${MVM_SMOKE_DIR}/v4k60_h264.mp4"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v4k60_h264.mp4")

mvm_add_test(probe_4k_hevc10
    COMMAND mvm_bench probe "${MVM_SMOKE_DIR}/v4k60_hevc10.mp4"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v4k60_hevc10.mp4")

# --- 日本語パス (V10) -------------------------------------------------------

mvm_add_test(probe_jp_path_video
    COMMAND mvm_bench probe "${MVM_JP_DIR}/講義映像　第1回＆演習.mp4"
    PROPERTIES REQUIRED_FILES "${MVM_JP_DIR}/講義映像　第1回＆演習.mp4")

mvm_add_test(probe_jp_path_png
    COMMAND mvm_bench probe "${MVM_JP_DIR}/数式オーバーレイ＆図.png"
    PROPERTIES REQUIRED_FILES "${MVM_JP_DIR}/数式オーバーレイ＆図.png")

mvm_add_test(probe_jp_path_wav
    COMMAND mvm_bench probe "${MVM_JP_DIR}/ナレーション　音声.wav"
    PROPERTIES REQUIRED_FILES "${MVM_JP_DIR}/ナレーション　音声.wav")

# --- decode + フレーム固有マーカー ------------------------------------------
# 要求フレームと、素材に焼き込まれたマーカーが一致することを確認する。
# OCR は使わない (docs/research/test-media-format.md)。

foreach(frame 0 1 137 299)
    mvm_add_test(decode_marker_h264_${frame}
        COMMAND mvm_bench decode "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
                --frame ${frame} --expect-marker
                --output "${CMAKE_CURRENT_BINARY_DIR}/decode_h264_${frame}.png"
        PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4")
endforeach()

mvm_add_test(decode_marker_hevc
    COMMAND mvm_bench decode "${MVM_SMOKE_DIR}/v1080p60_hevc.mp4"
            --frame 137 --expect-marker
            --output "${CMAKE_CURRENT_BINARY_DIR}/decode_hevc_137.png"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_hevc.mp4")

mvm_add_test(decode_marker_jp_path
    COMMAND mvm_bench decode "${MVM_JP_DIR}/講義映像　第1回＆演習.mp4"
            --frame 200 --expect-marker
            --output "${CMAKE_CURRENT_BINARY_DIR}/decode_jp_200.png"
    PROPERTIES REQUIRED_FILES "${MVM_JP_DIR}/講義映像　第1回＆演習.mp4")

# negative: 範囲外フレームを成功扱いしない
mvm_add_test(decode_out_of_range
    COMMAND mvm_bench decode "${MVM_SMOKE_DIR}/v1080p60_h264.mp4" --frame 999999
    PROPERTIES
        REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
        WILL_FAIL TRUE)

# negative: 存在しない素材を成功扱いしない
mvm_add_test(probe_missing_file
    COMMAND mvm_bench probe "${CMAKE_CURRENT_BINARY_DIR}/no-such-media.mp4"
    PROPERTIES WILL_FAIL TRUE)
