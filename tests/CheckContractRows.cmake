# SPDX-License-Identifier: Apache-2.0
#
# Checks that the [.contract] cases of a Catch2 binary are exactly the expected ones.
# Usage: cmake -DTEST_BINARY=<isoline_test> -DEXPECTED=<case,case,...> -P CheckContractRows.cmake

execute_process(COMMAND "${TEST_BINARY}" "[.contract]" --list-tests --verbosity quiet
                OUTPUT_VARIABLE listing
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "listing the [.contract] cases failed (${status}):\n${listing}")
endif()
string(REGEX REPLACE "\r?\n" ";" actual "${listing}")
list(TRANSFORM actual STRIP)
list(REMOVE_ITEM actual "")
list(SORT actual)
string(REPLACE "," ";" expected "${EXPECTED}")
list(SORT expected)
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "the [.contract] cases and the contract rows of tests/CMakeLists.txt differ\n"
                        "  cases: ${actual}\n  rows:  ${expected}")
endif()
