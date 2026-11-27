# SPDX-License-Identifier: Apache-2.0
#
# modules-off-build: a build with ISOLINE_MODULES OFF builds nothing of C++ modules. Its Ninja build files
# (build.ninja, or build-<config>.ninja of a multi-config generator) name no module target (*_module), no
# scan output (*.ddi), and no flag by which a compiler scans for module dependencies or compiles a module
# consumer. CMake's rules.ninja defines its scan rules whether or not a target uses them, so it is not read.
#
#   cmake -DBUILD_DIR=<build tree> -P tests/cmake/expect-no-module-build.cmake

cmake_minimum_required(VERSION 3.25)

if(NOT BUILD_DIR)
    message(FATAL_ERROR "expect-no-module-build: -DBUILD_DIR=... is required")
endif()
file(GLOB buildFiles LIST_DIRECTORIES false "${BUILD_DIR}/build.ninja" "${BUILD_DIR}/build-*.ninja")
if(NOT buildFiles)
    message(FATAL_ERROR "expect-no-module-build: ${BUILD_DIR} holds no build.ninja")
endif()
# (what, regular expression) rows: the traces a module build leaves in a build file.
set(traces
    "a module target"   "_module(\\.dir/|:| |$)"
    "a scan output"     "\\.ddi[ :|]"
    "a scanning flag"   "clang-scan-deps|-fdeps-format=|-fmodules-ts|-fmodule-mapper=|/scanDependencies|/ifcOutput")
set(found "")
foreach(buildFile IN LISTS buildFiles)
    file(STRINGS "${buildFile}" lines)
    set(rows "${traces}")
    while(rows)
        list(POP_FRONT rows what pattern)
        foreach(line IN LISTS lines)
            if(line MATCHES "${pattern}")
                list(APPEND found "${buildFile}: ${what}: ${line}")
                break()
            endif()
        endforeach()
    endwhile()
endforeach()
if(found)
    list(JOIN found "\n  " foundLines)
    message(FATAL_ERROR "expect-no-module-build: a build with ISOLINE_MODULES OFF builds modules:\n  ${foundLines}")
endif()
message(STATUS "expect-no-module-build: no module target, scan output or scanning flag")
