# SPDX-License-Identifier: Apache-2.0
#
# Checks, from the build's own compile and link commands, who pays for position-independent code:
#
#   cmake -DNINJA=<ninja> -DBUILD=<build directory> -DPAIRS=<original>=<twin>;... -DSHARED=<target>
#         -DPIC_FLAG=<flag> [-DTWIN_FLAGS=<flag>;...] -DCOMPILER=<the C++ compiler> -P CheckPicTwins.cmake
#   cmake -DSELFTEST=ON -P CheckPicTwins.cmake
#
# - An original (isoline_core, the core-cpp libraries it links) is compiled without the PIC flags, so the
#   executables and a static consumer get the code they would without a shared library in the build.
# - Its twin compiles the same sources with the same command, apart from its object directory, the
#   clang-tidy wrapper the original alone runs under, and the PIC flags, which it adds: a property the
#   twin failed to copy shows up as a difference. The PIC flag is PIC_FLAG; TWIN_FLAGS are what else the
#   twins alone are compiled with (GCC's -fno-semantic-interposition).
# - The shared library links the twins and none of the originals, and no other link step of the build
#   links a twin.
#
# Registered only where the build makes twins (tests/c/CMakeLists.txt), so PAIRS is never empty. SELFTEST checks
# how a command is read, on fixture commands: what the comparison ignores, and what it does not.

cmake_minimum_required(VERSION 3.25)

set(problems "")

## @brief Sets @p outVar to @p command as two compiles are compared: without the clang-tidy wrapper, without a
## compiler launcher in front of @p compiler, and without what names the target's own files (the object and
## its dependency file).
function(_comparable command compiler outVar)
    # The command proper starts at the compiler. What runs it is dropped: the clang-tidy wrapper
    # (`cmake -E __run_co_compile --launcher=... --tidy=... -- <compiler> ...`) the original alone runs under,
    # and a compiler launcher (a compile cache, CMAKE_CXX_COMPILER_LAUNCHER) a target without it runs in front,
    # so two compiles differ only where the compiler is told something different.
    string(FIND "${command}" "${compiler} " compilerAt)
    if(compilerAt GREATER 0)
        string(SUBSTRING "${command}" ${compilerAt} -1 command)
    endif()
    string(REGEX REPLACE " -MT [^ ]+| -MF [^ ]+| -o [^ ]+" "" command "${command}")
    set(${outVar} "${command}" PARENT_SCOPE)
endfunction()

if(SELFTEST)
    # One compile as an original under clang-tidy and as a twin under a compile cache, which read the same,
    # and one whose flags differ, which does not.
    set(compiler "/usr/bin/clang++")
    set(compile "${compiler} -I/src -g -std=c++23 -MD -c /src/a.cpp")
    set(tidied "/usr/bin/cmake -E __run_co_compile --launcher=/usr/bin/ccache --tidy=clang-tidy --source=/src/a.cpp -- ${compile} -MT a.o -MF a.o.d -o CMakeFiles/core.dir/a.o")
    set(cached "/usr/bin/cmake -E env PACE_ADDR=127.0.0.1:6674 /usr/bin/pace-cc ${compile} -MT b.o -MF b.o.d -o CMakeFiles/twin.dir/a.o")
    set(other "/usr/bin/ccache ${compiler} -I/src -g -O2 -std=c++23 -MD -c /src/a.cpp -o CMakeFiles/twin.dir/a.o")
    _comparable("${tidied}" "${compiler}" fromTidied)
    _comparable("${cached}" "${compiler}" fromCached)
    _comparable("${other}" "${compiler}" fromOther)
    if(NOT fromTidied STREQUAL compile OR NOT fromCached STREQUAL compile)
        message(FATAL_ERROR "CheckPicTwins selftest: the same compile reads as\n  '${fromTidied}'\n  '${fromCached}'\nnot '${compile}'")
    endif()
    if(fromOther STREQUAL compile)
        message(FATAL_ERROR "CheckPicTwins selftest: a compile of other flags reads as the same: '${fromOther}'")
    endif()
    message(STATUS "CheckPicTwins selftest: passed")
    return()
endif()

## @brief Sets @p outVar to the compile commands Ninja runs for the object files of @p target, keyed by
## the source file's path, each as _comparable() reads it.
function(_compile_commands target outVar)
    execute_process(COMMAND "${NINJA}" -C "${BUILD}" -t commands ${target}
                    RESULT_VARIABLE status OUTPUT_VARIABLE commands ERROR_VARIABLE errors)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "CheckPicTwins: ninja -t commands ${target} failed (${status}): ${errors}")
    endif()
    string(REPLACE ";" "\\;" commands "${commands}")
    string(REPLACE "\n" ";" commands "${commands}")
    set(found "")
    foreach(command IN LISTS commands)
        # The object files of this target are written to CMakeFiles/<target>.dir/.
        if(NOT command MATCHES "CMakeFiles/${target}[.]dir/" OR NOT command MATCHES " -c ")
            continue()
        endif()
        if(NOT command MATCHES " -c ([^ ]+)")
            continue()
        endif()
        set(source "${CMAKE_MATCH_1}")
        _comparable("${command}" "${COMPILER}" command)
        list(APPEND found "${source}=${command}")
    endforeach()
    set(${outVar} "${found}" PARENT_SCOPE)
endfunction()

## @brief Sets @p outVar to whether @p command carries one of the flags that follow.
function(_has_flag command outVar)
    set(has OFF)
    foreach(flag IN LISTS ARGN)
        string(FIND " ${command} " " ${flag} " at)
        if(NOT at EQUAL -1)
            set(has ON)
        endif()
    endforeach()
    set(${outVar} ${has} PARENT_SCOPE)
endfunction()

if(NOT PAIRS)
    message(FATAL_ERROR "CheckPicTwins: no twins to check (PAIRS is empty)")
endif()
set(originals "")
set(twins "")
foreach(pair IN LISTS PAIRS)
    if(NOT pair MATCHES "^([^=]+)=([^=]+)$")
        message(FATAL_ERROR "CheckPicTwins: '${pair}' is no <original>=<twin> pair")
    endif()
    set(original "${CMAKE_MATCH_1}")
    set(twin "${CMAKE_MATCH_2}")
    list(APPEND originals "${original}")
    list(APPEND twins "${twin}")
    _compile_commands(${original} originalCommands)
    _compile_commands(${twin} twinCommands)
    if(NOT originalCommands)
        string(APPEND problems "\n  ${original}: no compile command found")
    endif()
    list(LENGTH originalCommands originalCount)
    list(LENGTH twinCommands twinCount)
    if(NOT originalCount EQUAL twinCount)
        string(APPEND problems "\n  ${twin} compiles ${twinCount} sources, ${original} ${originalCount}")
    endif()
    foreach(entry IN LISTS originalCommands)
        string(REGEX MATCH "^[^=]+" source "${entry}")
        string(LENGTH "${source}=" prefixLength)
        string(SUBSTRING "${entry}" ${prefixLength} -1 command)
        _has_flag("${command}" originalPic ${PIC_FLAG} ${TWIN_FLAGS})
        if(originalPic)
            string(APPEND problems "\n  ${original} compiles ${source} with a twin's flag: ${command}")
        endif()
        set(twinCommand "")
        foreach(twinEntry IN LISTS twinCommands)
            if(twinEntry MATCHES "^([^=]+)=(.*)$" AND CMAKE_MATCH_1 STREQUAL source)
                set(twinCommand "${CMAKE_MATCH_2}")
            endif()
        endforeach()
        if(NOT twinCommand)
            string(APPEND problems "\n  ${twin} does not compile ${source}")
            continue()
        endif()
        _has_flag("${twinCommand}" twinPic ${PIC_FLAG})
        if(NOT twinPic)
            string(APPEND problems "\n  ${twin} compiles ${source} without a PIC flag: ${twinCommand}")
        endif()
        # The same command once the PIC flags are taken out of the twin's.
        set(stripped " ${twinCommand} ")
        foreach(flag IN ITEMS ${PIC_FLAG} ${TWIN_FLAGS})
            string(REPLACE " ${flag} " " " stripped "${stripped}")
        endforeach()
        string(STRIP "${stripped}" stripped)
        if(NOT stripped STREQUAL command)
            string(APPEND problems "\n  ${twin} compiles ${source} unlike ${original} beyond the PIC flags:"
                                   "\n    ${original}: ${command}\n    ${twin}: ${stripped}")
        endif()
    endforeach()
endforeach()

# Every link step of the build: the shared library's links the twins' archives and none of the originals';
# every other links no twin, so no executable gets the PIC code and none links an original beside its twin.
# A link step is a command that writes an output (-o) without compiling (-c) or archiving (twins are made
# for ELF alone, so the commands are GNU-style drivers').
execute_process(COMMAND "${NINJA}" -C "${BUILD}" -t commands
                RESULT_VARIABLE status OUTPUT_VARIABLE commands ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "CheckPicTwins: ninja -t commands failed (${status}): ${errors}")
endif()
string(REPLACE ";" "\\;" commands "${commands}")
string(REPLACE "\n" ";" commands "${commands}")
set(sharedLinks 0)
set(otherLinks 0)
foreach(command IN LISTS commands)
    if(command MATCHES " -c " OR NOT command MATCHES " -o " OR command MATCHES "(^|[/ ])(llvm-)?(ar|ranlib)( |$)")
        continue()
    endif()
    # A link whose inputs are in a response file (@<file>.rsp) names none of them on its line, and Ninja
    # deletes the file once the link succeeds: this check cannot read it, so it says so rather than pass it.
    if(command MATCHES " @[^ ]+[.]rsp( |$)")
        string(REGEX MATCH " -o [^ ]+" output "${command}")
        string(APPEND problems "\n  a link reads its inputs from a response file, which this check cannot read:${output}")
        continue()
    endif()
    set(linkedTwins "")
    foreach(twin IN LISTS twins)
        if(command MATCHES "/lib${twin}[.]a( |$)")
            list(APPEND linkedTwins ${twin})
        endif()
    endforeach()
    if(command MATCHES " -shared " AND command MATCHES "CMakeFiles/${SHARED}[.]dir/")
        math(EXPR sharedLinks "${sharedLinks} + 1")
        foreach(twin IN LISTS twins)
            if(NOT twin IN_LIST linkedTwins)
                string(APPEND problems "\n  ${SHARED} does not link ${twin}")
            endif()
        endforeach()
        foreach(original IN LISTS originals)
            if(command MATCHES "/lib${original}[.]a( |$)")
                string(APPEND problems "\n  ${SHARED} links ${original}, the executables' code, not its twin")
            endif()
        endforeach()
    else()
        math(EXPR otherLinks "${otherLinks} + 1")
        if(linkedTwins)
            string(REGEX MATCH " -o [^ ]+" output "${command}")
            string(APPEND problems "\n  a link other than ${SHARED}'s links the twin(s) ${linkedTwins}:${output}")
        endif()
    endif()
endforeach()
if(NOT sharedLinks EQUAL 1)
    string(APPEND problems "\n  ${sharedLinks} link commands of ${SHARED}, not one")
endif()
if(otherLinks EQUAL 0)
    string(APPEND problems "\n  no link command but ${SHARED}'s: the scan found none of the executables' links")
endif()

if(problems)
    message(FATAL_ERROR "CheckPicTwins:${problems}")
endif()
list(LENGTH twins twinCount)
message(STATUS "CheckPicTwins: ${twinCount} twin(s) compile as their originals do plus '${PIC_FLAG} ${TWIN_FLAGS}'; "
               "${SHARED} links them, and none of the other ${otherLinks} links does")
