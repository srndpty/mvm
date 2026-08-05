# Phase 0 の検証ハーネスとテスト。
# 性能最適化はここでは行わない (S7 以降)。

find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

# --- mvm_bench --------------------------------------------------------------
# Qt に依存しない C++20 CLI。MLT のヘッダは include しない。
add_executable(mvm_bench
    harness/mvm_bench.cpp
    harness/mvm_bench_compose.cpp
    harness/mvm_bench_proxy.cpp
)

# ScrubCoalescer の純粋単体テスト。MLT にも Qt にも依存しない。
add_executable(mvm_test_scrub_coalescer harness/test_scrub_coalescer.cpp)
target_include_directories(mvm_test_scrub_coalescer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/harness)
target_link_libraries(mvm_test_scrub_coalescer PRIVATE mvm::warnings)
add_test(NAME scrub_coalescer_unit COMMAND mvm_test_scrub_coalescer)

# 同じ契約を実際の mutex / condition_variable の下で検査する。
# sleep には依存しない (tests/harness/test_scrub_threaded.cpp 冒頭を参照)。
add_executable(mvm_test_scrub_threaded harness/test_scrub_threaded.cpp)
target_include_directories(mvm_test_scrub_threaded PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/harness)
target_link_libraries(mvm_test_scrub_threaded PRIVATE mvm::warnings)
add_test(NAME scrub_coalescer_threaded COMMAND mvm_test_scrub_threaded)

# proxy path resolver。preview と final で解決先が違うことを検査する。
add_executable(mvm_test_proxy_resolver harness/test_proxy_resolver.cpp)
target_include_directories(mvm_test_proxy_resolver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/harness)
target_link_libraries(mvm_test_proxy_resolver PRIVATE mvm::warnings)
add_test(NAME proxy_resolver_unit COMMAND mvm_test_proxy_resolver)
set_tests_properties(scrub_coalescer_threaded PROPERTIES TIMEOUT 120)

target_link_libraries(mvm_bench PRIVATE
    psapi
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

# --- negative test は終了コードを厳密に照合する -----------------------------
#
# CTest の WILL_FAIL は使わない。**「0 以外なら合格」でしかない。**
# 使い方エラー (2) を期待しているつもりでも、
# ヒープ破壊 (0xC0000374)、DLL ロード失敗、素材の取り違えでも緑になる。
# 「失敗したこと」ではなく「意図した理由で失敗したこと」を検査する。
#
# このリポジトリに WILL_FAIL は 1 件も無い。増やさないこと。
find_program(MVM_PWSH pwsh)
if(NOT MVM_PWSH)
    message(FATAL_ERROR
        "pwsh が見つかりません。negative test の終了コード照合に必要です。")
endif()

# ARGS に ';' を含む引数 (--map や --require-proxy-ids) を渡す場合は
# **'@SEMI@' と書く。** ここで ';' へ戻す。
# CMake の引数はリストなので、'\;' でエスケープしても関数へ渡す時点で
# 実体の ';' に戻り、そこで分割されてしまう。
# 実際に --map の 2 件目が落ち、「2 件必須」のテストが 1 件で通っていた。
function(mvm_add_exit_test name)
    cmake_parse_arguments(T "" "EXIT" "ARGS;PROPERTIES" ${ARGN})
    if(NOT DEFINED T_EXIT)
        message(FATAL_ERROR "mvm_add_exit_test(${name}) に EXIT がありません")
    endif()
    list(JOIN T_ARGS "|" _child_args)
    string(REPLACE "@SEMI@" ";" _child_args "${_child_args}")
    add_test(NAME ${name}
        COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/expect-exit.ps1"
                -ExpectExit ${T_EXIT} -Exe "$<TARGET_FILE:mvm_bench>"
                -ChildArgs "${_child_args}")
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
        ${T_PROPERTIES}
    )
endfunction()

# --- doctor -----------------------------------------------------------------

mvm_add_test(doctor COMMAND mvm_bench doctor)

# negative: 存在しない module dir を成功扱いしない。
# doctor の検査失敗は exit 3 (mismatch)。DLL ロード失敗 (1) と区別する。
mvm_add_exit_test(doctor_missing_module_dir EXIT 3
    ARGS doctor --module-dir "${CMAKE_CURRENT_BINARY_DIR}/no-such-module-dir")

# negative: 空の module dir (MLT が最も静かに縮退するケース) を成功扱いしない
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/empty-modules")
mvm_add_exit_test(doctor_empty_module_dir EXIT 3
    ARGS doctor --module-dir "${CMAKE_CURRENT_BINARY_DIR}/empty-modules")

# negative: data dir が空だと profile が既定値へ黙って落ちる。これを検出する。
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/empty-data")
mvm_add_exit_test(doctor_empty_data_dir EXIT 3
    ARGS doctor --data-dir "${CMAKE_CURRENT_BINARY_DIR}/empty-data")

# negative: 存在しない profile 名を成功扱いしない
mvm_add_exit_test(doctor_missing_profile EXIT 3
    ARGS doctor --profile mvm_no_such_profile)

# negative: 期待値が誤っていれば失敗する (profile 値の照合が効いていること)
mvm_add_exit_test(doctor_wrong_profile_expectation EXIT 3
    ARGS doctor --expect-width 1280 --expect-height 720)

# negative: SAR の期待値が誤っていれば失敗する。
# SAR を取り違えると V12 で「なぜか横に伸びる」形で表面化するため、
# doctor の段階で照合しておく。
mvm_add_exit_test(doctor_wrong_sar_expectation EXIT 3
    ARGS doctor --expect-sar-num 4 --expect-sar-den 3)

# SAR は gcd で正規化して比較する。2/2 は 1/1 と等価なので通るべき。
mvm_add_test(doctor_sar_normalized
    COMMAND mvm_bench doctor --expect-sar-num 2 --expect-sar-den 2)

# --- 素材の検証 -------------------------------------------------------------

mvm_add_test(verify_media_smoke
    COMMAND mvm_bench verify-media "${MVM_SMOKE_MANIFEST}"
    PROPERTIES
        REQUIRED_FILES "${MVM_SMOKE_MANIFEST}"
        FAIL_REGULAR_EXPRESSION "FAIL"
)

# --- manifest の negative test ---------------------------------------------
# 壊した manifest が失敗することを確認する。通ってしまうと verify-media が
# 「検証したつもり」で成功していることになり、V2 の判定根拠が無意味になる。
# 詳細は tests/fixtures/README.md。

set(MVM_FIXTURES "${CMAKE_SOURCE_DIR}/tests/fixtures")
set(MVM_S5_SCENARIO "${CMAKE_SOURCE_DIR}/bench/scenarios/s5-five-track.json")

# 対照群: 正しい最小 manifest は通る。
# これが落ちるなら、下の negative test は意図した箇所で落ちていない。
mvm_add_test(verify_media_fixture_good
    COMMAND mvm_bench verify-media "${MVM_FIXTURES}/good-minimal.json"
    PROPERTIES
        REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
        FAIL_REGULAR_EXPRESSION "FAIL")

# 期待する終了コードまで書く。manifest 自体が読めない (1) のか、
# 読めたうえで期待値と食い違った (3) のかは意味が違う。
# 「どちらでもよい」にすると、fixture を取り違えても緑になる。
set(MVM_BAD_MANIFESTS
    missing-schema      1   # schema_version が無い          -> manifest エラー
    schema-version      1   # schema_version が対応外        -> manifest エラー
    empty-assets        1   # assets が空                    -> manifest エラー
    unknown-kind        3   # kind が未知
    duplicate-id        3   # id が重複
    missing-expected    3   # expected が無い
    missing-field       3   # expected に必須フィールドが無い
    wrong-pix-fmt       3   # pix_fmt を改変
    wrong-sar           3   # SAR を改変
    wrong-duration      3   # duration を改変
    wrong-alpha         3   # alpha の期待範囲を改変
)
list(LENGTH MVM_BAD_MANIFESTS _bad_len)
math(EXPR _bad_last "${_bad_len} / 2 - 1")
foreach(i RANGE ${_bad_last})
    math(EXPR _bn "${i} * 2")
    math(EXPR _bc "${i} * 2 + 1")
    list(GET MVM_BAD_MANIFESTS ${_bn} bad)
    list(GET MVM_BAD_MANIFESTS ${_bc} bad_exit)
    string(REPLACE "-" "_" bad_name "${bad}")
    mvm_add_exit_test(verify_media_bad_${bad_name} EXIT ${bad_exit}
        ARGS verify-media "${MVM_FIXTURES}/bad-${bad}.json"
        PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4")
endforeach()

# --- 破損・退化素材の negative test -----------------------------------------
# これらが成功扱いされないことを確認する。
#
# subtitle_only は破損していない。コンテナは正当だが映像も音声も無い、
# 「読めるが使えない」入力であり、壊れたバイト列とは別の経路を通る。

set(MVM_CORRUPT_DIR "${MVM_SMOKE_DIR}/_corrupt")

# ファイルは存在する (ので「開けない」= 1 ではない)。
# 開いた結果が使い物にならないことを検出する exit 3 を期待する。
foreach(corrupt zero random truncated text subtitle_only)
    mvm_add_exit_test(probe_corrupt_${corrupt} EXIT 3
        ARGS probe "${MVM_CORRUPT_DIR}/${corrupt}.mp4"
        PROPERTIES REQUIRED_FILES "${MVM_CORRUPT_DIR}/${corrupt}.mp4")

    mvm_add_exit_test(decode_corrupt_${corrupt} EXIT 3
        ARGS decode "${MVM_CORRUPT_DIR}/${corrupt}.mp4" --frame 0
        PROPERTIES REQUIRED_FILES "${MVM_CORRUPT_DIR}/${corrupt}.mp4")
endforeach()

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

# 4K HEVC 10bit。10bit -> RGBA の変換を経てもマーカーが読めることを確認する。
# probe だけでは「ピクセルが正しく出てくるか」は分からない。
mvm_add_test(decode_marker_4k_hevc10
    COMMAND mvm_bench decode "${MVM_SMOKE_DIR}/v4k60_hevc10.mp4"
            --frame 137 --expect-marker
            --output "${CMAKE_CURRENT_BINARY_DIR}/decode_4k_hevc10_137.png"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v4k60_hevc10.mp4")

mvm_add_test(decode_marker_4k_h264
    COMMAND mvm_bench decode "${MVM_SMOKE_DIR}/v4k60_h264.mp4"
            --frame 42 --expect-marker
            --output "${CMAKE_CURRENT_BINARY_DIR}/decode_4k_h264_42.png"
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v4k60_h264.mp4")

mvm_add_test(decode_marker_jp_path
    COMMAND mvm_bench decode "${MVM_JP_DIR}/講義映像　第1回＆演習.mp4"
            --frame 200 --expect-marker
            --output "${CMAKE_CURRENT_BINARY_DIR}/decode_jp_200.png"
    PROPERTIES REQUIRED_FILES "${MVM_JP_DIR}/講義映像　第1回＆演習.mp4")

# negative: 範囲外フレームを成功扱いしない。
# ファイルは開けるので 3 (取得結果が期待と違う)。
mvm_add_exit_test(decode_out_of_range EXIT 3
    ARGS decode "${MVM_SMOKE_DIR}/v1080p60_h264.mp4" --frame 999999
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4")

# negative: 存在しない素材を成功扱いしない。
# こちらは開けないので 1。上の 3 と区別できることが要点である。
mvm_add_exit_test(probe_missing_file EXIT 1
    ARGS probe "${CMAKE_CURRENT_BINARY_DIR}/no-such-media.mp4")

# --- S5 音声 (M3) -----------------------------------------------------------
# 音声の正式なゲートは render-audio + verify-audio である。
# mvm_mlt_compose_audio (生バッファ) は fail-closed のままで、
# verify-compose の音声検査には使わない。

set(MVM_DIAG_DIR "${MVM_SMOKE_DIR}/_diag")
set(MVM_AUDIO_OUT "${CMAKE_CURRENT_BINARY_DIR}/audio")
file(MAKE_DIRECTORY "${MVM_AUDIO_OUT}")

# 最小グラフの切り分けケース。
# producer service に loader を使わないと AAC が壊れ volume が落ちることが
# 実測で分かっている (docs/research/composition-notes.md)。
# これらはその回帰テストである。
set(MVM_AG_CASES
    audio_direct_aac        A   # producer -> consumer のみ
    audio_playlist_aac      B   # + playlist + tractor
    audio_stream_selectors  C   # + video_index=-1
    audio_separate_resources D  # 別 resource
    audio_same_resource     E   # 同一 resource を別 producer で stream 分離
    audio_mix_sum           F   # mix(sum=1)
    audio_volume_minus_6db  G   # + volume -6dB
    audio_full_five_track   H   # 5 トラック相当
)
list(LENGTH MVM_AG_CASES _ag_len)
math(EXPR _ag_last "${_ag_len} / 2 - 1")
foreach(i RANGE ${_ag_last})
    math(EXPR _n "${i} * 2")
    math(EXPR _c "${i} * 2 + 1")
    list(GET MVM_AG_CASES ${_n} _name)
    list(GET MVM_AG_CASES ${_c} _case)
    mvm_add_test(${_name}
        COMMAND mvm_bench audio-graph-probe --case ${_case}
                --output "${MVM_AUDIO_OUT}/${_case}.wav"
                --diag-dir "${MVM_DIAG_DIR}" --smoke-dir "${MVM_SMOKE_DIR}"
                --timeout-ms 60000
        PROPERTIES REQUIRED_FILES "${MVM_DIAG_DIR}/a1_av.mp4")
endforeach()

# negative: 書き込めない出力先を成功扱いしない
mvm_add_exit_test(audio_invalid_output_negative EXIT 3
    ARGS audio-graph-probe --case A
         --output "${CMAKE_CURRENT_BINARY_DIR}/no-such-dir/x/y.wav"
         --diag-dir "${MVM_DIAG_DIR}" --smoke-dir "${MVM_SMOKE_DIR}"
    PROPERTIES REQUIRED_FILES "${MVM_DIAG_DIR}/a1_av.mp4")

# negative: timeout を成功扱いしない (1ms では絶対に終わらない)。
# timeout は専用の exit 4。書き込み失敗 (3) と取り違えないこと。
mvm_add_exit_test(audio_timeout_negative EXIT 4
    ARGS audio-graph-probe --case H
         --output "${MVM_AUDIO_OUT}/timeout.wav"
         --diag-dir "${MVM_DIAG_DIR}" --smoke-dir "${MVM_SMOKE_DIR}"
         --timeout-ms 1
    PROPERTIES REQUIRED_FILES "${MVM_DIAG_DIR}/a1_av.mp4")

# --- M3 の正式ゲート: 5 トラック構成の音声を実ファイルで検証 ----------------
# render で 4 種類を出力し、verify で 4 周波数 / gain / clipping を検査する。

mvm_add_test(render_audio_a1_only
    COMMAND mvm_bench render-audio "${MVM_S5_SCENARIO}"
            --output "${MVM_AUDIO_OUT}/A1-only.wav" --disable-track A2
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" FIXTURES_SETUP mvm_audio)

mvm_add_test(render_audio_a2_only
    COMMAND mvm_bench render-audio "${MVM_S5_SCENARIO}"
            --output "${MVM_AUDIO_OUT}/A2-only.wav" --disable-track A1
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" FIXTURES_SETUP mvm_audio)

mvm_add_test(render_audio_a2_only_0db
    COMMAND mvm_bench render-audio "${MVM_S5_SCENARIO}"
            --output "${MVM_AUDIO_OUT}/A2-only-0db.wav" --disable-track A1 --gain-db 0
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" FIXTURES_SETUP mvm_audio)

mvm_add_test(render_audio_mixed
    COMMAND mvm_bench render-audio "${MVM_S5_SCENARIO}" --output "${MVM_AUDIO_OUT}/mixed.wav"
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" FIXTURES_SETUP mvm_audio)

mvm_add_test(verify_audio_four_frequencies
    COMMAND mvm_bench verify-audio "${MVM_AUDIO_OUT}" --check-mix
    PROPERTIES FIXTURES_REQUIRED mvm_audio)

mvm_add_test(verify_audio_gain_minus_6db
    COMMAND mvm_bench verify-audio "${MVM_AUDIO_OUT}" --check-gain
    PROPERTIES FIXTURES_REQUIRED mvm_audio)

mvm_add_test(verify_audio_no_clipping
    COMMAND mvm_bench verify-audio "${MVM_AUDIO_OUT}" --check-clipping
    PROPERTIES FIXTURES_REQUIRED mvm_audio)

# --- 参照所有権の反復テスト -------------------------------------------------
# MLT の参照所有権を間違えると 1 回では露見せず、回数を重ねて
# handle 数や RSS の線形増加として現れる。
# 通常 CTest では 100 回、時間がかかるので performance ラベルは付けない
# (所有権は正しさの問題であり性能ではない)。

# ownership は「正しさ」だけを見る。
# Working Set の増減だけを理由に ownership 失敗としない。
# メモリは memory_soak_diagnostic (stability ラベル) へ分離した。
mvm_add_test(ownership_soak_100
    COMMAND mvm_bench soak "${MVM_S5_SCENARIO}" --iterations 100
            --wav-dir "${MVM_AUDIO_OUT}" --no-memory-check
    PROPERTIES
        REQUIRED_FILES "${MVM_S5_SCENARIO}"
        TIMEOUT 1800)

# メモリは長時間かかるうえ、まだ合否を決められる段階にない。
# 通常 CTest から外し、stability ラベルで明示的に実行する。
mvm_add_test(memory_soak_diagnostic
    COMMAND mvm_bench memory-probe "${MVM_S5_SCENARIO}" --case C
            --csv "${CMAKE_CURRENT_BINARY_DIR}/memory_case_c.csv"
            --wav-dir "${MVM_AUDIO_OUT}"
    PROPERTIES
        REQUIRED_FILES "${MVM_S5_SCENARIO}"
        LABELS stability
        TIMEOUT 1800)

# --- scrub 表示契約の回帰テスト (M6) ----------------------------------------
# 閾値ではなく契約を見る。counter invariant・表示 generation の単調性・
# 最終要求の表示・marker 一致・集計の自己整合が破れたら失敗する。
# 性能の判定は scripts/scrub-matrix.ps1 で行う (8 条件 x 3 回)。
foreach(iv 8333 16667)
    mvm_add_test(scrub_contract_linear_${iv}
        COMMAND mvm_bench scrub-bench "${MVM_S5_SCENARIO}"
                --requests 60 --pattern linear --submit-interval-us ${iv}
        PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" TIMEOUT 300)
endforeach()

# coalescing が実際に働いていることを確認する。
# superseded_pending が 0 なら、latest-only の置換が一度も起きていない。
mvm_add_test(scrub_coalescing_occurs
    COMMAND mvm_bench scrub-bench "${MVM_S5_SCENARIO}"
            --requests 60 --pattern linear --submit-interval-us 0
            --check --require-coalescing --min-updates-per-sec 0 --max-p95-ms 100000
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" TIMEOUT 300)

# negative: 音声を含むケースで --wav-dir が無いとき、
# 音声を黙って飛ばした測定を成功扱いしない。
#
# 旧 memory_probe_${mcase}_requires_wav_dir (WILL_FAIL 版) は削除した。
# 下の memory_probe_${mcase}_exit_code_is_usage と同じことを、
# より弱い条件 (0 以外なら何でも合格) で見ていただけの重複である。

# --- S7: preview / proxy -----------------------------------------------------

set(MVM_S7_SCENARIO "${CMAKE_SOURCE_DIR}/bench/scenarios/s7-five-track-60s.json")
set(MVM_BENCH_DIR   "${CMAKE_SOURCE_DIR}/tests/assets/benchmark")
set(MVM_PROXY_DIR   "${MVM_BENCH_DIR}/_proxy")
set(MVM_MAKE_BENCH_HINT "pwsh scripts/make-testmedia.ps1 -Mode Benchmark")

# 60 秒 scenario の schema と代表フレームの合成。
# 映像だけを見る。音声の正式ゲートは render-audio + verify-audio である。
mvm_add_test(preview_scenario_schema
    COMMAND mvm_bench compose "${MVM_S7_SCENARIO}" --frame 3599 --expect-marker
    PROPERTIES
        REQUIRED_FILES "${MVM_S7_SCENARIO}"
        TIMEOUT 300)

# preview は短時間で契約だけ見る。性能判定は performance ラベル側。
mvm_add_test(preview_contract_short
    COMMAND mvm_bench preview-bench "${MVM_S5_SCENARIO}"
            --consumer null --real-time -4 --measure-ms 3000 --warmup-ms 0
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}" TIMEOUT 300)

# negative: real_time=0 は mlt_frame_get_image を呼ばないので計測に使えない。
# 走らせてから「rendered が 0 件」で落とす (3) のではなく、
# 走らせる前に使い方エラー (2) で止める。
# 3 のままでは「使い方が誤っている」のか「実装が壊れている」のか区別できない。
mvm_add_exit_test(preview_invalid_real_time_exit2 EXIT 2
    ARGS preview-bench "${MVM_S5_SCENARIO}" --real-time 0 --measure-ms 1000
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}")

# negative: 存在しない consumer service を成功扱いしない。
mvm_add_exit_test(preview_missing_consumer_service EXIT 3
    ARGS preview-bench "${MVM_S5_SCENARIO}" --consumer mvm_no_such_consumer
         --real-time -1 --measure-ms 1000
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}")

# negative: 音声ケースの --wav-dir 欠落は「使い方エラー (2)」であること。
foreach(mcase D E)
    mvm_add_exit_test(memory_probe_${mcase}_exit_code_is_usage EXIT 2
        ARGS memory-probe "${MVM_S5_SCENARIO}" --case ${mcase} --iterations 1
        PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}")
endforeach()

# --- required proxy id 契約 (S7.1) ------------------------------------------
# 「全 source を proxy 必須」にはできない (WAV や音声専用 source が混ざる)。
# required と optional を分け、required が 1 件でも未解決なら exit 4 で落とす。
#
# **negative test には実在する proxy を使う。**
# 以前は `_proxy/a.mp4` のような架空のパスを使っていた。
# 架空のパスでは「required が解決できた」ことにならず、
# 文字列置換が成功しただけの状態を「合格」として見てしまう。
# resolver が proxy の実体を検査するようになった今、
# 架空のパスは常に exit 4 になり、正常系の証明にもならない。

set(MVM_S7_4K     "${CMAKE_SOURCE_DIR}/bench/scenarios/s7-4k-original.json")
set(MVM_PX_V1     "_proxy/v4k60_h264_proxy_gop12.mp4")
set(MVM_PX_V2     "_proxy/v1080p60_hevc_proxy_gop12.mp4")
set(MVM_PX_V1_ABS "${MVM_PROXY_DIR}/v4k60_h264_proxy_gop12.mp4")
set(MVM_PX_V2_ABS "${MVM_PROXY_DIR}/v1080p60_hevc_proxy_gop12.mp4")
# ';' は CMake のリスト区切りなので、1 引数として渡すためエスケープする。
set(MVM_REQ2 "v4k60_h264.mp4@SEMI@v1080p60_hevc.mp4")
set(MVM_MAP2 "v4k60_h264.mp4=${MVM_PX_V1}@SEMI@v1080p60_hevc.mp4=${MVM_PX_V2}")

# proxy が無い環境ではこの契約は検査できない。素材生成を促して skip する。
if(EXISTS "${MVM_PX_V1_ABS}" AND EXISTS "${MVM_PX_V2_ABS}")

    # 正常系: 実在する proxy 2 件が両方置換される。
    mvm_add_exit_test(proxy_required_two_real_proxies_resolved EXIT 0
        ARGS resolve-proxy "${MVM_S7_4K}" --target preview
             --map "${MVM_MAP2}" --require-proxy-ids "${MVM_REQ2}"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # map にはあるが proxy の実体が無い -> exit 4。
    # 文字列置換が成功しただけを「解決できた」と読まないことの検査である。
    mvm_add_exit_test(proxy_required_missing_proxy_file_fails EXIT 4
        ARGS resolve-proxy "${MVM_S7_4K}" --target preview
             --map "v4k60_h264.mp4=${MVM_PX_V1}@SEMI@v1080p60_hevc.mp4=_proxy/mvm_no_such_proxy.mp4"
             --require-proxy-ids "${MVM_REQ2}"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # required の片方だけ map にある -> exit 4
    mvm_add_exit_test(proxy_required_partial_map_fails EXIT 4
        ARGS resolve-proxy "${MVM_S7_4K}" --target preview
             --map "v4k60_h264.mp4=${MVM_PX_V1}"
             --require-proxy-ids "${MVM_REQ2}"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # scenario に存在しない required id -> exit 4。
    # proxy 自体は実在するものを指す。落ちる理由を 1 つに絞るためである。
    mvm_add_exit_test(proxy_required_absent_in_scenario_fails EXIT 4
        ARGS resolve-proxy "${MVM_S7_4K}" --target preview
             --map "mvm_no_such_source.mp4=${MVM_PX_V1}"
             --require-proxy-ids "mvm_no_such_source.mp4"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # 同じ required id を二度指定するのは使い方の誤り -> exit 2。
    # 黙って 1 件として扱うと「2 件必須のつもりが 1 件」に気づけない。
    mvm_add_exit_test(proxy_required_duplicate_id_is_usage_error EXIT 2
        ARGS resolve-proxy "${MVM_S7_4K}" --target preview
             --map "${MVM_MAP2}"
             --require-proxy-ids "v4k60_h264.mp4@SEMI@v4k60_h264.mp4"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # final では required も original のまま。proxy になったら exit 3。
    mvm_add_exit_test(proxy_required_final_keeps_original EXIT 0
        ARGS resolve-proxy "${MVM_S7_4K}" --target final
             --map "${MVM_MAP2}" --require-proxy-ids "${MVM_REQ2}"
        PROPERTIES REQUIRED_FILES "${MVM_S7_4K}")

    # out ファイルの契約。終了コードだけでは見えない部分を検査する。
    #   - 失敗時に out を新規作成しない
    #   - 失敗時に既存の out を変更しない
    #   - 一時ファイル (.mvmtmp) を残さない
    #   - final の出力に proxy パスが 1 件も無い
    #   - optional (音声専用 source / WAV) は original のまま
    add_test(NAME proxy_resolver_out_file_contract
        COMMAND "${MVM_PWSH}" -NoProfile -File
                "${CMAKE_SOURCE_DIR}/scripts/check-resolver-contract.ps1"
                -Bench "$<TARGET_FILE:mvm_bench>"
                -Scenario "${MVM_S7_4K}"
                -WorkDir "${CMAKE_CURRENT_BINARY_DIR}/resolver-contract")
    set_tests_properties(proxy_resolver_out_file_contract PROPERTIES
        ENVIRONMENT "PATH=${MVM_UCRT64_ROOT}/bin;$ENV{PATH}"
        REQUIRED_FILES "${MVM_S7_4K}"
        TIMEOUT 300)
else()
    message(STATUS
        "proxy が無いため required proxy id 契約のテストを登録しません。"
        " 先に ${MVM_MAKE_BENCH_HINT} と pwsh scripts/proxy-matrix.ps1 を実行してください。")
endif()

# 旧 --require-proxy は diagnostic 専用へ降格した。
# 「0 件なら失敗」であることは維持するが、判定には使わない。
mvm_add_exit_test(proxy_legacy_require_proxy_diagnostic EXIT 3
    ARGS resolve-proxy "${MVM_S5_SCENARIO}" --target preview
         --map "mvm_no_such_source.mp4=_proxy/x.mp4" --require-proxy
    PROPERTIES REQUIRED_FILES "${MVM_S5_SCENARIO}")

# negative: メタデータが違う proxy を成功扱いしない。
# 1080p 素材を 4K 素材の proxy と称して渡す。frame count も解像度も違う。
mvm_add_exit_test(proxy_mapping_metadata_mismatch_negative EXIT 3
    ARGS proxy-mapping
         --original "${MVM_SMOKE_DIR}/v1080p60_h264.mp4"
         --proxy "${MVM_BENCH_DIR}/v1080p60_h264.mp4"
         --random 0
    PROPERTIES REQUIRED_FILES "${MVM_SMOKE_DIR}/v1080p60_h264.mp4")

# --- performance ラベル ------------------------------------------------------
# 長時間の性能試験。通常 CTest には混ぜない。
# 実行: pwsh scripts/test.ps1 -Preset ucrt64-release -Performance
#
# **このラベルの緑を M5 / M8 の合格根拠として扱わない。**
# ここにあるのは「経路が壊れていないこと」を短時間で見る smoke であり、
# 閾値判定を含まない。M5 / M8 の判定は
#   pwsh scripts/seek-matrix.ps1     (M5: 1000 点 x 3 回)
#   pwsh scripts/preview-matrix.ps1  (M8: 60 秒 x 3 回)
# の記録された数値だけで行う。
#
# **テスト名は測っている対象と一致させる。**
# 旧名の seek_proxy / scrub_proxy は 1080p native の scenario を測っており
# proxy を一切使っていなかった。preview_4k_proxy は V1 だけを proxy 化した
# 部分 proxy を測っていた。名前で対象を誤認すると、判定そのものを誤る。

mvm_add_test(preview_1080p60_native_smoke
    COMMAND mvm_bench preview-bench "${MVM_S7_SCENARIO}"
            --consumer null --real-time -8 --measure-ms 30000 --warmup-ms 5000
    PROPERTIES
        REQUIRED_FILES "${MVM_S7_SCENARIO}"
        LABELS performance
        TIMEOUT 900)

# native (1080p) の seek。proxy は使っていない。
mvm_add_test(native_seek_smoke
    COMMAND mvm_bench seek-bench "${MVM_S7_SCENARIO}" --random 100
    PROPERTIES
        REQUIRED_FILES "${MVM_S7_SCENARIO}"
        LABELS performance
        TIMEOUT 1800)

# native (1080p) の scrub。proxy は使っていない。
mvm_add_test(native_scrub_smoke
    COMMAND mvm_bench scrub-bench "${MVM_S7_SCENARIO}"
            --requests 300 --pattern linear --submit-interval-us 16667
    PROPERTIES
        REQUIRED_FILES "${MVM_S7_SCENARIO}"
        LABELS performance
        TIMEOUT 1800)

if(EXISTS "${MVM_PX_V1_ABS}")
    mvm_add_test(proxy_generate_4k
        COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/proxy-matrix.ps1"
                -Candidates gop12 -Force
        PROPERTIES LABELS performance TIMEOUT 1800)

    # V1 だけを proxy 化した部分 proxy。**正式な M8 評価ではない。**
    mvm_add_test(preview_partial_proxy_diagnostic
        COMMAND mvm_bench preview-bench
                "${CMAKE_SOURCE_DIR}/bench/scenarios/s7-4k-proxy-gop12.json"
                --consumer null --real-time -8 --measure-ms 30000 --warmup-ms 5000
        PROPERTIES
            REQUIRED_FILES "${CMAKE_SOURCE_DIR}/bench/scenarios/s7-4k-proxy-gop12.json"
            LABELS performance
            TIMEOUT 900)

    mvm_add_test(proxy_frame_mapping
        COMMAND mvm_bench proxy-mapping
                --original "${MVM_BENCH_DIR}/v4k60_h264.mp4"
                --proxy "${MVM_PX_V1_ABS}"
                --random 200 --seed 20260804
        PROPERTIES LABELS performance TIMEOUT 1800)
endif()

# all-video proxy (V1 + V2)。正式な M8 / M5 が対象にしている構成である。
if(EXISTS "${MVM_PX_V1_ABS}" AND EXISTS "${MVM_PX_V2_ABS}")
    set(MVM_S7_4K_ALL "${CMAKE_SOURCE_DIR}/bench/scenarios/s7-4k-proxy-all-gop12.json")

    # **事前検査**: scenario が本当に V1 / V2 の両方を proxy 化しているか。
    # 部分 proxy の scenario を all-video のつもりで測るのを防ぐ。
    # required 2 件が解決できなければ exit 4 で落ちる。
    mvm_add_exit_test(preview_all_video_proxy_precheck EXIT 0
        ARGS resolve-proxy "${MVM_S7_4K}" --target preview
             --map "${MVM_MAP2}" --require-proxy-ids "${MVM_REQ2}"
        PROPERTIES
            REQUIRED_FILES "${MVM_S7_4K}"
            LABELS performance
            FIXTURES_SETUP mvm_all_proxy
            TIMEOUT 300)

    # 短時間の compose smoke。合成経路が all-video proxy で通ることを見る。
    mvm_add_test(compose_all_video_proxy_smoke
        COMMAND mvm_bench compose "${MVM_S7_4K_ALL}" --frame 3599 --expect-marker
        PROPERTIES
            REQUIRED_FILES "${MVM_S7_4K_ALL}"
            LABELS performance
            FIXTURES_REQUIRED mvm_all_proxy
            TIMEOUT 600)

    # 短時間の preview smoke。**判定用の 60 秒測定ではない。**
    mvm_add_test(preview_all_video_proxy_diagnostic
        COMMAND mvm_bench preview-bench "${MVM_S7_4K_ALL}"
                --consumer null --real-time -1 --measure-ms 10000 --warmup-ms 3000
        PROPERTIES
            REQUIRED_FILES "${MVM_S7_4K_ALL}"
            LABELS performance
            FIXTURES_REQUIRED mvm_all_proxy
            TIMEOUT 900)
endif()

# --- lint 自身の negative test ----------------------------------------------
# 検査を書いただけでは効いている証明にならない。
# 違反を入れたら落ち、正しいコードでは落ちないことを両方確かめる。

# lint.ps1 は違反を見つけたら exit 1 で落ちる。
# 「0 以外」ではなく 1 であることを見る。pwsh 自体の起動失敗や
# スクリプトの構文エラーでも落ちるが、それは 1 ではない。
add_test(NAME lint_producer_service_negative
    COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/expect-exit.ps1"
            -ExpectExit 1 -Exe "${MVM_PWSH}"
            # 先頭を空要素にしている。'-NoProfile' で始めると PowerShell が
            # -ChildArgs の値ではなくパラメータ名として束縛しようとする。
            -ChildArgs "|-NoProfile|-File|${CMAKE_SOURCE_DIR}/scripts/lint.ps1|-Path|${CMAKE_SOURCE_DIR}/tests/lint-negative/bad")

add_test(NAME lint_producer_service_control
    COMMAND "${MVM_PWSH}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/lint.ps1"
            -Path "${CMAKE_SOURCE_DIR}/tests/lint-negative/good")
