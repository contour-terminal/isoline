# SPDX-License-Identifier: Apache-2.0
#
# What an installed isoline asks of its consumer's toolchain, in one place for both sides: the build
# records the contracts path and its compiler in isolineConfig.cmake, and the installed package, beside
# which this file is installed, refuses a consumer that could not link it, at the consumer's configure
# rather than as an undefined handle_contract_violation at its link (docs/language-features.md,
# "The consumer side of a native package").
#
# A package whose contracts are native links libstdc++exp, the GCC library that holds the contract
# violation handler. Its ABI is that GCC release's alone, and no other standard library (libc++, the MSVC
# STL) has it, so the consumer compiles with the same compiler at the same major.minor release. A package
# built on the fallback asks nothing.
#
# Pure: no target, no cache entry, so it is testable in script mode
# (tests/cmake/install-consumer-toolchain.cmake).

include_guard(GLOBAL)

# An empty keyword value (a consumer without C++) is an empty string, not an unset variable. CMake before
# 3.31 has no such policy: every comparison below quotes its expansion, so both read the same.
if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()

## @brief Sets OUT to why a consumer compiling with CONSUMER_ID CONSUMER_VERSION cannot link a package
## whose contracts took CONTRACTS_PATH and that BUILT_ID BUILT_VERSION built, or to "" when it can.
## An empty CONSUMER_ID is a consumer that has not enabled C++.
##
## isoline_consumer_toolchain_refusal(CONTRACTS_PATH <native|fallback> BUILT_ID <id> BUILT_VERSION <version>
##                                    CONSUMER_ID <id> CONSUMER_VERSION <version> OUT <var>)
function(isoline_consumer_toolchain_refusal)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "CONTRACTS_PATH;BUILT_ID;BUILT_VERSION;CONSUMER_ID;CONSUMER_VERSION;OUT"
                          "")
    set(refusal "")
    if("${arg_CONTRACTS_PATH}" STREQUAL "native")
        string(REGEX MATCH "^[0-9]+[.][0-9]+" built "${arg_BUILT_VERSION}")
        string(REGEX MATCH "^[0-9]+[.][0-9]+" consumer "${arg_CONSUMER_VERSION}")
        set(why "isoline was built with native C++26 contracts by ${arg_BUILT_ID} ${built}, so it links "
                "libstdc++exp, whose ABI is that release's alone")
        set(remedy "or use an isoline built with ISOLINE_CONTRACTS=OFF")
        if("${arg_CONSUMER_ID}" STREQUAL "")
            string(CONCAT refusal ${why} ": enable C++ before find_package(isoline) and compile with "
                                  "${arg_BUILT_ID} ${built}, " ${remedy})
        elseif(NOT "${arg_CONSUMER_ID}" STREQUAL "${arg_BUILT_ID}" OR NOT "${consumer}" STREQUAL "${built}")
            string(CONCAT refusal ${why} ", and ${arg_CONSUMER_ID} ${consumer} cannot link it (libc++ and the "
                                  "MSVC STL have no libstdc++exp): compile with ${arg_BUILT_ID} ${built}, " ${remedy})
        endif()
    endif()
    set(${arg_OUT} "${refusal}" PARENT_SCOPE)
endfunction()
