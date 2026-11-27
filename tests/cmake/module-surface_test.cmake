# SPDX-License-Identifier: Apache-2.0
#
# The self-test of cmake/IsolineModuleSurface.cmake's decision, run in script mode:
#     cmake -P tests/cmake/module-surface_test.cmake
# It drives isoline_module_surface_decision() with one row per case and reports every case whose outcome
# differs, so a broken precedence names each case it breaks.

cmake_minimum_required(VERSION 3.25)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/IsolineModuleSurface.cmake")

# Decision rows:
#     expected|reflectionPath|python|lintClang|compilerId|verdict|reason
# expected is ISOLINE_EXPECT_MODULE_SURFACE; python is whether Python 3 was found; lintClang is the export
# lint's Clang, empty where there is none. verdict is register, off (a STATUS line) or refused (the configure's
# FATAL_ERROR); reason is a regular expression the reason must match, empty where the test registers.
set(decisionRows
    "OFF|native|TRUE|/usr/bin/clang++-22|GNU|register|"
    "ON|native|TRUE|/usr/bin/clang++-22|GNU|register|"
    "OFF|fallback|TRUE|/usr/bin/clang++-22|GNU|off|^reflection takes the fallback$"
    "OFF|native|FALSE|/usr/bin/clang++-22|GNU|off|^no Python 3$"
    "OFF|native|TRUE||GNU|off|^no export lint [(]ISOLINE_MODULE_LINT_CLANG[)]$"
    "OFF|native|TRUE|/usr/bin/clang++-22|Clang|off|^the compiler is Clang, as the export lint's is"
    "ON|fallback|TRUE|/usr/bin/clang++-22|GNU|refused|^ISOLINE_EXPECT_MODULE_SURFACE=ON, but meta-module-surface[.]reflection cannot run: reflection takes the fallback$"
    "ON|native|FALSE|/usr/bin/clang++-22|GNU|refused|: no Python 3$"
    "ON|native|TRUE||GNU|refused|: no export lint [(]ISOLINE_MODULE_LINT_CLANG[)]$"
    "ON|native|TRUE|/usr/bin/clang++-22|Clang|refused|: the compiler is Clang"
    # The precedence, fallback > same compiler > Python > lint: the first reason that holds is the one given.
    "OFF|fallback|FALSE||Clang|off|^reflection takes the fallback$"
    "OFF|native|FALSE||Clang|off|^the compiler is Clang"
    "OFF|native|FALSE||GNU|off|^no Python 3$"
    # CMake's boolean spellings are ON and OFF.
    "yes|native|TRUE||GNU|refused|^ISOLINE_EXPECT_MODULE_SURFACE=ON, but"
    "0|native|TRUE||GNU|off|^no export lint")
set(failures "")
set(caseCount 0)
foreach(row IN LISTS decisionRows)
    string(REPLACE "|" ";" columns "${row}")
    list(LENGTH columns columnCount)
    if(columnCount LESS 7)
        list(APPEND columns "")
    endif()
    list(GET columns 0 expected)
    list(GET columns 1 reflectionPath)
    list(GET columns 2 python)
    list(GET columns 3 lintClang)
    list(GET columns 4 compilerId)
    list(GET columns 5 expectedVerdict)
    list(GET columns 6 expectedReason)
    isoline_module_surface_decision(EXPECTED "${expected}" REFLECTION_PATH "${reflectionPath}" PYTHON_FOUND "${python}"
                                    LINT_CLANG "${lintClang}" COMPILER_ID "${compilerId}"
                                    OUT_VERDICT verdict OUT_REASON reason)
    math(EXPR caseCount "${caseCount} + 1")
    if(NOT verdict STREQUAL expectedVerdict
       OR (expectedReason STREQUAL "" AND NOT reason STREQUAL "")
       OR NOT reason MATCHES "${expectedReason}")
        string(APPEND failures "\n  ${row}\n    got ${verdict}: '${reason}'")
    endif()
endforeach()

if(NOT failures STREQUAL "")
    message(FATAL_ERROR "module-surface: rows whose outcome differs:${failures}")
endif()
message(STATUS "module-surface: all ${caseCount} rows hold")
