# SPDX-License-Identifier: Apache-2.0
#
# What an installed isoline asks of its consumer's toolchain (cmake/isolineConsumerToolchain.cmake), in
# script mode:
#
#   cmake -P tests/cmake/install-consumer-toolchain.cmake
#
# Each row is a package (its contracts path and the compiler that built it), a consumer's compiler (empty:
# C++ not enabled), the components it asks for (`,`-separated; the package's C components are `c`), and the
# refusal expected: a regular expression, or empty where the consumer links.

cmake_minimum_required(VERSION 3.25)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/isolineConsumerToolchain.cmake")

# contractsPath|builtId|builtVersion|consumerId|consumerVersion|components|expectedRefusal
set(toolchainCases
    "native|GNU|16.2.1|GNU|16.2.1|core|"
    "native|GNU|16.2.1|GNU|16.2.0|core|"
    "native|GNU|16.2.0|GNU|16.3.0|core|GNU 16[.]3 cannot link it .* compile with GNU 16[.]2, or use an isoline built with ISOLINE_CONTRACTS=OFF$"
    "native|GNU|16.2.1|GNU|17.1.0|core|GNU 17[.]1 cannot link it"
    "native|GNU|16.2.1|GNU|14.2.0|core|GNU 14[.]2 cannot link it"
    "native|GNU|16.2.1|Clang|22.1.8|core|^isoline was built with native C[+][+]26 contracts by GNU 16[.]2, so it links libstdc[+][+]exp, whose ABI is that release's alone, and Clang 22[.]1 cannot link it [(]libc[+][+] and the MSVC STL have no libstdc[+][+]exp[)]"
    "native|GNU|16.2.1|AppleClang|17.0.0|core|AppleClang 17[.]0 cannot link it"
    "native|GNU|16.2.1|MSVC|19.44.35217|core|MSVC 19[.]44 cannot link it"
    "native|GNU|16.2.1|||core|enable C[+][+] before find_package[(]isoline[)] and compile with GNU 16[.]2"
    "fallback|GNU|14.2.0|Clang|22.1.8|core|"
    "fallback|Clang|22.1.8|GNU|16.2.1|core|"
    "fallback|MSVC|19.44.35217|||core|"
    # The C API alone: a C program, or a C++ one of any compiler, links the shared library, which holds
    # libstdc++exp; asking for a C++ component beside it is asked the C++ toolchain again.
    "native|GNU|16.2.1|||c|"
    "native|GNU|16.2.1|Clang|22.1.8|c|"
    "native|GNU|16.2.1|||c,core|enable C[+][+] before find_package[(]isoline[)]"
    "native|GNU|16.2.1|Clang|22.1.8|core,c|Clang 22[.]1 cannot link it")

set(failures "")
set(caseCount 0)
foreach(row IN LISTS toolchainCases)
    string(REPLACE "|" ";" columns "${row}")
    list(GET columns 0 contractsPath)
    list(GET columns 1 builtId)
    list(GET columns 2 builtVersion)
    list(GET columns 3 consumerId)
    list(GET columns 4 consumerVersion)
    list(GET columns 5 components)
    string(REPLACE "," ";" components "${components}")
    list(GET columns 6 expected)
    isoline_consumer_toolchain_refusal(CONTRACTS_PATH "${contractsPath}" BUILT_ID "${builtId}"
                                       BUILT_VERSION "${builtVersion}" CONSUMER_ID "${consumerId}"
                                       CONSUMER_VERSION "${consumerVersion}" COMPONENTS ${components}
                                       C_COMPONENTS c OUT refusal)
    math(EXPR caseCount "${caseCount} + 1")
    if(expected STREQUAL "")
        if(NOT refusal STREQUAL "")
            string(APPEND failures "\n  ${row}: refused (${refusal}), but the consumer links")
        endif()
    elseif(NOT refusal MATCHES "${expected}")
        string(APPEND failures "\n  ${row}: '${refusal}' does not match '${expected}'")
    endif()
endforeach()

if(failures)
    message(FATAL_ERROR "consumer toolchain cases failed:${failures}")
endif()
message(STATUS "consumer toolchain: ${caseCount} cases pass")
