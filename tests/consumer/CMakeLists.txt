# SPDX-License-Identifier: Apache-2.0
#
# A consumer of the INSTALLED package, configured against an install prefix and nothing else, the way
# a project outside Isoline's tree finds it:
#
#   find_package(isoline 0.1 REQUIRED COMPONENTS core)
#   target_link_libraries(app PRIVATE isoline::core)
#
# A mode asks for the components of what it uses (ConsumerModes.cmake) and links their targets: include-text
# asks for text alone and import-fixture for recipe_fixture alone, beneath which the package loads core.
#
# tests/cmake/check-install.cmake configures, builds and runs it with -DISOLINE_CONSUMER_MODE=<mode>.
# Its own project, so nothing of Isoline's build reaches it: no warning flag, no sanitizer, no
# CMAKE_CXX_FLAGS. Its warnings are its executables' own (target_compile_options below).
#
# The policy range reaches 4.3, where CMP0155 is NEW and a C++20 source is scanned for module imports,
# so the import modes need nothing more; the include mode runs on the header lanes' CMake from 3.25,
# and turns the scan off: a header lane's compiler (GCC 14, MSVC) would otherwise compile it as a
# module consumer (-fmodules-ts, /scanDependencies), which is not what it tests.
#
# The modes are the rows of ConsumerModes.cmake. Mode import-std turns on CMake's experimental
# `import std;` before project(), by the gate UUID of this CMake's row below, and then needs a toolchain
# whose CMAKE_CXX_COMPILER_IMPORT_STD lists 23. Where either is missing, it prints
# "import std unavailable: <reason>", records the reason in ISOLINE_CONSUMER_IMPORT_STD_UNAVAILABLE and
# defines nothing to build: check-install then skips (exit 77), or fails where the lane expects import
# std (ISOLINE_EXPECT_IMPORT_STD).

cmake_minimum_required(VERSION 3.25...4.3)

set(ISOLINE_CONSUMER_MODE "include" CACHE STRING "How the consumer uses Isoline: one mode of ConsumerModes.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/ConsumerModes.cmake")
isoline_consumer_mode("${ISOLINE_CONSUMER_MODE}" isolineConsumer)
if(NOT isolineConsumerFound)
    message(FATAL_ERROR
        "ISOLINE_CONSUMER_MODE must be one of ${isolineConsumerKnownModes}, not '${ISOLINE_CONSUMER_MODE}'.")
endif()
if(CMAKE_VERSION VERSION_LESS isolineConsumerMinimumCMake)
    message(FATAL_ERROR
        "ISOLINE_CONSUMER_MODE=${ISOLINE_CONSUMER_MODE} needs CMake ${isolineConsumerMinimumCMake} or newer; "
        "this is ${CMAKE_VERSION}.")
endif()

# The gate of CMake's experimental `import std;`, per CMake version: (major.minor, CMAKE_EXPERIMENTAL_CXX_IMPORT_STD).
# Each CMake release names its own UUID (Help/dev/experimental.rst), measured here on 4.3.0; ababa1b5-...
# is CMAKE_EXPERIMENTAL_MAPPED_PACKAGE_INFO's, and is rejected. A CMake version is a row (D9).
set(isolineConsumerImportStdGates
    "4.3" "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")

set(ISOLINE_CONSUMER_IMPORT_STD_UNAVAILABLE "" CACHE INTERNAL "Why mode import-std builds nothing, or empty")
set(isolineConsumerImportStdUnavailable "")
if(isolineConsumerImportsStd)
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" isolineConsumerCMakeMinor "${CMAKE_VERSION}")
    set(isolineConsumerGate "")
    set(isolineConsumerRows "${isolineConsumerImportStdGates}")
    while(isolineConsumerRows)
        list(POP_FRONT isolineConsumerRows rowVersion rowGate)
        if(rowVersion STREQUAL isolineConsumerCMakeMinor)
            set(isolineConsumerGate "${rowGate}")
        endif()
    endwhile()
    if(isolineConsumerGate)
        set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "${isolineConsumerGate}")
    else()
        set(isolineConsumerImportStdUnavailable
            "CMake ${CMAKE_VERSION} has no row in the consumer's import std gate table")
    endif()
endif()

project(isoline_consumer LANGUAGES CXX)

# The standard library the compiler uses (libstdc++, libc++; empty where CMake does not tell, as for MSVC),
# which check-install's rows of tolerated diagnostics are keyed by. CMake determines it with the compiler
# and keeps it nowhere, so the cache keeps the first configure's answer.
if(CMAKE_CXX_STANDARD_LIBRARY)
    set(ISOLINE_CONSUMER_STANDARD_LIBRARY "${CMAKE_CXX_STANDARD_LIBRARY}" CACHE INTERNAL
        "The C++ standard library of the consumer's compiler, or empty")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The components of what the mode uses (ConsumerModes.cmake).
find_package(isoline 0.1 REQUIRED COMPONENTS ${isolineConsumerPackageComponents})
message(STATUS "isoline consumer: mode ${ISOLINE_CONSUMER_MODE}, "
               "isoline ${isoline_VERSION} from ${isoline_DIR}")

enable_testing()

if(isolineConsumerImportsStd AND NOT isolineConsumerImportStdUnavailable
   AND NOT "23" IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    list(JOIN CMAKE_CXX_COMPILER_IMPORT_STD ", " isolineConsumerImportStdStandards)
    string(CONCAT isolineConsumerImportStdUnavailable
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} provides no std module for C++23 "
        "(CMAKE_CXX_COMPILER_IMPORT_STD: '${isolineConsumerImportStdStandards}')")
endif()
# CMake 4.3 asks Clang for the standard library's modules.json by `-print-file-name`, which prints the bare
# name when there is none (Clang 19.1.0 with Ubuntu 24.04's libstdc++ 14, measured 2026-11-27), and then
# lists 23 in CMAKE_CXX_COMPILER_IMPORT_STD all the same: the generate step fails on the missing file. A
# modules.json that is not an existing file is no std module. This guards that known CMake defect and
# nothing else: remove it once CMake checks the file itself (its upstream report is pending the owner).
# The clang-floor CI row asserts that its import-std skip gives this reason, so the branch is seen to run.
if(isolineConsumerImportsStd AND NOT isolineConsumerImportStdUnavailable AND CMAKE_CXX_STDLIB_MODULES_JSON
   AND NOT (IS_ABSOLUTE "${CMAKE_CXX_STDLIB_MODULES_JSON}" AND EXISTS "${CMAKE_CXX_STDLIB_MODULES_JSON}"))
    string(CONCAT isolineConsumerImportStdUnavailable
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} names the std module's description "
        "'${CMAKE_CXX_STDLIB_MODULES_JSON}', which is no file (CMAKE_CXX_STDLIB_MODULES_JSON)")
endif()
if(isolineConsumerImportStdUnavailable)
    message(STATUS "import std unavailable: ${isolineConsumerImportStdUnavailable}")
    set(ISOLINE_CONSUMER_IMPORT_STD_UNAVAILABLE "${isolineConsumerImportStdUnavailable}" CACHE INTERNAL
        "Why mode import-std builds nothing, or empty")
    return()
endif()

# Warnings as errors, per compiler front end, on the consumer's executable only.
set(isolineConsumerGnuStyleWarnings -Wall -Wextra -pedantic -Werror)
set(isolineConsumerMsvcWarnings /W4 /WX)
# MSVC's flags that shape the compilation environment go to every compile, not to the executable alone:
# CMake builds an imported module's BMI (and import std's) in a synthetic target that takes
# CMAKE_CXX_FLAGS and the module's own exported options, but neither the importer's target options nor
# its directory's. An importer whose environment differs from the BMI's fails under /WX with C5050,
# "Possible incompatible environment while importing module 'isoline': _UTF8 is defined in current
# command line and not in module command line" (measured, MSVC 19.44).
if(MSVC)
    string(APPEND CMAKE_CXX_FLAGS " /permissive- /utf-8")
endif()

add_executable(isoline_consumer "${isolineConsumerSource}")
if(isolineConsumerImportsIsoline)
    # The test modules' bytes as a module of the consumer's own (WasmModules.cppm), so an importer
    # reaches them without an #include.
    target_sources(isoline_consumer PRIVATE FILE_SET CXX_MODULES FILES WasmModules.cppm)
endif()
target_link_libraries(isoline_consumer PRIVATE ${isolineConsumerLinkTargets})
set(isolineConsumerScansForModules OFF)
if(isolineConsumerImportsIsoline OR isolineConsumerImportsStd)
    set(isolineConsumerScansForModules ON)
endif()
set_target_properties(isoline_consumer PROPERTIES CXX_SCAN_FOR_MODULES ${isolineConsumerScansForModules})
if(isolineConsumerImportsStd)
    set_target_properties(isoline_consumer PROPERTIES CXX_MODULE_STD ON)
    target_compile_definitions(isoline_consumer PRIVATE ISOLINE_CONSUMER_IMPORT_STD=1)
endif()
if(MSVC)
    target_compile_options(isoline_consumer PRIVATE ${isolineConsumerMsvcWarnings})
else()
    target_compile_options(isoline_consumer PRIVATE ${isolineConsumerGnuStyleWarnings})
endif()

add_test(NAME isoline-consumer-${ISOLINE_CONSUMER_MODE} COMMAND isoline_consumer)
# A shared library it links (isoline::c) is found beside the executable or on PATH on Windows, which has no
# run path: its directory, the prefix's bin/, goes first on the test's PATH. Without it the loader would
# open a dialog about the missing DLL and hold the run until its timeout.
if(WIN32)
    foreach(isolineConsumerLinkTarget IN LISTS isolineConsumerLinkTargets)
        get_target_property(isolineConsumerLinkType ${isolineConsumerLinkTarget} TYPE)
        if(isolineConsumerLinkType STREQUAL "SHARED_LIBRARY")
            set_property(TEST isoline-consumer-${ISOLINE_CONSUMER_MODE} APPEND PROPERTY ENVIRONMENT_MODIFICATION
                         "PATH=path_list_prepend:$<TARGET_FILE_DIR:${isolineConsumerLinkTarget}>")
        endif()
    endforeach()
endif()
