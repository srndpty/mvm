cmake_minimum_required(VERSION 3.25)
project(mvm_ctest_selection NONE)
enable_testing()
find_program(MVM_PWSH NAMES pwsh REQUIRED)

add_test(NAME normal_build_dependent COMMAND "${CMAKE_COMMAND}" -E true)
add_test(NAME normal_build_independent
    COMMAND "${MVM_PWSH}" -NoProfile -Command "exit 0"
        "${CMAKE_BINARY_DIR}/tests/independent-output.json")
add_test(NAME dependent_pwsh
    COMMAND "${MVM_PWSH}" -NoProfile -Command "exit 0"
        "${CMAKE_BINARY_DIR}/bin/mock.exe")
add_test(NAME linked_pwsh COMMAND "${MVM_PWSH}" -NoProfile -Command "exit 0")
set_tests_properties(linked_pwsh PROPERTIES DEPENDS normal_build_dependent)

foreach(label IN ITEMS extended workstation performance stability)
    add_test(NAME "${label}_case" COMMAND "${CMAKE_COMMAND}" -E true)
    set_tests_properties("${label}_case" PROPERTIES LABELS "${label}")
endforeach()
