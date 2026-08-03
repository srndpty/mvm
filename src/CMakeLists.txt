# Phase 0 S1: 依存が正しく解決され、実行時にも動作することを確認するための
# 最小ターゲットのみ。製品コードはまだ無い。

# --- MLT hello world --------------------------------------------------------
# MLT が「リンクできる」だけでなく「モジュールを実際にロードできる」ことまで確認する。
# MLT はモジュールを実行時に動的ロードするため、リンク成功は動作を保証しない。
add_executable(mvm_mlt_hello media/mlt/mlt_hello.c)
target_link_libraries(mvm_mlt_hello PRIVATE MLT::MLT)
target_compile_definitions(mvm_mlt_hello PRIVATE
    MVM_MLT_VERSION_STRING="${MLT_VERSION}"
    # 開発ビルドでは実行ファイルが build/ 配下にあり、MLT の実行ファイル相対の
    # 推測が必ず外れる。ビルド時に実際の場所を埋め込む。
    # V11 (staging) ではこれを同梱パスへ差し替える。
    MVM_MLT_MODULE_DIR="${MLT_MODULE_DIR}"
    MVM_MLT_DATA_DIR="${MLT_DATA_DIR}"
)

# --- Qt probe ---------------------------------------------------------------
# Qt が UCRT64 版であり、実際にリンク・起動できることを確認する。
if(MVM_ENABLE_QT)
    add_executable(mvm_qt_probe app/qt_probe.cpp)
    target_link_libraries(mvm_qt_probe PRIVATE Qt6::Core mvm::warnings)
endif()
