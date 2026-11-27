# SPDX-License-Identifier: Apache-2.0
#
# Cross-compiles Windows x64 executables on a Linux host with Microsoft's own compiler: msvc-wine's
# cl.exe, link.exe and lib.exe (https://github.com/mstorsjo/msvc-wine), run under wine. Unlike clang-cl
# (windows-clangcl-cross.cmake), this is the MSVC that CMake scans C++ modules for, which makes it the
# local lane of the msvc-wine-modules preset.
#
# msvc-wine's wrappers (<MSVC_WINE_ROOT>/bin/x64/cl, ...) set INCLUDE and LIB themselves, so no system
# directory is passed here. They would run in the user's default wine prefix; each is called through a
# runner of this build instead (<build>/msvc-wine-bin/<tool>), which runs it in the build's own prefix,
# the one the tests run in. The presets link the static CRT (CMAKE_MSVC_RUNTIME_LIBRARY), so a test
# binary needs no MSVC runtime DLL under wine.
#
# The install, its versions, the prefix and the test emulator: cmake/toolchains/msvc-wine-common.cmake.
#
# @see docs/cross-compiling.md

include("${CMAKE_CURRENT_LIST_DIR}/msvc-wine-common.cmake")

set(_msvcWineBin "${MSVC_WINE_ROOT}/bin/x64")
if(NOT EXISTS "${_msvcWineBin}/cl")
    message(FATAL_ERROR "${_msvcWineBin}/cl does not exist: MSVC_WINE_ROOT=${MSVC_WINE_ROOT} has no msvc-wine "
                        "wrappers (msvc-wine's install.sh writes them)")
endif()

set(MSVC_WINE_TOOL_DIRECTORY "${CMAKE_BINARY_DIR}/msvc-wine-bin" CACHE PATH
    "Where the runners of msvc-wine's tools for this build are written")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MSVC_WINE_TOOL_DIRECTORY)

# The tools CMake calls: (msvc-wine wrapper, CMake variable).
set(_msvcWineTools
    "cl"   "CMAKE_CXX_COMPILER"
    "cl"   "CMAKE_C_COMPILER"
    "link" "CMAKE_LINKER"
    "lib"  "CMAKE_AR"
    "rc"   "CMAKE_RC_COMPILER"
    "mt"   "CMAKE_MT")
set(_rows "${_msvcWineTools}")
while(_rows)
    list(POP_FRONT _rows _tool _variable)
    set(_runner "${MSVC_WINE_TOOL_DIRECTORY}/${_tool}")
    msvc_wine_write_runner("${_runner}" "${_msvcWineBin}/${_tool}" "msvc-wine's ${_tool}")
    set(${_variable} "${_runner}")
endwhile()

# Debug information goes into the objects (/Z7), not through a PDB server (/Zi). Under wine the first /Zi
# compile of a fresh prefix fails with C1902 ("Program database manager mismatch") before mspdbsrv.exe is
# up (measured with cl 19.51 on the configure's compiler check, every time on a new prefix); /Z7 needs
# no server, and a parallel build then shares nothing between compiles.
if(NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
endif()
