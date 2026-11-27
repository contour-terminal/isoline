# SPDX-License-Identifier: Apache-2.0
#
# Checks the name a loader resolves for the C API's shared library (plan P17a, D8):
#
#   cmake -DFORMAT=elf|pe|macho -DTOOL=<tool> -DFILE=<file> -DEXPECTED=<name> -P CheckLibraryName.cmake
#
# elf    FILE is the shared library; its SONAME (readelf -d) must be EXPECTED.
# pe     FILE is a program linked against the DLL; its import table (llvm-readobj --coff-imports) must
#        name EXPECTED, the DLL's file name.
# macho  FILE is the shared library; the file name of its install name (otool -D) must be EXPECTED.

cmake_minimum_required(VERSION 3.25)

# format|tool arguments|the pattern of the line that names it, the name being its first group
set(rows
    "elf|-d|Library soname: \\[(.+)\\]"
    "pe|--coff-imports|Name: (isoline_c[^ ]*[.]dll)"
    "macho|-D|^.*/?(lib[^/]+[.]dylib)$")

foreach(variable IN ITEMS FORMAT TOOL FILE EXPECTED)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "CheckLibraryName: ${variable} is not set")
    endif()
endforeach()

set(arguments "")
foreach(row IN LISTS rows)
    if(row MATCHES "^([a-z]+)[|]([^|]+)[|](.+)$" AND CMAKE_MATCH_1 STREQUAL FORMAT)
        set(arguments "${CMAKE_MATCH_2}")
        set(pattern "${CMAKE_MATCH_3}")
    endif()
endforeach()
if(arguments STREQUAL "")
    message(FATAL_ERROR "CheckLibraryName: no row for the object format '${FORMAT}'")
endif()

execute_process(COMMAND "${TOOL}" ${arguments} "${FILE}" RESULT_VARIABLE status OUTPUT_VARIABLE output
                ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "CheckLibraryName: ${TOOL} ${arguments} ${FILE} failed (${status}): ${errors}")
endif()
string(REPLACE "\n" ";" lines "${output}")
set(found "")
foreach(line IN LISTS lines)
    if(line MATCHES "${pattern}")
        list(APPEND found "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(NOT found STREQUAL EXPECTED)
    message(FATAL_ERROR "CheckLibraryName: ${FILE} names '${found}', not '${EXPECTED}'")
endif()
message(STATUS "CheckLibraryName: ${FILE} names ${EXPECTED}")
