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
target_link_libraries(mvm_wer_launch PRIVATE mvm::warnings dbghelp)
add_executable(mvm_test_crash_probe tests/harness/crash_probe.c)
target_link_libraries(mvm_test_crash_probe PRIVATE mvm::warnings)
set(CMAKE_TEST_LAUNCHER "$<TARGET_FILE:mvm_wer_launch>;--test")

# 起動器に各試験の TIMEOUT を渡す (MVM_TEST_TIMEOUT_SECONDS)。起動器は打ち切りの少し前に、終わらない
# 子の dump を書く。TIMEOUT の無い試験は、scripts/test.ps1 が ctest の --timeout と同じ値を環境で渡す。
# 全 directory の add_test と set_tests_properties の後 (top-level の最後) に呼ぶ。
function(mvm_pass_test_timeouts_to_launcher)
    if(CMAKE_VERSION VERSION_LESS 3.28)
        message(FATAL_ERROR "別の directory の試験の property を設定するには CMake 3.28 以降が要ります")
    endif()
    set(pending "${CMAKE_SOURCE_DIR}")
    while(pending)
        list(POP_FRONT pending dir)
        get_property(children DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
        list(APPEND pending ${children})
        get_property(tests DIRECTORY "${dir}" PROPERTY TESTS)
        foreach(test IN LISTS tests)
            get_property(timeout TEST "${test}" DIRECTORY "${dir}" PROPERTY TIMEOUT)
            if(timeout)
                set_property(TEST "${test}" DIRECTORY "${dir}" APPEND PROPERTY
                    ENVIRONMENT_MODIFICATION "MVM_TEST_TIMEOUT_SECONDS=set:${timeout}")
            endif()
        endforeach()
    endwhile()
endfunction()
