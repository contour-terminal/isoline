# SPDX-License-Identifier: Apache-2.0
#
# cmake-precompile-tripwire, E1's (D13): passes while both halves of E1's cause in
# cmake/IsolineModules.cmake hold. CMake's half: a BMI command of the build still carries a redundant `-c`
# beside Clang's `--precompile`. Clang's half: the compiler still warns about that `-c`
# (-Wunused-command-line-argument). Once either goes, E1 has nothing left to lift and the tripwire fails,
# saying to remove it.
#
#   cmake -DNINJA=<ninja> -DBUILD_DIR=<build tree> -DBUILD_CMAKE_VERSION=<version> -DCOMPILER=<clang++>
#         -P tests/cmake/expect-precompile-with-c.cmake
#
# It reads the build's own commands as Ninja expands them (`ninja -t commands`), so the layout of
# build.ninja's rules and variables plays no part; the build's module consumers (tests/modules/) are
# always among them. A build without any --precompile command fails it too: CMake then builds its BMIs
# some other way. Clang's half is probed on an empty module unit under BUILD_DIR.

cmake_minimum_required(VERSION 3.25)

foreach(argument IN ITEMS NINJA BUILD_DIR BUILD_CMAKE_VERSION COMPILER)
    if(NOT DEFINED ${argument} OR "${${argument}}" STREQUAL "")
        message(FATAL_ERROR "expect-precompile-with-c: -D${argument}=... is required")
    endif()
endforeach()

execute_process(COMMAND "${NINJA}" -C "${BUILD_DIR}" -t commands
                RESULT_VARIABLE exitStatus OUTPUT_VARIABLE commands ERROR_VARIABLE errors)
if(NOT exitStatus EQUAL 0)
    message(FATAL_ERROR "expect-precompile-with-c: `ninja -t commands` failed (${exitStatus}): ${errors}")
endif()

string(REGEX MATCHALL "[^\n]*[ ]--precompile[ ][^\n]*" precompileCommands "${commands}")
if(NOT precompileCommands)
    message(FATAL_ERROR "CMake ${BUILD_CMAKE_VERSION} runs no --precompile command in ${BUILD_DIR}, though "
                        "it builds module consumers: remove E1 from cmake/IsolineModules.cmake")
endif()
set(withC "")
foreach(command IN LISTS precompileCommands)
    if(command MATCHES "[ ]-c[ ]")
        list(APPEND withC "${command}")
    endif()
endforeach()
list(LENGTH precompileCommands precompileCount)
list(LENGTH withC withCCount)
if(withCCount EQUAL 0)
    message(FATAL_ERROR "CMake ${BUILD_CMAKE_VERSION} no longer passes -c with --precompile: remove E1 from "
                        "cmake/IsolineModules.cmake")
endif()

# Clang's half: what the compiler says about `-c` beside --precompile, on an empty module unit.
set(probeDirectory "${BUILD_DIR}/cmake-precompile-tripwire")
file(MAKE_DIRECTORY "${probeDirectory}")
file(WRITE "${probeDirectory}/probe.cppm" "export module probe;\n")
execute_process(COMMAND "${COMPILER}" -std=c++23 -x c++-module --precompile -o "${probeDirectory}/probe.pcm"
                        -c "${probeDirectory}/probe.cppm"
                RESULT_VARIABLE exitStatus OUTPUT_VARIABLE output ERROR_VARIABLE output)
execute_process(COMMAND "${COMPILER}" --version OUTPUT_VARIABLE compilerVersion)
string(REGEX MATCH "version [0-9.]+" compilerVersion "${compilerVersion}")
if(NOT exitStatus EQUAL 0)
    message(FATAL_ERROR "expect-precompile-with-c: ${COMPILER} failed on an empty module unit: ${output}")
endif()
if(NOT output MATCHES "argument unused during compilation: '-c'")
    message(FATAL_ERROR "Clang ${compilerVersion} no longer warns about -c with --precompile: remove E1 from "
                        "cmake/IsolineModules.cmake")
endif()
message(STATUS "E1 holds: ${withCCount} of ${precompileCount} --precompile commands carry -c "
               "(CMake ${BUILD_CMAKE_VERSION}), and Clang ${compilerVersion} warns about it")
