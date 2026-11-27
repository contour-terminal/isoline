# SPDX-License-Identifier: Apache-2.0
#
# The platform row of a build (src/isoline/platform/PlatformTable.hpp), as data: which (operating system,
# architecture) the compiler targets names one row, and that row names the implementation file of each
# platform seam. src/isoline/platform/CMakeLists.txt calls it with what the compiler's predefined macros
# say; tests/platform/UnknownPlatformConfigure.cmake calls it with forced pairs, in script mode.
include_guard(GLOBAL)

# The OperatingSystem and Architecture enumerators (src/isoline/platform/Platform.hpp) a pair may name; any
# other architecture is Other, and any other system has no row.
set(ISOLINE_PLATFORM_SYSTEMS Linux MacOS FreeBSD Windows)
set(ISOLINE_PLATFORM_ARCHITECTURES X64 Arm64)

# The implementation file of each seam, one row per (seam, system, architecture, file), `*` matching any
# system or architecture; a seam's first matching row is the build's. `core` files build into isoline_core,
# `test` files into isoline_test. Relative to src/isoline/platform/.
set(ISOLINE_PLATFORM_SOURCE_ROWS
    "core|VirtualMemory|Windows|*|VirtualMemoryWindows.cpp"
    "core|VirtualMemory|*|*|VirtualMemoryPosix.cpp"
    "core|HardwareCapabilities|Linux|Arm64|HardwareCapabilitiesLinuxArm64.cpp"
    "core|HardwareCapabilities|*|*|HardwareCapabilitiesNone.cpp"
    "core|JitMemory|MacOS|*|JitMemoryMacOS.cpp"
    "core|JitMemory|*|*|JitMemoryNone.cpp"
    "core|InstructionCache|Linux|Arm64|InstructionCacheClearCacheArm64.cpp"
    "core|InstructionCache|FreeBSD|Arm64|InstructionCacheClearCacheArm64.cpp"
    "core|InstructionCache|MacOS|Arm64|InstructionCacheMacOSArm64.cpp"
    "core|InstructionCache|Windows|Arm64|InstructionCacheWindowsArm64.cpp"
    "core|InstructionCache|*|*|InstructionCacheCoherent.cpp"
    "core|Membarrier|Linux|*|MembarrierLinux.cpp"
    "core|Membarrier|*|*|MembarrierNone.cpp"
    "test|BackedMemory|Windows|*|testing/BackedMemoryWindows.cpp"
    "test|BackedMemory|*|*|testing/BackedMemoryPosix.cpp")

## @brief The platform row of (@p system, @p architecture).
##
## isoline_platform_row(<system> <architecture> <outSystem> <outArchitecture> <outError>)
## Sets <outSystem> and <outArchitecture> to the row's enumerators: <system> itself, and <architecture>
## itself or Other when it is none of ISOLINE_PLATFORM_ARCHITECTURES. A <system> without a row sets
## <outError> to the message naming the pair, and both enumerators empty; otherwise <outError> is empty.
function(isoline_platform_row system architecture outSystem outArchitecture outError)
    if(NOT system IN_LIST ISOLINE_PLATFORM_SYSTEMS)
        set(${outSystem} "" PARENT_SCOPE)
        set(${outArchitecture} "" PARENT_SCOPE)
        list(JOIN ISOLINE_PLATFORM_SYSTEMS ", " systems)
        string(CONCAT message
            "[isoline] no platform row for (${system}, ${architecture}): the operating systems with one are ${systems}. "
            "A new one is an OperatingSystem enumerator (src/isoline/platform/Platform.hpp), its rows of "
            "PlatformTable (PlatformTable.hpp) and of ISOLINE_PLATFORM_SOURCE_ROWS (cmake/IsolinePlatform.cmake)")
        set(${outError} "${message}" PARENT_SCOPE)
        return()
    endif()
    set(${outSystem} "${system}" PARENT_SCOPE)
    if(architecture IN_LIST ISOLINE_PLATFORM_ARCHITECTURES)
        set(${outArchitecture} "${architecture}" PARENT_SCOPE)
    else()
        set(${outArchitecture} Other PARENT_SCOPE)
    endif()
    set(${outError} "" PARENT_SCOPE)
endfunction()

## @brief isoline_platform_row(), failing the configure for a system without a row.
##
## isoline_require_platform_row(<system> <architecture> <outSystem> <outArchitecture>)
function(isoline_require_platform_row system architecture outSystem outArchitecture)
    isoline_platform_row("${system}" "${architecture}" rowSystem rowArchitecture error)
    if(error)
        message(FATAL_ERROR "${error}")
    endif()
    set(${outSystem} "${rowSystem}" PARENT_SCOPE)
    set(${outArchitecture} "${rowArchitecture}" PARENT_SCOPE)
endfunction()

## @brief The implementation files of one row's seams.
##
## isoline_platform_sources(<kind> <system> <architecture> <outVar>)
## Sets <outVar> to the files of kind <kind> (core or test) that ISOLINE_PLATFORM_SOURCE_ROWS selects for
## the row (<system>, <architecture>): one per seam, its first matching row.
function(isoline_platform_sources kind system architecture outVar)
    set(seams "")
    set(sources "")
    foreach(row IN LISTS ISOLINE_PLATFORM_SOURCE_ROWS)
        string(REPLACE "|" ";" fields "${row}")
        list(GET fields 0 rowKind)
        list(GET fields 1 seam)
        list(GET fields 2 rowSystem)
        list(GET fields 3 rowArchitecture)
        list(GET fields 4 source)
        if(NOT rowKind STREQUAL kind OR seam IN_LIST seams)
            continue()
        endif()
        if((rowSystem STREQUAL "*" OR rowSystem STREQUAL system)
           AND (rowArchitecture STREQUAL "*" OR rowArchitecture STREQUAL architecture))
            list(APPEND seams "${seam}")
            list(APPEND sources "${source}")
        endif()
    endforeach()
    set(${outVar} "${sources}" PARENT_SCOPE)
endfunction()
