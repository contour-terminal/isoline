# SPDX-License-Identifier: Apache-2.0
#
# contracts-expansion: preprocesses tests/contracts-consumer/Expansion.cpp through the installed
# <isoline/Contracts.hpp> (INSTALLED_DIR) and through the proving one (PROOF_DIR), and fails unless both
# expand ISOLINE_PRE, ISOLINE_POST and ISOLINE_DECLARATION_CONTRACTS alike, the proof's noexcept proof aside.
#
#   cmake -DCXX_COMPILER=<c++> -DSOURCE=<Expansion.cpp> -DINSTALLED_DIR=<dir> -DPROOF_DIR=<dir> -P contracts-expansion.cmake
cmake_minimum_required(VERSION 3.25)
foreach(required IN ITEMS CXX_COMPILER SOURCE INSTALLED_DIR PROOF_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "contracts-expansion: -D${required}=... is required")
    endif()
endforeach()

## Sets @p outVar to what @p SOURCE expands to after its marker, through the header under @p directory,
## its whitespace collapsed.
function(expansion directory outVar)
    execute_process(COMMAND "${CXX_COMPILER}" -std=c++26 -E -P "-I${directory}" "${SOURCE}"
                    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "contracts-expansion: preprocessing through ${directory} failed:\n${errors}")
    endif()
    string(FIND "${output}" "isoline_expansion_begin" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "contracts-expansion: no marker in what ${directory} expands to")
    endif()
    string(SUBSTRING "${output}" ${at} -1 output)
    string(REGEX REPLACE "[ \t\r\n]+" " " output "${output}")
    string(STRIP "${output}" output)
    set(${outVar} "${output}" PARENT_SCOPE)
endfunction()

expansion("${INSTALLED_DIR}" installed)
expansion("${PROOF_DIR}" proven)
if(NOT installed STREQUAL proven)
    message(FATAL_ERROR "contracts-expansion: the proof expands otherwise than the installed header:\n"
                        "  installed: ${installed}\n  proof:     ${proven}")
endif()
message(STATUS "contracts-expansion: ${installed}")
