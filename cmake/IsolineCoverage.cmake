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
