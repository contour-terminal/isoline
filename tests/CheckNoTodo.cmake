# SPDX-License-Identifier: Apache-2.0
#
# Checks that no source under a directory still calls core::todo: the grep gate of a subsystem whose every
# path is implemented, so that a placeholder cannot return to it unnoticed.
# Usage: cmake -DROOT=<directory> -P CheckNoTodo.cmake

if(NOT IS_DIRECTORY "${ROOT}")
    message(FATAL_ERROR "no directory to check: '${ROOT}'")
endif()
file(GLOB_RECURSE sources "${ROOT}/*.cpp" "${ROOT}/*.hpp" "${ROOT}/*.def")
if(NOT sources)
    message(FATAL_ERROR "no sources under ${ROOT}")
endif()
set(offenders "")
foreach(source IN LISTS sources)
    file(STRINGS "${source}" hits REGEX "core::todo")
    if(hits)
        string(APPEND offenders "\n  ${source}")
    endif()
endforeach()
if(offenders)
    message(FATAL_ERROR "core::todo remains in:${offenders}")
endif()
