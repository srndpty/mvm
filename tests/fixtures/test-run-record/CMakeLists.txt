# scripts/lib/test-run-record.ps1 の契約試験の fixture。compiler は使わない。
# 起動器と probe は mvm の build から -D で受け取る (MVM_FIXTURE_LAUNCHER・MVM_FIXTURE_PROBE)。
cmake_minimum_required(VERSION 3.29)
project(mvm_test_run_record_fixture NONE)
enable_testing()

add_test(NAME fixture_pass COMMAND "${MVM_FIXTURE_LAUNCHER}" --test "${MVM_FIXTURE_PROBE}" echo)
add_test(NAME fixture_fail COMMAND "${MVM_FIXTURE_LAUNCHER}" --test "${MVM_FIXTURE_PROBE}" exit 1)
add_test(NAME fixture_crash COMMAND "${MVM_FIXTURE_LAUNCHER}" --test "${MVM_FIXTURE_PROBE}" crash)
add_test(NAME fixture_timeout COMMAND "${MVM_FIXTURE_LAUNCHER}" --test "${MVM_FIXTURE_PROBE}" sleep 30000)
set_tests_properties(fixture_timeout PROPERTIES TIMEOUT 2)
