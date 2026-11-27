# SPDX-License-Identifier: Apache-2.0
#
# module-import-then-include (D7): builds the probe target, whose one TU imports isoline and then includes an
# Isoline header, and holds the outcome to its compiler's row of ISOLINE_MODULE_IMPORT_THEN_INCLUDE
# (tests/modules/CMakeLists.txt). A row `works` passes while the build succeeds; a row `fails` passes while
# the build fails with output that matches the row's diagnostic, a regular expression. Either other outcome fails, named: an unexpected success means
# the table in docs/modules.md is out of date.
#
#   cmake -DBUILD_DIR=<build tree> -DTARGET=<probe target> [-DCONFIG=<configuration>]
#         -DCOMPILER=<id> -DCOMPILER_VERSION=<version> -DEXPECTED=works|fails -DDIAGNOSTIC=<regular expression>
#         -P tests/cmake/expect-import-then-include.cmake

cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS BUILD_DIR TARGET COMPILER COMPILER_VERSION EXPECTED DIAGNOSTIC)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "expect-import-then-include: -D${argument}=... is required")
    endif()
endforeach()
if(NOT EXPECTED MATCHES "^(works|fails)$")
    message(FATAL_ERROR "expect-import-then-include: EXPECTED is `works` or `fails`, not `${EXPECTED}`")
endif()

set(configArguments "")
if(CONFIG)
    set(configArguments --config "${CONFIG}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET}" ${configArguments}
                OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
set(compiler "${COMPILER} ${COMPILER_VERSION}")

if(result EQUAL 0)
    if(EXPECTED STREQUAL "fails")
        message(FATAL_ERROR "import, then include now works on ${compiler}: update D7's table (the ${COMPILER} row "
                            "of ISOLINE_MODULE_IMPORT_THEN_INCLUDE and docs/modules.md)")
    endif()
    message(STATUS "import, then include works on ${compiler}, as its row says")
    return()
endif()
if(EXPECTED STREQUAL "works")
    message(FATAL_ERROR "import, then include no longer builds on ${compiler}, whose row says it works:\n${output}")
endif()
# The build's colours (-fdiagnostics-color) may split a name, so the diagnostic is looked for in plain text.
string(ASCII 27 escape)
string(REGEX REPLACE "${escape}\\[[0-9;]*[mK]" "" output "${output}")
if(NOT output MATCHES "${DIAGNOSTIC}")
    message(FATAL_ERROR "import, then include fails on ${compiler}, but without its row's diagnostic "
                        "`${DIAGNOSTIC}`:\n${output}")
endif()
message(STATUS "import, then include fails on ${compiler} with `${DIAGNOSTIC}`, as its row says")
