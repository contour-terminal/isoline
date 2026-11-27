# SPDX-License-Identifier: Apache-2.0
#
# C++ modules, opt-in: ISOLINE_MODULES, default OFF.
#
# This is the gate. With ISOLINE_MODULES ON it feeds the real toolchain to the support table
# (cmake/IsolineModuleSupport.cmake) and stops the configure on a toolchain the table refuses, naming
# why; beside a module build it also holds clang-tidy to the compiler's own LLVM build. With
# ISOLINE_MODULES OFF it changes nothing about the build: no target, no flag. Either way it knows the
# tripwire of a refused compiler, which tests/CMakeLists.txt registers (isoline_add_module_tripwires()).

include_guard(GLOBAL)
include(IsolineModuleSupport)

# The target ABI the table's rows name (isoline_module_target_abi()).
isoline_module_target_abi(COMPILER_ID "${CMAKE_CXX_COMPILER_ID}" SIMULATE_ID "${CMAKE_CXX_SIMULATE_ID}"
                          OUT_TARGET_ABI ISOLINE_CXX_TARGET_ABI)

# The tripwires a refused compiler row names: (tripwire, CTest name, script). Each probes CMake itself,
# not the table, and goes red once CMake builds modules for that compiler: the row should then become a
# lane.
set(ISOLINE_MODULE_TRIPWIRES
    "scanning-probe" "module-scanning-tripwire" "tests/cmake/expect-no-module-scanning.cmake")

# The cache variables a tripwire's probe project takes over from this build, so that it configures the
# same toolchain: the compiler and its toolchain file, the build tool, and what the msvc-wine toolchain
# files read.
set(ISOLINE_MODULE_PROBE_CACHE_VARIABLES
    CMAKE_TOOLCHAIN_FILE
    CMAKE_CXX_COMPILER
    CMAKE_MAKE_PROGRAM
    MSVC_WINE_ROOT
    MSVC_WINE_TOOLSET_VERSION
    MSVC_WINE_SDK_VERSION)

## @brief Refuses the configure unless the toolchain builds Isoline's C++ modules, and unless clang-tidy,
## where it runs, is the compiler's own LLVM build. Prints the verdict.
function(_isoline_module_gate)
    set(scanDepsDefined "no")
    if(DEFINED CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS)
        set(scanDepsDefined "yes")
    endif()
    # The generator's build tool is CMAKE_MAKE_PROGRAM; its row says whether it has a floor.
    isoline_module_generator_tool("${CMAKE_GENERATOR}" buildTool)
    set(buildToolOutput "")
    if(buildTool)
        execute_process(COMMAND "${CMAKE_MAKE_PROGRAM}" --version OUTPUT_VARIABLE buildToolOutput ERROR_QUIET)
    endif()
    isoline_module_scan_inputs(
        SCAN_RULE "${CMAKE_CXX_SCANDEP_SOURCE}" SCAN_DEPS_DEFINED "${scanDepsDefined}"
        SCAN_DEPS "${CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS}" BUILD_TOOL_OUTPUT "${buildToolOutput}"
        OUT_SCAN_RULE_DEFINED scanRuleDefined OUT_SCAN_DEPS_FOUND scanDepsFound
        OUT_BUILD_TOOL_VERSION buildToolVersion)
    isoline_module_support(
        COMPILER_ID "${CMAKE_CXX_COMPILER_ID}" FRONTEND "${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}"
        TARGET_ABI "${ISOLINE_CXX_TARGET_ABI}" VERSION "${CMAKE_CXX_COMPILER_VERSION}"
        GENERATOR "${CMAKE_GENERATOR}" CMAKE_VERSION "${CMAKE_VERSION}" NINJA_VERSION "${buildToolVersion}"
        SCAN_RULE_DEFINED "${scanRuleDefined}" SCAN_DEPS_FOUND "${scanDepsFound}"
        OUT_VERDICT verdict OUT_REASON reason OUT_INSTALLED_MODULES installedModules)
    if(NOT verdict STREQUAL "supported")
        message(FATAL_ERROR "[isoline] ISOLINE_MODULES: ${reason}")
    endif()

    if(ISOLINE_CLANG_TIDY)
        execute_process(COMMAND "${ISOLINE_CLANG_TIDY_EXE}" --version
                        OUTPUT_VARIABLE tidyVersionOutput ERROR_QUIET)
        isoline_module_tidy_refusal(COMPILER_ID "${CMAKE_CXX_COMPILER_ID}" VERSION "${CMAKE_CXX_COMPILER_VERSION}"
                                    TIDY_VERSION_OUTPUT "${tidyVersionOutput}" OUT_REASON tidyRefusal)
        if(tidyRefusal)
            message(FATAL_ERROR "[isoline] ISOLINE_MODULES with ISOLINE_CLANG_TIDY: ${tidyRefusal} "
                                "(ISOLINE_CLANG_TIDY_EXE=${ISOLINE_CLANG_TIDY_EXE})")
        endif()
    endif()

    message(STATUS "[isoline] C++ modules: on (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, "
                   "installed modules: ${installedModules})")
endfunction()

if(ISOLINE_MODULES)
    _isoline_module_gate()
endif()

## @brief Registers the tripwire of the compiler's row, where it is refused with one, as a CTest of label
## `lint`. Called by tests/CMakeLists.txt, whatever ISOLINE_MODULES says: the probe is a project of its own
## and does not configure Isoline, so it tests CMake, never the table.
function(isoline_add_module_tripwires)
    isoline_module_compiler_tripwire(COMPILER_ID "${CMAKE_CXX_COMPILER_ID}"
                                     FRONTEND "${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}"
                                     TARGET_ABI "${ISOLINE_CXX_TARGET_ABI}" CMAKE_VERSION "${CMAKE_VERSION}"
                                     OUT_TRIPWIRE tripwire)
    if(NOT tripwire)
        return()
    endif()
    set(rows "${ISOLINE_MODULE_TRIPWIRES}")
    set(testName "")
    while(rows)
        list(POP_FRONT rows rowTripwire rowTestName rowScript)
        if(rowTripwire STREQUAL tripwire)
            set(testName "${rowTestName}")
            set(script "${rowScript}")
            break()
        endif()
    endwhile()
    if(NOT testName)
        message(FATAL_ERROR "[isoline] ISOLINE_MODULE_TRIPWIRES has no row for the tripwire ${tripwire}")
    endif()

    set(probeDirectory "${PROJECT_BINARY_DIR}/${testName}")
    set(initialCache "")
    foreach(variable IN LISTS ISOLINE_MODULE_PROBE_CACHE_VARIABLES)
        if(DEFINED ${variable})
            string(APPEND initialCache "set(${variable} [==[${${variable}}]==] CACHE STRING \"\")\n")
        endif()
    endforeach()
    file(CONFIGURE OUTPUT "${probeDirectory}/initial-cache.cmake" CONTENT "${initialCache}" @ONLY)
    add_test(NAME ${testName}
             COMMAND "${CMAKE_COMMAND}"
                     "-DPROBE_BINARY_DIR=${probeDirectory}/build"
                     "-DINITIAL_CACHE=${probeDirectory}/initial-cache.cmake"
                     "-DGENERATOR=${CMAKE_GENERATOR}"
                     "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                     "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                     "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
                     -P "${PROJECT_SOURCE_DIR}/${script}")
    # A configure of its own, plus a build where that succeeds: seconds, but a toolchain under wine is slow
    # to start.
    set_tests_properties(${testName} PROPERTIES LABELS lint TIMEOUT 300)
endfunction()
