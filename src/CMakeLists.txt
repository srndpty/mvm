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

# --- Qt probe ---------------------------------------------------------------
# Qt が UCRT64 版であり、実際にリンク・起動できることを確認する。
if(MVM_ENABLE_QT)
    add_executable(mvm_qt_probe app/qt_probe.cpp)
    target_link_libraries(mvm_qt_probe PRIVATE Qt6::Core mvm::warnings)
endif()
