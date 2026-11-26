# SPDX-License-Identifier: Apache-2.0

# Suites that are not unit tests of one module.
add_subdirectory(spec)

# The spec runner's self-test: a script with wrong assertions the runner must report and fail on.
# Two are wrong in their values (1 + 2 is not 4, and dividing by zero is no overflow). The rest pin
# each failure assertion's stage: for every kind, one module that fails at its stage (passes) and
# one that fails with the right message at another stage (fails) — a malformed module to
# assert_invalid, an invalid one to assert_malformed, an uninstantiable one to assert_unlinkable and
# an unlinkable one to assert_uninstantiable.
add_test(NAME spec-selftest COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-selftest")
set_tests_properties(spec-selftest PROPERTIES LABELS unit PASS_REGULAR_EXPRESSION "passed 6, failed 6, skipped 0")
# The same script, checked for its exit status: a failing script must make the runner fail.
add_test(NAME spec-selftest-exit COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-selftest")
set_tests_properties(spec-selftest-exit PROPERTIES LABELS unit WILL_FAIL TRUE)

# The pinned skip count: a script of one skip line passes with one skip pinned, and fails with none
# pinned (the default) or two.
add_test(NAME spec-skips COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-skips" 1)
set_tests_properties(spec-skips PROPERTIES LABELS unit PASS_REGULAR_EXPRESSION "passed 0, failed 0, skipped 1")
add_test(NAME spec-skips-unpinned COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-skips")
set_tests_properties(spec-skips-unpinned PROPERTIES LABELS unit WILL_FAIL TRUE)
add_test(NAME spec-skips-fewer COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-skips" 2)
set_tests_properties(spec-skips-fewer PROPERTIES LABELS unit WILL_FAIL TRUE)

# The coverage ratchet's checker, tested on fixture reports: it needs Python, not a coverage build.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME coverage-check-selftest
             COMMAND Python3::Interpreter -I "${PROJECT_SOURCE_DIR}/scripts/coverage-check_test.py")
    set_tests_properties(coverage-check-selftest PROPERTIES LABELS lint)
    # The spec importer's translation: which commands become skip lines, and which fail the import.
    add_test(NAME import-spec-tests-selftest
             COMMAND Python3::Interpreter -I "${PROJECT_SOURCE_DIR}/scripts/import-spec-tests_test.py")
    set_tests_properties(import-spec-tests-selftest PROPERTIES LABELS lint)
endif()
