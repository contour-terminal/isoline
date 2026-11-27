# SPDX-License-Identifier: Apache-2.0
#
# Cross-compiles Windows x64 executables on a Linux host: clang-cl, lld-link and the llvm-* tools from the
# host's LLVM, against the MSVC headers and libraries and the Windows SDK of an msvc-wine style install
# (https://github.com/mstorsjo/msvc-wine).
#
# The install (MSVC_WINE_ROOT, default /opt/msvc), its versions, and the wine prefix of the build's own
# that the tests run in (CMAKE_CROSSCOMPILING_EMULATOR): cmake/toolchains/msvc-wine-common.cmake.
#
# Adapted from Contour's cmake/toolchains/windows-clangcl-cross.cmake, without its Qt part.
#
# @see docs/cross-compiling.md

include("${CMAKE_CURRENT_LIST_DIR}/msvc-wine-common.cmake")

find_program(CMAKE_C_COMPILER NAMES clang-cl REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang-cl REQUIRED)
find_program(CMAKE_LINKER NAMES lld-link REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES llvm-rc REQUIRED)
find_program(CMAKE_MT NAMES llvm-mt REQUIRED)
find_program(CMAKE_AR NAMES llvm-lib REQUIRED)

# The system headers are -imsvc, so the pedantic warnings stay on Isoline's own code, as on Windows.
set(_includes "")
foreach(_dir IN ITEMS "${_msvc}/include" "${_sdkInclude}/ucrt" "${_sdkInclude}/shared" "${_sdkInclude}/um"
                      "${_sdkInclude}/winrt")
    string(APPEND _includes " /imsvc \"${_dir}\"")
endforeach()
# _MSC_VER is the toolset's, not clang-cl's default (1933, VS 2022 17.3): the STL and other headers decide
# by it. Toolset 14.XY is _MSC_VER 19XY.
string(REGEX REPLACE "^14\\.([0-9]+).*$" "19.\\1" _msvcCompatibilityVersion "${MSVC_WINE_TOOLSET_VERSION}")
set(_target "--target=x86_64-pc-windows-msvc -fuse-ld=lld-link")
string(APPEND _target " -fms-compatibility-version=${_msvcCompatibilityVersion}")
set(CMAKE_C_FLAGS_INIT "${_target}${_includes}")
set(CMAKE_CXX_FLAGS_INIT "${_target}${_includes}")
set(CMAKE_RC_FLAGS_INIT "/I \"${_sdkInclude}/um\" /I \"${_sdkInclude}/shared\"")

set(_libpaths "")
foreach(_dir IN ITEMS "${_msvc}/lib/x64" "${_sdkLib}/ucrt/x64" "${_sdkLib}/um/x64")
    string(APPEND _libpaths " /libpath:\"${_dir}\"")
endforeach()
foreach(_kind IN ITEMS EXE SHARED MODULE)
    set(CMAKE_${_kind}_LINKER_FLAGS_INIT "${_libpaths}")
endforeach()
