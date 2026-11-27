# SPDX-License-Identifier: Apache-2.0
#
# A consumer of the INSTALLED package, configured against an install prefix and nothing else, the way
# a project outside Isoline's tree finds it:
#
#   find_package(isoline 0.1 REQUIRED COMPONENTS core)
#   target_link_libraries(app PRIVATE isoline::core)
#
# tests/cmake/check-install.cmake configures, builds and runs it with -DISOLINE_CONSUMER_MODE=<mode>.
# Its own project, so nothing of Isoline's build reaches it: no warning flag, no sanitizer, no
# CMAKE_CXX_FLAGS. Its warnings are its executables' own (target_compile_options below).
#
# The policy range reaches 4.3, where CMP0155 is NEW and a C++20 source is scanned for module imports,
# so the import modes need nothing more; the include mode runs on the header lanes' CMake from 3.25,
# and turns the scan off: a header lane's compiler (GCC 14, MSVC) would otherwise compile it as a
# module consumer (-fmodules-ts, /scanDependencies), which is not what it tests.

cmake_minimum_required(VERSION 3.25...4.3)
project(isoline_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ISOLINE_CONSUMER_MODE "include" CACHE STRING "How the consumer uses Isoline: one mode of the table below")

# The modes: (mode, the lowest CMake it runs on, its source, whether it is scanned for modules).
# A mode is a row.
set(isolineConsumerModes
    "include" 3.25 IncludeConsumer.cpp OFF)

set(isolineConsumerKnownModes "")
set(isolineConsumerSource "")
set(isolineConsumerScansForModules OFF)
set(isolineConsumerRows ${isolineConsumerModes})
while(isolineConsumerRows)
    list(POP_FRONT isolineConsumerRows mode minimumCMake source scansForModules)
    list(APPEND isolineConsumerKnownModes "${mode}")
    if(mode STREQUAL ISOLINE_CONSUMER_MODE)
        if(CMAKE_VERSION VERSION_LESS minimumCMake)
            message(FATAL_ERROR
                "ISOLINE_CONSUMER_MODE=${mode} needs CMake ${minimumCMake} or newer; "
                "this is ${CMAKE_VERSION}.")
        endif()
        set(isolineConsumerSource "${source}")
        set(isolineConsumerScansForModules ${scansForModules})
    endif()
endwhile()
if(NOT isolineConsumerSource)
    message(FATAL_ERROR
        "ISOLINE_CONSUMER_MODE must be one of ${isolineConsumerKnownModes}, not '${ISOLINE_CONSUMER_MODE}'.")
endif()

find_package(isoline 0.1 REQUIRED COMPONENTS core)
message(STATUS "isoline consumer: mode ${ISOLINE_CONSUMER_MODE}, "
               "isoline ${isoline_VERSION} from ${isoline_DIR}")

# Warnings as errors, per compiler front end, on the consumer's executable only.
set(isolineConsumerGnuStyleWarnings -Wall -Wextra -pedantic -Werror)
set(isolineConsumerMsvcWarnings /W4 /WX /permissive- /utf-8)

add_executable(isoline_consumer "${isolineConsumerSource}")
target_link_libraries(isoline_consumer PRIVATE isoline::core)
set_target_properties(isoline_consumer PROPERTIES CXX_SCAN_FOR_MODULES ${isolineConsumerScansForModules})
if(MSVC)
    target_compile_options(isoline_consumer PRIVATE ${isolineConsumerMsvcWarnings})
else()
    target_compile_options(isoline_consumer PRIVATE ${isolineConsumerGnuStyleWarnings})
endif()

enable_testing()
add_test(NAME isoline-consumer-${ISOLINE_CONSUMER_MODE} COMMAND isoline_consumer)
