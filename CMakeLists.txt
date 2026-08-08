cmake_minimum_required(VERSION 3.25)

# --- project() より前に PATH を検査する --------------------------------------
# UCRT64 の gcc.exe は C:/msys64/ucrt64/bin が PATH に無いと、依存 DLL を解決
# できずエラー出力なしに終了する。その状態で project() を呼ぶと CMake は
# 「The C compiler is not able to compile a simple test program」という
# 原因の分からないメッセージを出すだけになる。先回りして実情を伝える。
if(NOT DEFINED MVM_SKIP_PATH_CHECK)
    set(_mvm_ucrt64_bin "C:/msys64/ucrt64/bin")
    string(TOLOWER "$ENV{PATH}" _mvm_path_lower)
    string(REPLACE "\\" "/" _mvm_path_lower "${_mvm_path_lower}")
    string(FIND "${_mvm_path_lower}" "msys64/ucrt64/bin" _mvm_path_idx)
    if(_mvm_path_idx EQUAL -1)
        message(FATAL_ERROR
            "\n"
            "[mvm] PATH に ${_mvm_ucrt64_bin} が含まれていません。\n"
            "\n"
            "  UCRT64 の gcc は依存 DLL を PATH から解決するため、この状態では\n"
            "  エラーメッセージを出さずに失敗します。\n"
            "\n"
            "  対処 (いずれか):\n"
            "    - pwsh scripts/build.ps1 を使う (PATH を設定して cmake を呼ぶ)\n"
            "    - MSYS2 UCRT64 シェル (C:/msys64/ucrt64.exe) から cmake を実行する\n"
            "    - VSCode の統合ターミナル 'MSYS2 UCRT64' を使う\n"
            "\n"
            "  この検査を意図的に外す場合: -DMVM_SKIP_PATH_CHECK=ON\n")
    endif()
endif()

project(mvm
    VERSION 0.0.1
    DESCRIPTION "mvm - non-linear video editor (Phase 0 technical spike)"
    LANGUAGES C CXX
)

# --- Phase 0 の範囲 ---------------------------------------------------------
# 現在実装されているのは S0 (リポジトリ初期化) と S1 (依存導入 + MLT hello world)
# のみ。S2 以降のターゲット (mvm_bench, mvm_spike, テスト群) はまだ追加しない。
# 詳細は docs/phase0-plan.md を参照。

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# --- ツールチェーン検証 (最優先で実行する) ----------------------------------
# 既存の Qt 6.8.3 (MSVC) や pip 版 cmake を拾っていないことを、ここで確定させる。
include(mvm_toolchain_guard)
mvm_guard_toolchain()

include(mvm_warnings)

# --- カバレッジ -------------------------------------------------------------
# 専用ビルドディレクトリ (build/ucrt64-coverage) でのみ有効にする。
# Phase 0 では合格条件にせず、「一度も実行されていない検証コード」を
# 見つけるために使う。
option(MVM_ENABLE_COVERAGE "gcov 用の計測を有効にする" OFF)
if(MVM_ENABLE_COVERAGE)
    message(STATUS "[mvm] カバレッジ計測を有効化します")
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

# --- 依存 -------------------------------------------------------------------
option(MVM_ENABLE_QT "Qt6 を探して検証する (S1 の依存確認用)" ON)

find_package(PkgConfig REQUIRED)

find_package(MLT REQUIRED)
message(STATUS "[mvm] MLT ${MLT_VERSION}: ${MLT_LIBRARY}")

# 開発ビルドでは実行ファイルが build/ 配下にあり、MLT の実行ファイル相対の
# 推測が必ず外れる (モジュール 0 件のまま静かに縮退する)。ビルド時に実際の
# 場所を埋め込む。V11 (staging) ではこれを同梱パスへ差し替える。
#
# src/ と tests/ の両方から使うので、ここ (トップレベル) で定義する。
set(MVM_MLT_PATH_DEFS
    MVM_MLT_MODULE_DIR="${MLT_MODULE_DIR}"
    MVM_MLT_DATA_DIR="${MLT_DATA_DIR}"
)
message(STATUS "[mvm] MLT modules: ${MLT_MODULE_DIR}")
message(STATUS "[mvm] MLT data   : ${MLT_DATA_DIR}")

if(MVM_ENABLE_QT)
    # Phase 1 の PreviewRhiItem は QRhi (QtGui の private API) を使う。
    # 互換保証が弱いことは docs/phase1-plan.md §7 に記録し、
    # 依存を src/app/preview/ の 2 ファイルへ隔離している。
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick Svg GuiPrivate Qml QuickControls2)
    mvm_guard_qt()
endif()

# --- Phase 1 / P1: FFmpeg -----------------------------------------------------
# UCRT64 の libav* だけを使う。ホストの C:\tools 版・winget 版へは落ちない。
pkg_check_modules(MVM_FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswresample)
message(STATUS "[mvm] FFmpeg: libavcodec ${MVM_FFMPEG_libavcodec_VERSION}")

add_subdirectory(src)

# Phase 1 / P1 の検証アプリ。Qt が無ければ作らない。
if(MVM_ENABLE_QT)
    qt_standard_project_setup(REQUIRES 6.5)
    add_subdirectory(apps/preview_spike)
    add_subdirectory(apps/compositor_spike)
    add_subdirectory(apps/p3_av_sync_spike)
endif()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
