# crash の証拠 (docs/crash-evidence.md)。
#
# CTest は子 process に SEM_NOGPFAULTERRORBOX を渡すので、試験が落ちても WER は呼ばれず、dump も
# Application の event log も残らない (2026-10-15 に実測)。この file の後に作る実行ファイルを add_test の
# COMMAND に使うと、CTest は mvm_wer_launch を通して起動する (TEST_LAUNCHER)。起動器は error mode を
# 戻し、子の終了コードをそのまま返し、落ちたら子の dump を MVM_CRASH_STAGING へ複製する。debugger は
# 付けない。
#
# apps/ の試験用の実行ファイル (mvm_test_text_ui_input など) にも掛けるため、top-level で
# add_subdirectory(src) の前に include する。起動器と probe 自身は、この設定の前に作る。
# pwsh から起動する試験 (COMMAND が pwsh) には掛からない。

add_executable(mvm_wer_launch tests/harness/wer_launch.c)
target_link_options(mvm_wer_launch PRIVATE -municode)
target_link_libraries(mvm_wer_launch PRIVATE mvm::warnings)
add_executable(mvm_test_crash_probe tests/harness/crash_probe.c)
target_link_libraries(mvm_test_crash_probe PRIVATE mvm::warnings)
set(CMAKE_TEST_LAUNCHER "$<TARGET_FILE:mvm_wer_launch>;--test")
