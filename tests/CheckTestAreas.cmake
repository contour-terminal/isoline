# SPDX-License-Identifier: Apache-2.0
#
# Checks that the test areas of a Catch2 binary are exactly the registered ones: every case the unit run selects
# (SELECTION) is named "<area>.<name>" for one of AREAS, and every area names at least one case, so that no case
# is left out of every area's run and no area runs nothing.
# Usage: cmake -DTEST_BINARY=<isoline_test> [-DTEST_EMULATOR=<command|argument|...>] -DSELECTION=<test spec>
#        -DAREAS=<area,area,...> -P CheckTestAreas.cmake
# TEST_EMULATOR is a cross build's emulator (qemu, wine), its items separated by |; empty for a native build.

string(REPLACE "|" ";" emulator "${TEST_EMULATOR}")
execute_process(COMMAND ${emulator} "${TEST_BINARY}" "${SELECTION}" --list-tests --verbosity quiet
                OUTPUT_VARIABLE listing
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "listing the cases of '${SELECTION}' failed (${status}):\n${listing}")
endif()
string(REGEX REPLACE "\r?\n" ";" cases "${listing}")
list(TRANSFORM cases STRIP)
list(REMOVE_ITEM cases "")
string(REPLACE "," ";" areas "${AREAS}")
set(caseAreas)
set(outside)
foreach(case IN LISTS cases)
    string(FIND "${case}" "." dot)
    if(dot GREATER 0)
        string(SUBSTRING "${case}" 0 ${dot} area)
    else()
        set(area "")
    endif()
    if(area IN_LIST areas)
        list(APPEND caseAreas "${area}")
    else()
        list(APPEND outside "${case}")
    endif()
endforeach()
set(empty)
foreach(area IN LISTS areas)
    if(NOT area IN_LIST caseAreas)
        list(APPEND empty "${area}")
    endif()
endforeach()
if(outside OR empty)
    list(JOIN outside "\n    " outsideText)
    list(JOIN empty ", " emptyText)
    message(FATAL_ERROR "the test areas of src/isoline/CMakeLists.txt and the cases of '${SELECTION}' differ\n"
                        "  cases in no area:\n    ${outsideText}\n  areas without a case: ${emptyText}")
endif()
list(LENGTH cases caseCount)
list(LENGTH areas areaCount)
message(STATUS "${caseCount} cases in ${areaCount} areas")
