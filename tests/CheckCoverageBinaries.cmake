# SPDX-License-Identifier: Apache-2.0
#
# Checks the list of binaries the coverage report reads (cmake/IsolineCoverage.cmake):
#
#   cmake -DLIST=<build>/coverage-binaries.txt "-DREQUIRED=<path>;..." -P CheckCoverageBinaries.cmake
#
# Every line names a binary the build made, none twice, and each REQUIRED binary is among them: a binary
# that drops out of the list drops its code out of the coverage ratchet without failing it.

cmake_minimum_required(VERSION 3.25)

if(NOT EXISTS "${LIST}")
    message(FATAL_ERROR "CheckCoverageBinaries: ${LIST} does not exist")
endif()
file(STRINGS "${LIST}" binaries)
set(problems "")
if(NOT binaries)
    string(APPEND problems "\n  the list is empty")
endif()
set(seen "")
foreach(binary IN LISTS binaries)
    if(NOT EXISTS "${binary}")
        string(APPEND problems "\n  ${binary}: listed, but not built")
    endif()
    if(binary IN_LIST seen)
        string(APPEND problems "\n  ${binary}: listed twice")
    endif()
    list(APPEND seen "${binary}")
endforeach()
foreach(binary IN LISTS REQUIRED)
    if(NOT binary IN_LIST binaries)
        string(APPEND problems "\n  ${binary}: not listed")
    endif()
endforeach()
if(problems)
    message(FATAL_ERROR "CheckCoverageBinaries:${problems}")
endif()
list(LENGTH binaries count)
message(STATUS "CheckCoverageBinaries: ${count} binaries, the required ones among them")
