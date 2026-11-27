# SPDX-License-Identifier: Apache-2.0
#
# What the Windows x64 toolchain files that build on a Linux host share (windows-clangcl-cross.cmake,
# windows-msvc-wine-cross.cmake): the msvc-wine style install (https://github.com/mstorsjo/msvc-wine) of
# the MSVC toolset and the Windows SDK, and wine, which runs the test binaries and, for msvc-wine's
# cl.exe, the compiler too. Included by those files, never used as a toolchain file itself.
#
# Where the install lives is MSVC_WINE_ROOT -- a cache variable, else the environment variable of the
# same name, else /opt/msvc. The MSVC toolset and SDK versions are the newest found there unless
# MSVC_WINE_TOOLSET_VERSION / MSVC_WINE_SDK_VERSION name one.
#
# Sets, for the including file: _msvc (the toolset directory), _sdkInclude and _sdkLib (the SDK's include
# and library directories), and msvc_wine_write_runner(), which writes a script that runs a command in
# this build's wine prefix. Sets CMAKE_CROSSCOMPILING_EMULATOR to <build>/wine-run.sh, such a script
# running wine itself, so ctest runs test binaries as it would on Windows.
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

# Windows binaries run under wine, with Microsoft's own C++ runtime rather than wine's reimplementation of
# it (the debug one exists only as Microsoft's), and the DLLs of every CMAKE_PREFIX_PATH entry on the DLL
# search path.
#
# The prefix is the build's own, never the user's default ~/.wine: MSVC_WINE_PREFIX, default
# <build>/wineprefix. The first program run initializes it (wineboot), under a lock every run takes first,
# so that a parallel ctest or build does not race on it. WINEDEBUG defaults to -all, so wine's fixme lines
# stay out of the output.
#
# Each runner is a script written into the build directory rather than an `env WINEPATH=... wine` list:
# WINEPATH is ;-separated, and a ; inside a CMake list does not survive being expanded into a custom
# command. A runner also runs a binary by hand.
find_program(MSVC_WINE_WINE NAMES wine wine64)
if(NOT MSVC_WINE_WINE)
    message(FATAL_ERROR "wine is not installed: the tests of a Windows cross build run under it "
                        "(Fedora: dnf install wine; see docs/cross-compiling.md)")
endif()
set(MSVC_WINE_PREFIX "${CMAKE_BINARY_DIR}/wineprefix" CACHE PATH "The wine prefix the tests run in")
# A try_compile project (check_cxx_compiler_flag) reads this file again in a directory of its own: it gets
# the build's install and prefix, so a compiler that runs under wine does not boot a prefix per check.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MSVC_WINE_ROOT MSVC_WINE_TOOLSET_VERSION MSVC_WINE_SDK_VERSION
            MSVC_WINE_PREFIX)
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

## @brief Writes the executable sh script @p path, which runs @p program (a path, or `$wine` for wine
## itself) with the script's arguments in this build's wine prefix, initializing the prefix first.
## @p purpose completes the sentence "runs ..." in the script's header comment.
function(msvc_wine_write_runner path program purpose)
    _msvc_wine_sh_quote(_shPrefix "${MSVC_WINE_PREFIX}")
    _msvc_wine_sh_quote(_shWinePath "${_winePath}")
    _msvc_wine_sh_quote(_shWine "${MSVC_WINE_WINE}")
    if(program STREQUAL "$wine")
        set(_shProgram "\"$wine\"")
    else()
        _msvc_wine_sh_quote(_shProgram "${program}")
    endif()
    file(WRITE "${path}"
         "#!/bin/sh\n"
         "# Written by cmake/toolchains/msvc-wine-common.cmake: runs ${purpose} under wine.\n"
         "export WINEPREFIX=${_shPrefix}\n"
         "export WINEDEBUG=\"\${WINEDEBUG:--all}\"\n"
         "export WINEPATH=${_shWinePath}\n"
         "export WINEDLLOVERRIDES='msvcp140,msvcp140_1,msvcp140_2,vcruntime140,vcruntime140_1=n,b'\n"
         "wine=${_shWine}\n"
         "# The first run initializes the prefix, under the lock, which every run takes first: a parallel\n"
         "# run waits until the initialization is done, then sees the stamp written after it. -o keeps the\n"
         "# lock out of the wineserver that wineboot leaves running, which would otherwise hold it for its\n"
         "# lifetime.\n"
         "mkdir -p \"$WINEPREFIX\"\n"
         "export wine\n"
         "flock -o \"$WINEPREFIX/.init.lock\" sh -c '[ -f \"$WINEPREFIX/.initialized\" ] || "
         "{ \"$wine\" wineboot --init >/dev/null 2>&1 && touch \"$WINEPREFIX/.initialized\"; }'\n"
         "exec ${_shProgram} \"$@\"\n")
    file(CHMOD "${path}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ
                                     WORLD_EXECUTE)
endfunction()

msvc_wine_write_runner("${CMAKE_BINARY_DIR}/wine-run.sh" "$wine" "a Windows executable of this build")
set(CMAKE_CROSSCOMPILING_EMULATOR "${CMAKE_BINARY_DIR}/wine-run.sh")

# Dependencies come from CMAKE_PREFIX_PATH / CMAKE_FIND_ROOT_PATH, never from the Linux host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
