# SPDX-License-Identifier: Apache-2.0
#
# The platform row's selection (cmake/IsolinePlatform.cmake), in script mode:
#
#   cmake -P tests/platform/UnknownPlatformConfigure.cmake
#
# Each table is rows of inputs and the answer they must get; any row that gets another fails the script.
# The last check runs the script again (-DMODE=require) for a system without a row, and expects that
# configure to fail with the message naming the pair.

cmake_minimum_required(VERSION 3.25)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/IsolinePlatform.cmake")

if(MODE STREQUAL "require")
    isoline_require_platform_row("${SYSTEM}" "${ARCHITECTURE}" system architecture)
    message(STATUS "row: ${system}, ${architecture}")
    return()
endif()

set(failures "")

# (system, architecture → the row's system and architecture, or `-` for none).
set(rowCases
    "Linux"     "X64"      "Linux"   "X64"
    "Linux"     "Arm64"    "Linux"   "Arm64"
    "Linux"     "riscv64"  "Linux"   "Other"
    "MacOS"     "Arm64"    "MacOS"   "Arm64"
    "MacOS"     "x86_64+arm64" "MacOS" "Other"
    "FreeBSD"   "X64"      "FreeBSD" "X64"
    "Windows"   "ARM64"    "Windows" "Other"
    "Windows"   "Arm64"    "Windows" "Arm64"
    "Haiku"     "X64"      "-"       "-"
    "NetBSD"    "Arm64"    "-"       "-"
    "<unrecognized>" "X64" "-"       "-")
while(rowCases)
    list(POP_FRONT rowCases system architecture expectedSystem expectedArchitecture)
    isoline_platform_row("${system}" "${architecture}" actualSystem actualArchitecture error)
    if(expectedSystem STREQUAL "-")
        if(NOT error MATCHES "no platform row for [(]${system}, ")
            list(APPEND failures "(${system}, ${architecture}): expected a refusal naming the pair, got '${error}'")
        endif()
        if(NOT actualSystem STREQUAL "" OR NOT actualArchitecture STREQUAL "")
            list(APPEND failures "(${system}, ${architecture}): a refusal must name no row")
        endif()
    elseif(NOT error STREQUAL "" OR NOT actualSystem STREQUAL expectedSystem
           OR NOT actualArchitecture STREQUAL expectedArchitecture)
        list(APPEND failures "(${system}, ${architecture}): got (${actualSystem}, ${actualArchitecture}) '${error}', "
                             "expected (${expectedSystem}, ${expectedArchitecture})")
    endif()
endwhile()

# (kind, system, architecture → the files, `,`-separated).
set(sourceCases
    "core" "Linux"   "X64"   "VirtualMemoryPosix.cpp,HardwareCapabilitiesNone.cpp"
    "core" "Linux"   "Arm64" "VirtualMemoryPosix.cpp,HardwareCapabilitiesLinuxArm64.cpp"
    "core" "Linux"   "Other" "VirtualMemoryPosix.cpp,HardwareCapabilitiesNone.cpp"
    "core" "FreeBSD" "Arm64" "VirtualMemoryPosix.cpp,HardwareCapabilitiesNone.cpp"
    "core" "MacOS"   "Arm64" "VirtualMemoryPosix.cpp,HardwareCapabilitiesNone.cpp"
    "core" "Windows" "Arm64" "VirtualMemoryWindows.cpp,HardwareCapabilitiesNone.cpp"
    "test" "Windows" "X64"   "testing/BackedMemoryWindows.cpp"
    "test" "Linux"   "Arm64" "testing/BackedMemoryPosix.cpp")
while(sourceCases)
    list(POP_FRONT sourceCases kind system architecture expected)
    isoline_platform_sources("${kind}" "${system}" "${architecture}" actual)
    list(JOIN actual "," actual)
    if(NOT actual STREQUAL expected)
        list(APPEND failures "${kind} sources of (${system}, ${architecture}): '${actual}', expected '${expected}'")
    endif()
endwhile()

# A configure for a system without a row fails, naming the pair; one with a row passes.
foreach(case IN ITEMS "Haiku|X64|1" "Linux|riscv64|0")
    string(REPLACE "|" ";" fields "${case}")
    list(GET fields 0 system)
    list(GET fields 1 architecture)
    list(GET fields 2 expectedFailure)
    execute_process(COMMAND "${CMAKE_COMMAND}" -DMODE=require "-DSYSTEM=${system}" "-DARCHITECTURE=${architecture}"
                            -P "${CMAKE_CURRENT_LIST_FILE}"
                    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(expectedFailure)
        if(result EQUAL 0 OR NOT output MATCHES "no platform row for [(]${system}, ${architecture}[)]")
            list(APPEND failures "configure for (${system}, ${architecture}) must fail naming the pair: ${result} ${output}")
        endif()
    elseif(NOT result EQUAL 0 OR NOT output MATCHES "row: ${system}, Other")
        list(APPEND failures "configure for (${system}, ${architecture}) must pass to Other: ${result} ${output}")
    endif()
endforeach()

if(failures)
    list(JOIN failures "\n  " text)
    message(FATAL_ERROR "platform selection:\n  ${text}")
endif()
message(STATUS "platform selection: every row as expected")
