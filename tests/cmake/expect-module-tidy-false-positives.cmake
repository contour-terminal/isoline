# SPDX-License-Identifier: Apache-2.0
#
# clang-tidy-module-tripwire, E2's (D13): runs the build's clang-tidy on one source of each kind that
# ISOLINE_MODULE_TIDY_FALSE_POSITIVES lists (cmake/IsolineModules.cmake), through the build's
# compile_commands.json, and passes while every listed check still fires there. One that no longer does
# fails it, named, so that E2's list, and the checks lifted for its kind, shrink with it.
#
#   cmake -DCLANG_TIDY=<clang-tidy> -DBUILD_DIR=<build tree> -DARGUMENTS_FILE=<file>
#         -P tests/cmake/expect-module-tidy-false-positives.cmake
#
# ARGUMENTS_FILE sets ROWS, the (kind, check) rows, and SOURCES, (kind, source) rows: lists, which a
# command line would split. A row of a kind with no source fails it: every row must have a tripwire. The
# sources compile in the build, so their module maps exist: run it after the build.

cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS CLANG_TIDY BUILD_DIR ARGUMENTS_FILE)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "expect-module-tidy-false-positives: -D${argument}=... is required")
    endif()
endforeach()
include("${ARGUMENTS_FILE}")

execute_process(COMMAND "${CLANG_TIDY}" --version OUTPUT_VARIABLE versionOutput)
string(REGEX MATCH "LLVM version ([0-9.]+)" ignored "${versionOutput}")
set(tidyVersion "${CMAKE_MATCH_1}")

set(failures "")
set(kinds "")
set(sources "${SOURCES}")
while(sources)
    list(POP_FRONT sources kind source)
    list(APPEND kinds "${kind}")
endwhile()
set(rows "${ROWS}")
while(rows)
    list(POP_FRONT rows rowKind check)
    if(NOT rowKind IN_LIST kinds)
        list(APPEND failures "${check} on ${rowKind}: no source of kind ${rowKind} to run clang-tidy on")
    endif()
endwhile()

set(sources "${SOURCES}")
while(sources)
    list(POP_FRONT sources kind source)
    # The exit status says only that a check fired, which the output tells check by check.
    execute_process(COMMAND "${CLANG_TIDY}" -p "${BUILD_DIR}" "${source}"
                    OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    set(rows "${ROWS}")
    while(rows)
        list(POP_FRONT rows rowKind check)
        if(rowKind STREQUAL kind AND NOT output MATCHES "\\[${check}[],]")
            list(APPEND failures "clang-tidy ${tidyVersion} no longer reports ${check} on ${kind}: drop its row "
                                 "from ISOLINE_MODULE_TIDY_FALSE_POSITIVES, and the kind's "
                                 "isoline_lift_module_tidy_checks() calls where nothing is left")
        endif()
    endwhile()
endwhile()

if(failures)
    list(JOIN failures "\n" message)
    message(FATAL_ERROR "${message}")
endif()
message(STATUS "E2 holds: every row of ISOLINE_MODULE_TIDY_FALSE_POSITIVES fires (clang-tidy ${tidyVersion})")
