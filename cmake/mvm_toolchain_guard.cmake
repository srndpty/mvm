# mvm_toolchain_guard.cmake
#
# 目的: Phase 0 のビルドが MSYS2 UCRT64 (C:/msys64/ucrt64) で完結していることを
# configure 時に機械的に保証する。
#
# 背景:
#   この開発機には他プロジェクト用の Qt 6.8.3 (MSVC ビルド) が
#   C:/Users/lambe/sdk/Qt/6.8.3/msvc2022_64 に存在する。これを誤って拾うと、
#   MSVC ABI と mingw ABI が混在し「リンクは通るのに実行時に不可解に壊れる」
#   最も原因究明が困難な事故になる。受動的に PATH が綺麗であることに頼らず、
#   能動的に検証して違えば configure を失敗させる。
#
#   ホストの pip 版 cmake (Python310/Scripts) や C:/tools/ffmpeg.exe も同様に
#   誤検出の事故源であるため、同じ仕組みで弾く。

set(MVM_UCRT64_ROOT "C:/msys64/ucrt64" CACHE PATH
    "Phase 0 で使用する MSYS2 UCRT64 のルート。ここ以外の toolchain / Qt は拒否される")

# 既知の「拾ってはいけない」パス断片。小文字で比較する。
set(_MVM_FORBIDDEN_FRAGMENTS
    "sdk/qt"          # 既存 Qt 6.8.3 (他プロジェクト用、保持するが参照しない)
    "msvc"            # MSVC ビルドの Qt / ツール全般
    "/qt/6."          # 公式インストーラ形式の Qt レイアウト
    "visual studio"
    "python310/scripts"   # pip 版 cmake
    "/msys64/mingw64/"    # UCRT64 以外の MSYS2 環境
    "/msys64/clang64/"
    "/msys64/mingw32/"
)

# パスを比較可能な正規形へ: CMake 形式 + 実体解決 + 小文字化
function(_mvm_normalize_path out_var raw_path)
    if(NOT raw_path)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    file(TO_CMAKE_PATH "${raw_path}" _p)
    get_filename_component(_p "${_p}" REALPATH)
    string(TOLOWER "${_p}" _p)
    set(${out_var} "${_p}" PARENT_SCOPE)
endfunction()

# 指定パスが MVM_UCRT64_ROOT 配下であることを要求する。
# 違反時は、何をどう直せばよいかまで書いた FATAL_ERROR を出す。
function(mvm_require_under_ucrt64 label raw_path)
    _mvm_normalize_path(_path "${raw_path}")
    _mvm_normalize_path(_root "${MVM_UCRT64_ROOT}")

    if(NOT _path)
        message(FATAL_ERROR
            "[mvm toolchain guard] ${label} が解決できませんでした。\n"
            "  MSYS2 UCRT64 シェルから configure しているか確認してください。")
    endif()

    string(FIND "${_path}" "${_root}/" _idx)
    if(NOT _idx EQUAL 0)
        set(_hint "")
        foreach(_frag IN LISTS _MVM_FORBIDDEN_FRAGMENTS)
            string(FIND "${_path}" "${_frag}" _fidx)
            if(NOT _fidx EQUAL -1)
                set(_hint "  検出された既知の誤参照パターン: '${_frag}'\n")
                break()
            endif()
        endforeach()

        message(FATAL_ERROR
            "[mvm toolchain guard] ${label} が UCRT64 の外を指しています。\n"
            "  実際:   ${_path}\n"
            "  必須:   ${_root}/ 配下\n"
            "${_hint}"
            "\n"
            "  mvm Phase 0 は MSYS2 UCRT64 で統一する方針です。CRT および C++ ABI を\n"
            "  混在させると、リンクが通っても実行時に不可解な形で破綻します。\n"
            "\n"
            "  対処:\n"
            "    1. MSYS2 UCRT64 シェル (ucrt64.exe) から configure する\n"
            "    2. CMakePresets.json の 'ucrt64-debug' / 'ucrt64-release' を使う\n"
            "    3. 古い build ディレクトリを削除してから configure し直す\n"
            "       (CMakeCache.txt に誤った値が残っている可能性があります)\n"
            "    4. 環境変数 CMAKE_PREFIX_PATH / Qt6_DIR / QTDIR を確認する\n"
            "\n"
            "  注意: C:/Users/lambe/sdk/Qt/6.8.3 は他プロジェクト用として保持している\n"
            "        MSVC ビルドの Qt です。削除・変更せず、mvm からは参照しません。")
    endif()

    message(STATUS "[mvm toolchain guard] OK  ${label}: ${_path}")
endfunction()

# compiler と pkg-config を検証する。find_package(Qt6) より前に呼ぶ。
function(mvm_guard_toolchain)
    message(STATUS "[mvm toolchain guard] UCRT64 root = ${MVM_UCRT64_ROOT}")

    if(NOT EXISTS "${MVM_UCRT64_ROOT}/bin")
        message(FATAL_ERROR
            "[mvm toolchain guard] MSYS2 UCRT64 が見つかりません: ${MVM_UCRT64_ROOT}\n"
            "  scripts/bootstrap-msys2.ps1 を実行して依存を導入してください。")
    endif()

    mvm_require_under_ucrt64("C++ compiler"  "${CMAKE_CXX_COMPILER}")
    mvm_require_under_ucrt64("C compiler"    "${CMAKE_C_COMPILER}")

    find_program(MVM_PKG_CONFIG_EXECUTABLE NAMES pkgconf pkg-config
                 HINTS "${MVM_UCRT64_ROOT}/bin" NO_DEFAULT_PATH)
    if(NOT MVM_PKG_CONFIG_EXECUTABLE)
        message(FATAL_ERROR
            "[mvm toolchain guard] pkgconf が ${MVM_UCRT64_ROOT}/bin に見つかりません。\n"
            "  pacman -S mingw-w64-ucrt-x86_64-pkgconf")
    endif()
    mvm_require_under_ucrt64("pkg-config" "${MVM_PKG_CONFIG_EXECUTABLE}")
    set(PKG_CONFIG_EXECUTABLE "${MVM_PKG_CONFIG_EXECUTABLE}" CACHE FILEPATH "" FORCE)

    # ホスト側の非 UCRT64 な残骸が紛れ込んでいないか
    foreach(_var CMAKE_PREFIX_PATH Qt6_DIR QTDIR)
        if(DEFINED ENV{${_var}} AND NOT "$ENV{${_var}}" STREQUAL "")
            message(STATUS "[mvm toolchain guard] 環境変数 ${_var}=$ENV{${_var}} (preset で上書きされます)")
        endif()
    endforeach()
endfunction()

# Qt を検証する。find_package(Qt6 ...) の直後に呼ぶ。
# Qt6_DIR のような設定値ではなく、実際にリンクされる Qt6::Core の実体を見る。
function(mvm_guard_qt)
    if(NOT TARGET Qt6::Core)
        message(FATAL_ERROR "[mvm toolchain guard] mvm_guard_qt() は find_package(Qt6) の後に呼ぶこと")
    endif()

    mvm_require_under_ucrt64("Qt6_DIR" "${Qt6_DIR}")

    # 設定値ではなく実体を検証する。ここが本命。
    set(_loc "")
    foreach(_prop IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION_DEBUG IMPORTED_LOCATION)
        get_target_property(_v Qt6::Core ${_prop})
        if(_v AND NOT _v STREQUAL "_v-NOTFOUND")
            set(_loc "${_v}")
            break()
        endif()
    endforeach()

    if(NOT _loc)
        message(FATAL_ERROR
            "[mvm toolchain guard] Qt6::Core の IMPORTED_LOCATION を取得できませんでした。\n"
            "  Qt の実体を検証できないため、安全側に倒して失敗させます。")
    endif()
    mvm_require_under_ucrt64("Qt6::Core の実体" "${_loc}")

    message(STATUS "[mvm toolchain guard] Qt ${Qt6_VERSION} (UCRT64) を使用します")
endfunction()
