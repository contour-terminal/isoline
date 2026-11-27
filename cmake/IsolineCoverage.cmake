# SPDX-License-Identifier: Apache-2.0
#
# The binaries the coverage report reads: every test executable, and the shared library that holds code
# no test executable links (the C API's, which the C tests reach through libisoline_c). The report merges
# the profiles of all of them, so a source file's coverage is what any of them reached.
#
# The list is generated into <build>/coverage-binaries.txt, one path per line, the first the report's main
# object and the rest its -object arguments. CI's coverage step (.github/workflows/build.yml) and the local
# gate read it from there, so a new binary is a call of isoline_coverage_binary(), never an edit of either.
# A multi-config generator (Visual Studio) writes one list per configuration,
# <build>/coverage-binaries-<config>.txt, since each configuration's binaries are files of their own; the
# coverage build is single-config Ninja, whose list keeps the plain name.
# isoline_apply_target_policy(<target> TEST) makes that call for every test executable; a library calls
# it itself.

include_guard(GLOBAL)

# Where the list is written, for its writer and for the coverage-binaries test that reads it.
get_property(_isolineCoverageIsMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_isolineCoverageIsMultiConfig)
    set(ISOLINE_COVERAGE_BINARIES_FILE "${PROJECT_BINARY_DIR}/coverage-binaries-$<CONFIG>.txt")
else()
    set(ISOLINE_COVERAGE_BINARIES_FILE "${PROJECT_BINARY_DIR}/coverage-binaries.txt")
endif()

## @brief Adds @p target, an executable or a shared library, to the binaries the coverage report reads.
function(isoline_coverage_binary target)
    set_property(GLOBAL APPEND PROPERTY ISOLINE_COVERAGE_BINARIES ${target})
endfunction()

## @brief Writes ISOLINE_COVERAGE_BINARIES_FILE: the path of each binary isoline_coverage_binary() added, one
## per line, in the order they were added. Called once, after every directory has added its binaries.
function(isoline_write_coverage_binaries)
    get_property(binaries GLOBAL PROPERTY ISOLINE_COVERAGE_BINARIES)
    if(NOT binaries)
        message(FATAL_ERROR "isoline_write_coverage_binaries: no binary was added")
    endif()
    set(content "")
    foreach(binary IN LISTS binaries)
        string(APPEND content "$<TARGET_FILE:${binary}>\n")
    endforeach()
    file(GENERATE OUTPUT "${ISOLINE_COVERAGE_BINARIES_FILE}" CONTENT "${content}")
endfunction()

## The files the report leaves out (llvm-cov's -ignore-filename-regex): everything but Isoline's own
## sources, the tests' sources and test support included.
set(ISOLINE_COVERAGE_IGNORE_REGEX "vendor/|_deps/|^/usr/|_test\\.cpp$|/out/|/tests/|/testing/")

## @brief Adds isoline_coverage_check to a coverage build (ISOLINE_COVERAGE with ISOLINE_TESTING): after the
## tests ran under the clang-coverage test preset, which writes each process's profile to profiles/ of the
## build tree,
##
##     cmake --build --preset clang-coverage --target isoline_coverage_check
##
## merges the profiles, writes the report (coverage-summary.txt, coverage.json, coverage.lcov and
## coverage-html/ in the build tree) over the binaries of ISOLINE_COVERAGE_BINARIES_FILE, leaving out
## ISOLINE_COVERAGE_IGNORE_REGEX, and holds it to coverage-baseline.txt (scripts/coverage-check.py). CI runs
## the same target, so a local run and CI's measure the same thing. Called once, after
## isoline_write_coverage_binaries().
function(isoline_add_coverage_check)
    if(NOT ISOLINE_COVERAGE OR NOT ISOLINE_TESTING)
        return()
    endif()
    # The LLVM tools of the compiler's own release, beside it or versioned on the PATH (clang++-22 pairs with
    # llvm-cov-22): a profile is read by the release that wrote it.
    string(REGEX MATCH "^[0-9]+" llvmMajor "${CMAKE_CXX_COMPILER_VERSION}")
    get_filename_component(compilerDirectory "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(ISOLINE_LLVM_PROFDATA NAMES "llvm-profdata-${llvmMajor}" llvm-profdata HINTS "${compilerDirectory}"
                 REQUIRED)
    find_program(ISOLINE_LLVM_COV NAMES "llvm-cov-${llvmMajor}" llvm-cov HINTS "${compilerDirectory}" REQUIRED)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(profileDirectory "${PROJECT_BINARY_DIR}/profiles")
    add_custom_target(isoline_coverage_check
        COMMAND "${CMAKE_COMMAND}" "-DPROFDATA=${ISOLINE_LLVM_PROFDATA}" "-DCOV=${ISOLINE_LLVM_COV}"
                "-DPYTHON=${Python3_EXECUTABLE}" "-DOBJECTS_FILE=${ISOLINE_COVERAGE_BINARIES_FILE}"
                "-DIGNORE_REGEX=${ISOLINE_COVERAGE_IGNORE_REGEX}" "-DPROFILE_DIR=${profileDirectory}"
                "-DOUTPUT_DIR=${PROJECT_BINARY_DIR}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
                -P "${PROJECT_SOURCE_DIR}/cmake/IsolineCoverageReport.cmake"
        COMMENT "Reporting coverage and holding it to coverage-baseline.txt"
        VERBATIM)
    message(STATUS "[isoline] coverage: isoline_coverage_check reports ${profileDirectory} with ${ISOLINE_LLVM_COV}")
endfunction()
