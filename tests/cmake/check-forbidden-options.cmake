# SPDX-License-Identifier: Apache-2.0
#
# install-forbidden-options: every option an installed target is built with is one of the build's own,
# as isoline_apply_build_options() recorded them (cmake/IsolinePedantic.cmake), the list check-install's
# step 4 holds the installed package to. An option outside it would leave that check blind to its leak.
#
#   cmake -DAPPLIED_FILE=<target TAB option ... per line> -DFORBIDDEN_OPTIONS_FILE=<one option per line>
#         -P tests/cmake/check-forbidden-options.cmake

cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS APPLIED_FILE FORBIDDEN_OPTIONS_FILE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "check-forbidden-options: -D${required}=... is required")
    endif()
endforeach()
file(STRINGS "${FORBIDDEN_OPTIONS_FILE}" forbidden)
file(STRINGS "${APPLIED_FILE}" lines)
if(NOT lines)
    message(FATAL_ERROR "check-forbidden-options: ${APPLIED_FILE} names no installed target")
endif()
set(unrecorded "")
foreach(line IN LISTS lines)
    string(REPLACE "\t" ";" fields "${line}")
    list(POP_FRONT fields target)
    foreach(option IN LISTS fields)
        if(NOT option STREQUAL "" AND NOT option IN_LIST forbidden)
            list(APPEND unrecorded "${target}: ${option}")
        endif()
    endforeach()
endforeach()
if(unrecorded)
    list(JOIN unrecorded "\n  " unrecordedLines)
    message(FATAL_ERROR "check-forbidden-options: installed targets are built with options the record of "
                        "isoline_apply_build_options() does not hold, so check-install cannot see them leak; "
                        "apply them through it:\n  ${unrecordedLines}")
endif()
list(LENGTH lines targetCount)
message(STATUS "check-forbidden-options: the options of ${targetCount} installed targets are all recorded")
