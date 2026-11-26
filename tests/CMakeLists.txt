# SPDX-License-Identifier: Apache-2.0

# Suites that are not unit tests of one module.
add_subdirectory(spec)

# The spec runner's self-test: a script with wrong assertions the runner must report and fail on.
# Two are wrong in their values (1 + 2 is not 4, and dividing by zero is no overflow). The rest pin
# each failure assertion's stage: for every kind, one module that fails at its stage (passes) and
# one that fails with the right message at another stage (fails) — a malformed module to
# assert_invalid, an invalid one to assert_malformed, an uninstantiable one to assert_unlinkable and
# an unlinkable one to assert_uninstantiable. Then a definition, instantiated under a name and invoked
# by it and by an either, passes; an assert_exception of a function that returns, an
# assert_invalid_custom of a module without custom sections, and an instance of an unknown
# definition fail. Last, an instance of a named module, which names a definition too, passes.
add_test(NAME spec-selftest COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-selftest" 0 --features wasm2)
set_tests_properties(spec-selftest PROPERTIES LABELS unit PASS_REGULAR_EXPRESSION "passed 13, failed 9, skipped 0")
# The same script, checked for its exit status: a failing script must make the runner fail.
add_test(NAME spec-selftest-exit
         COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-selftest" 0 --features wasm2)
set_tests_properties(spec-selftest-exit PROPERTIES LABELS unit WILL_FAIL TRUE)

# The pinned skip count: a script of one skip line passes with one skip pinned, and fails with none
# pinned or two.
add_test(NAME spec-skips COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-skips" 1 --features wasm2)
set_tests_properties(spec-skips PROPERTIES LABELS unit PASS_REGULAR_EXPRESSION "passed 0, failed 0, skipped 1")
add_test(NAME spec-skips-none COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-skips" 0 --features wasm2)
set_tests_properties(spec-skips-none PROPERTIES LABELS unit PASS_REGULAR_EXPRESSION "skipped 1, but 0 skips are pinned")
add_test(NAME spec-skips-fewer COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-skips" 2 --features wasm2)
set_tests_properties(spec-skips-fewer PROPERTIES LABELS unit WILL_FAIL TRUE)

# The feature sets: a module of extended constant expressions, a 3.0 feature, instantiates under
# wasm3 and all, and fails, with the read of its global, under wasm2.
foreach(features IN ITEMS wasm3 all)
    add_test(NAME spec-features-${features}
             COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-features" 0 --features ${features})
    set_tests_properties(spec-features-${features} PROPERTIES LABELS unit
                         PASS_REGULAR_EXPRESSION "passed 2, failed 0, skipped 0")
endforeach()
add_test(NAME spec-features-wasm2
         COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-features" 0 --features wasm2)
set_tests_properties(spec-features-wasm2 PROPERTIES LABELS unit PASS_REGULAR_EXPRESSION "passed 0, failed 2, skipped 0")

# The command line: a skip count and a feature set are required, and the set must be one it knows.
foreach(usage IN ITEMS "1" "1;--features" "1;--features;wasm4" "x;--features;wasm2" "1;--feature;wasm2")
    string(REPLACE ";" "-" usageName "${usage}")
    add_test(NAME spec-usage-${usageName}
             COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/spec-skips" ${usage})
    set_tests_properties(spec-usage-${usageName} PROPERTIES LABELS unit PASS_REGULAR_EXPRESSION "usage: ")
endforeach()

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
    # docs/conformance.md lists exactly the excluded files and the assertions the GC cut skips.
    add_test(NAME conformance-doc-current
             COMMAND Python3::Interpreter -I "${PROJECT_SOURCE_DIR}/scripts/check-conformance.py")
    set_tests_properties(conformance-doc-current PROPERTIES LABELS lint)
    add_test(NAME check-conformance-selftest
             COMMAND Python3::Interpreter -I "${PROJECT_SOURCE_DIR}/scripts/check-conformance_test.py")
    set_tests_properties(check-conformance-selftest PROPERTIES LABELS lint)
endif()
