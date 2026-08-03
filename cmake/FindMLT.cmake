# FindMLT.cmake
#
# MLT 7 の C API のみを探す薄い pkg-config ラッパ。
#
# 重要: mlt++ (C++ ラッパ) は意図的に探さない。mvm は MLT の C API (mlt_*) だけを
# 使う方針であり、これにより
#   - Project Model / UI / 公開 interface へ MLT の型が漏れない
#   - 将来 MSVC 本体 + mingw 製 MLT DLL という構成へ退避する余地が残る
# の両方を確保する。mlt++ を追加したくなったら、まず ADR を更新すること。
#
# 定義するもの:
#   MLT_FOUND, MLT_VERSION, MLT::MLT (IMPORTED target)

find_package(PkgConfig REQUIRED)

pkg_check_modules(PC_MLT QUIET mlt-framework-7)

if(NOT PC_MLT_FOUND)
    # MSYS2 以外や version 差分に備えた fallback
    pkg_check_modules(PC_MLT QUIET mlt-framework)
endif()

find_path(MLT_INCLUDE_DIR
    NAMES framework/mlt.h
    HINTS ${PC_MLT_INCLUDEDIR} ${PC_MLT_INCLUDE_DIRS}
          "${MVM_UCRT64_ROOT}/include/mlt-7"
          "${MVM_UCRT64_ROOT}/include"
    PATH_SUFFIXES mlt-7 mlt
)

find_library(MLT_LIBRARY
    NAMES mlt-7 mlt
    HINTS ${PC_MLT_LIBDIR} ${PC_MLT_LIBRARY_DIRS} "${MVM_UCRT64_ROOT}/lib"
)

set(MLT_VERSION "${PC_MLT_VERSION}")

# MLT はモジュールとデータの場所を実行ファイル相対で推測する。開発ビルドでは
# 実行ファイルが build/ 配下にあるため、この推測は必ず外れる (モジュールが
# 1 つも見つからないまま静かに縮退する)。pkg-config から実際の場所を取得し、
# 呼び出し側が明示的に設定できるようにする。
# V11 (staging) では、これを実行ファイル相対の同梱パスに差し替える。
if(PC_MLT_FOUND)
    pkg_get_variable(MLT_MODULE_DIR mlt-framework-7 moduledir)
    pkg_get_variable(MLT_DATA_DIR   mlt-framework-7 mltdatadir)
endif()

if(NOT MLT_MODULE_DIR)
    set(MLT_MODULE_DIR "${MVM_UCRT64_ROOT}/lib/mlt")
endif()
if(NOT MLT_DATA_DIR)
    set(MLT_DATA_DIR "${MVM_UCRT64_ROOT}/share/mlt")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MLT
    REQUIRED_VARS MLT_LIBRARY MLT_INCLUDE_DIR
    VERSION_VAR MLT_VERSION
)

if(MLT_FOUND AND NOT TARGET MLT::MLT)
    add_library(MLT::MLT UNKNOWN IMPORTED)
    set_target_properties(MLT::MLT PROPERTIES
        IMPORTED_LOCATION "${MLT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MLT_INCLUDE_DIR}"
    )
    if(PC_MLT_CFLAGS_OTHER)
        set_property(TARGET MLT::MLT PROPERTY
            INTERFACE_COMPILE_OPTIONS "${PC_MLT_CFLAGS_OTHER}")
    endif()

    # MLT も UCRT64 配下から来ていることを保証する
    if(COMMAND mvm_require_under_ucrt64)
        mvm_require_under_ucrt64("libmlt" "${MLT_LIBRARY}")
    endif()
endif()

mark_as_advanced(MLT_INCLUDE_DIR MLT_LIBRARY)
