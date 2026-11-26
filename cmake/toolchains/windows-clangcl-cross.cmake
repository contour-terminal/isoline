# SPDX-License-Identifier: Apache-2.0
#
# Cross-compiles Windows x64 executables on a Linux host: clang-cl, lld-link and the llvm-* tools from the
# host's LLVM, against the MSVC headers and libraries and the Windows SDK of an msvc-wine style install
# (https://github.com/mstorsjo/msvc-wine).
#
# Where that install lives is MSVC_WINE_ROOT -- a cache variable, else the environment variable of the
# same name, else /opt/msvc. The MSVC toolset and SDK versions are the newest found there unless
# MSVC_WINE_TOOLSET_VERSION / MSVC_WINE_SDK_VERSION name one.
#
# Test binaries run under wine: CMAKE_CROSSCOMPILING_EMULATOR is set, so ctest runs them as it would on
# Windows, in a wine prefix of the build's own (MSVC_WINE_PREFIX, default <build>/wineprefix).
#
# Adapted from Contour's cmake/toolchains/windows-clangcl-cross.cmake, without its Qt part.
#
# @see docs/cross-compiling.md

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT DEFINED MSVC_WINE_ROOT)
    if(DEFINED ENV{MSVC_WINE_ROOT})
        set(MSVC_WINE_ROOT "$ENV{MSVC_WINE_ROOT}")
    else()
        set(MSVC_WINE_ROOT "/opt/msvc")
    endif()
endif()
set(MSVC_WINE_ROOT "${MSVC_WINE_ROOT}" CACHE PATH "The msvc-wine style MSVC + Windows SDK install")
if(NOT IS_DIRECTORY "${MSVC_WINE_ROOT}/vc/tools/msvc" OR NOT IS_DIRECTORY "${MSVC_WINE_ROOT}/kits/10/lib")
    message(FATAL_ERROR "MSVC_WINE_ROOT=${MSVC_WINE_ROOT} is no msvc-wine style install "
                        "(expected vc/tools/msvc/ and kits/10/lib/ beneath it)")
endif()

# Picks the newest version directory under @p parent unless @p variable already names one.
function(_msvc_wine_pick_version variable parent)
    if(${variable})
        return()
    endif()
    file(GLOB _versions LIST_DIRECTORIES true RELATIVE "${parent}" "${parent}/*")
    list(FILTER _versions INCLUDE REGEX "^[0-9.]+$")
    list(SORT _versions COMPARE NATURAL ORDER DESCENDING)
    list(GET _versions 0 _newest)
    set(${variable} "${_newest}" CACHE STRING "" FORCE)
endfunction()
_msvc_wine_pick_version(MSVC_WINE_TOOLSET_VERSION "${MSVC_WINE_ROOT}/vc/tools/msvc")
_msvc_wine_pick_version(MSVC_WINE_SDK_VERSION "${MSVC_WINE_ROOT}/kits/10/lib")

set(_msvc "${MSVC_WINE_ROOT}/vc/tools/msvc/${MSVC_WINE_TOOLSET_VERSION}")
set(_sdkInclude "${MSVC_WINE_ROOT}/kits/10/include/${MSVC_WINE_SDK_VERSION}")
set(_sdkLib "${MSVC_WINE_ROOT}/kits/10/lib/${MSVC_WINE_SDK_VERSION}")
foreach(_dir IN ITEMS "${_msvc}" "${_sdkInclude}" "${_sdkLib}")
    if(NOT IS_DIRECTORY "${_dir}")
        message(FATAL_ERROR "${_dir} does not exist: MSVC_WINE_TOOLSET_VERSION=${MSVC_WINE_TOOLSET_VERSION} / "
                            "MSVC_WINE_SDK_VERSION=${MSVC_WINE_SDK_VERSION} name no version installed in "
                            "${MSVC_WINE_ROOT}")
    endif()
endforeach()

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

# Windows binaries run under wine, with Microsoft's own C++ runtime rather than wine's reimplementation of
# it (the debug one exists only as Microsoft's), and the DLLs of every CMAKE_PREFIX_PATH entry on the DLL
# search path.
#
# The prefix is the build's own, never the user's default ~/.wine: MSVC_WINE_PREFIX, default
# <build>/wineprefix. The first binary run initializes it (wineboot), under a lock every run takes first,
# so that a parallel ctest does not race on it. WINEDEBUG defaults to -all, so wine's fixme lines stay out
# of the test output.
#
# The emulator is a script written into the build directory, <build>/wine-run.sh, rather than an
# `env WINEPATH=... wine` list: WINEPATH is ;-separated, and a ; inside a CMake list does not survive being
# expanded into a custom command. It also runs a binary by hand.
find_program(MSVC_WINE_WINE NAMES wine wine64)
if(NOT MSVC_WINE_WINE)
    message(FATAL_ERROR "wine is not installed: the tests of a Windows cross build run under it "
                        "(Fedora: dnf install wine; see docs/cross-compiling.md)")
endif()
set(MSVC_WINE_PREFIX "${CMAKE_BINARY_DIR}/wineprefix" CACHE PATH "The wine prefix the tests run in")
set(_redistRoot "${MSVC_WINE_ROOT}/vc/Redist/MSVC/${MSVC_WINE_TOOLSET_VERSION}")
file(GLOB _redist LIST_DIRECTORIES true "${_redistRoot}/x64/*.CRT" "${_redistRoot}/debug_nonredist/x64/*.DebugCRT")
set(_winePath ${_redist} "${MSVC_WINE_ROOT}/kits/10/bin/${MSVC_WINE_SDK_VERSION}/x64/ucrt")
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    list(APPEND _winePath "${_prefix}/bin")
endforeach()
list(REMOVE_DUPLICATES _winePath)
list(JOIN _winePath ";" _winePath)

# Quotes @p value for sh as one single-quoted word: a ' inside it becomes '\''.
function(_msvc_wine_sh_quote outVar value)
    string(REPLACE "'" "'\\''" value "${value}")
    set(${outVar} "'${value}'" PARENT_SCOPE)
endfunction()
_msvc_wine_sh_quote(_shPrefix "${MSVC_WINE_PREFIX}")
_msvc_wine_sh_quote(_shWinePath "${_winePath}")
_msvc_wine_sh_quote(_shWine "${MSVC_WINE_WINE}")
file(WRITE "${CMAKE_BINARY_DIR}/wine-run.sh"
     "#!/bin/sh\n"
     "# Written by cmake/toolchains/windows-clangcl-cross.cmake: runs a Windows executable of this build "
     "under wine.\n"
     "export WINEPREFIX=${_shPrefix}\n"
     "export WINEDEBUG=\"\${WINEDEBUG:--all}\"\n"
     "export WINEPATH=${_shWinePath}\n"
     "export WINEDLLOVERRIDES='msvcp140,msvcp140_1,msvcp140_2,vcruntime140,vcruntime140_1=n,b'\n"
     "wine=${_shWine}\n"
     "# The first test initializes the prefix, under the lock, which every test takes first: a parallel ctest\n"
     "# waits until the initialization is done, then sees the stamp written after it. -o keeps the lock out of\n"
     "# the wineserver that wineboot leaves running, which would otherwise hold it for its lifetime.\n"
     "mkdir -p \"$WINEPREFIX\"\n"
     "export wine\n"
     "flock -o \"$WINEPREFIX/.init.lock\" sh -c '[ -f \"$WINEPREFIX/.initialized\" ] || "
     "{ \"$wine\" wineboot --init >/dev/null 2>&1 && touch \"$WINEPREFIX/.initialized\"; }'\n"
     "exec \"$wine\" \"$@\"\n")
file(CHMOD "${CMAKE_BINARY_DIR}/wine-run.sh" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ
                                                         GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(CMAKE_CROSSCOMPILING_EMULATOR "${CMAKE_BINARY_DIR}/wine-run.sh")

# Dependencies come from CMAKE_PREFIX_PATH / CMAKE_FIND_ROOT_PATH, never from the Linux host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
