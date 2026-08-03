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

# --- 依存 -------------------------------------------------------------------
option(MVM_ENABLE_QT "Qt6 を探して検証する (S1 の依存確認用)" ON)

find_package(MLT REQUIRED)
message(STATUS "[mvm] MLT ${MLT_VERSION}: ${MLT_LIBRARY}")

if(MVM_ENABLE_QT)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick Svg)
    mvm_guard_qt()
endif()

add_subdirectory(src)
