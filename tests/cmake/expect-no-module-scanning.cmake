# SPDX-License-Identifier: Apache-2.0
#
# The refusal tripwire of a compiler the module support table refuses because CMake cannot scan its
# module dependencies (Apple Clang, clang-cl): configures, and where that succeeds builds, the probe
# project tests/cmake/module-scanning-probe/ with that compiler, and passes only while CMake refuses it
# with a module-scanning diagnostic. It probes CMake, not Isoline: the probe does not configure Isoline,
# so ISOLINE_MODULES and the table play no part.
#
#   cmake -DPROBE_BINARY_DIR=<dir> -DINITIAL_CACHE=<file> -DGENERATOR=<generator>
#         [-DGENERATOR_PLATFORM=<platform>] [-DGENERATOR_TOOLSET=<toolset>] -DCOMPILER=<id version>
#         -P tests/cmake/expect-no-module-scanning.cmake
#
# INITIAL_CACHE (a `cmake -C` script) carries the build's toolchain over: its toolchain file, compiler and
# build tool (ISOLINE_MODULE_PROBE_CACHE_VARIABLES in cmake/IsolineModules.cmake).
#
# Any other failure fails the tripwire, so a broken probe never passes for a refusal. A probe that builds
# fails it too: CMake then scans modules for that compiler, and its row should become a lane.

cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS PROBE_BINARY_DIR INITIAL_CACHE GENERATOR COMPILER)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "expect-no-module-scanning: -D${argument}=... is required")
    endif()
endforeach()

# What CMake says where it cannot scan a target's module dependencies: one row per wording.
set(scanningDiagnostics
    "the compiler does not provide a way to discover the import graph dependencies")

set(probeSource "${CMAKE_CURRENT_LIST_DIR}/module-scanning-probe")
set(generatorArguments -G "${GENERATOR}")
if(GENERATOR_PLATFORM)
    list(APPEND generatorArguments -A "${GENERATOR_PLATFORM}")
endif()
if(GENERATOR_TOOLSET)
    list(APPEND generatorArguments -T "${GENERATOR_TOOLSET}")
endif()

## @brief Passes where @p output holds a row of scanningDiagnostics, and fails the script otherwise, naming
## the @p step that failed and its output. CMake wraps a diagnostic across lines, so whitespace is folded
## first.
function(_expect_scanning_refusal step output)
    string(REGEX REPLACE "[ \t\r\n]+" " " foldedOutput "${output}")
    foreach(diagnostic IN LISTS scanningDiagnostics)
        string(FIND "${foldedOutput}" "${diagnostic}" position)
        if(NOT position EQUAL -1)
            message(STATUS "module-scanning-tripwire: passed: CMake ${CMAKE_VERSION} refuses to scan the "
                           "module dependencies of ${COMPILER} (${step}: ${diagnostic})")
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "module-scanning-tripwire: the probe's ${step} failed, but not for module scanning, "
                        "so it says nothing about CMake's support for ${COMPILER}:\n${output}")
endfunction()

file(REMOVE_RECURSE "${PROBE_BINARY_DIR}")
execute_process(COMMAND "${CMAKE_COMMAND}" -C "${INITIAL_CACHE}" ${generatorArguments}
                        -S "${probeSource}" -B "${PROBE_BINARY_DIR}"
                RESULT_VARIABLE configureStatus OUTPUT_VARIABLE configureOutput ERROR_VARIABLE configureOutput)
if(NOT configureStatus EQUAL 0)
    _expect_scanning_refusal(configure "${configureOutput}")
    return()
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${PROBE_BINARY_DIR}" --config Debug
                RESULT_VARIABLE buildStatus OUTPUT_VARIABLE buildOutput ERROR_VARIABLE buildOutput)
if(NOT buildStatus EQUAL 0)
    _expect_scanning_refusal(build "${buildOutput}")
    return()
endif()

message(FATAL_ERROR "module-scanning-tripwire: CMake ${CMAKE_VERSION} now scans modules for ${COMPILER}: "
                    "turn its refused row in cmake/IsolineModuleSupport.cmake into a lane")
