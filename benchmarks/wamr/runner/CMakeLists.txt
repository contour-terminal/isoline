# SPDX-License-Identifier: Apache-2.0
#
# isoline-benchmark-wamr: the WAMR runner of the benchmarks, one build per WAMR variant (../CMakeLists.txt
# builds this project as an external project per row of its variant table). It adds WAMR's source tree
# as Contour does (CPM's add_subdirectory, with the variant's options set first) and links its vmlib.
#
#   -D WAMR_SOURCE_DIR=<WAMR's source, patched as the variant asks>
#   -D ISOLINE_WAMR_OPTIONS_FILE=<a file setting ISOLINE_WAMR_OPTIONS_VARIABLE>   (empty: WAMR's defaults)
#   -D ISOLINE_WAMR_OPTIONS_VARIABLE=<the list variable that file sets>
#   -D ISOLINE_WAMR_VARIANT=<the variant's name, which the runner reports>
cmake_minimum_required(VERSION 3.25)
project(isoline-benchmark-wamr LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

foreach(required IN ITEMS WAMR_SOURCE_DIR ISOLINE_WAMR_VARIANT)
    if(NOT ${required})
        message(FATAL_ERROR "isoline-benchmark-wamr needs -D ${required}=...")
    endif()
endforeach()

if(ISOLINE_WAMR_OPTIONS_FILE)
    include("${ISOLINE_WAMR_OPTIONS_FILE}")
    foreach(option IN LISTS ${ISOLINE_WAMR_OPTIONS_VARIABLE})
        string(REPLACE " " ";" option "${option}")
        list(GET option 0 name)
        list(GET option 1 value)
        set(${name} ${value} CACHE INTERNAL "Set from ${ISOLINE_WAMR_OPTIONS_FILE}")
    endforeach()
else()
    set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "A static WAMR, as every variant links")
endif()

add_subdirectory("${WAMR_SOURCE_DIR}" wamr EXCLUDE_FROM_ALL SYSTEM)

add_executable(isoline-benchmark-wamr WamrBenchmarkRunner.cpp)
target_link_libraries(isoline-benchmark-wamr PRIVATE vmlib)
target_compile_definitions(isoline-benchmark-wamr PRIVATE ISOLINE_WAMR_VARIANT="${ISOLINE_WAMR_VARIANT}")
