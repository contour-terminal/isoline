# SPDX-License-Identifier: Apache-2.0
#
# The WAMR runners of the benchmarks: isoline-benchmark-wamr (runner/) built once per row of the variant table
# below, each against its own copy of WAMR's source, as an external project, since two WAMR trees cannot share
# one CMake build (both define vmlib). The source is the archive scripts/tools.lock pins (wamr.commit,
# wamr.source.url, wamr.source.sha256), checked against its SHA-256 before it is unpacked. Built with this
# tree's compilers and release flags, which scripts/benchmark.py's flag table sets for every C and C++ build.
include(ExternalProject)
find_package(Git REQUIRED)

# The variant table: a row per WAMR runner, its name (wamr-<name>), whether Contour's patches (patches.lock)
# are applied, and the file and list variable of the options set before WAMR's tree is added (none: WAMR's
# own defaults). Adding a variant is adding a row.
set(isolineWamrVariants contour default)
set(isolineWamrVariant_contour_patched ON)
set(isolineWamrVariant_contour_options "${CMAKE_CURRENT_SOURCE_DIR}/ContourOptions.cmake")
set(isolineWamrVariant_contour_optionsVariable ISOLINE_WAMR_CONTOUR_OPTIONS)
set(isolineWamrVariant_default_patched OFF)
set(isolineWamrVariant_default_options "")
set(isolineWamrVariant_default_optionsVariable "")

# The pinned source, from scripts/tools.lock.
file(STRINGS "${PROJECT_SOURCE_DIR}/scripts/tools.lock" isolineWamrLockRows REGEX "^wamr\\.")
foreach(row IN LISTS isolineWamrLockRows)
    if(row MATCHES "^wamr\\.([a-z0-9.]+) = (.+)$")
        string(REPLACE "." "_" key "${CMAKE_MATCH_1}")
        set("isolineWamr_${key}" "${CMAKE_MATCH_2}")
    endif()
endforeach()
foreach(required IN ITEMS commit source_url source_sha256)
    if(NOT isolineWamr_${required})
        message(FATAL_ERROR "scripts/tools.lock has no wamr.${required} row")
    endif()
endforeach()
string(REPLACE "{commit}" "${isolineWamr_commit}" isolineWamrUrl "${isolineWamr_source_url}")
set(ISOLINE_BENCHMARK_WAMR_ARCHIVE "" CACHE FILEPATH
    "A local copy of the WAMR source archive scripts/tools.lock pins, used instead of downloading it (checked against its SHA-256 alike)")
set(ISOLINE_BENCHMARK_WAMR_JOBS "" CACHE STRING
    "How many jobs each WAMR runner's build runs at once (scripts/benchmark.py build passes its --jobs); empty: the generator's default")
if(ISOLINE_BENCHMARK_WAMR_JOBS)
    set(isolineWamrParallel --parallel "${ISOLINE_BENCHMARK_WAMR_JOBS}")
else()
    set(isolineWamrParallel "")
endif()
if(ISOLINE_BENCHMARK_WAMR_ARCHIVE)
    set(isolineWamrUrl "${ISOLINE_BENCHMARK_WAMR_ARCHIVE}")
endif()

foreach(variant IN LISTS isolineWamrVariants)
    if(isolineWamrVariant_${variant}_patched)
        set(patchCommand "${CMAKE_COMMAND}" -D "GIT_EXECUTABLE=${GIT_EXECUTABLE}"
                         -D "PATCHES_LOCK=${CMAKE_CURRENT_SOURCE_DIR}/patches.lock"
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/ApplyPatches.cmake")
    else()
        set(patchCommand "")
    endif()
    ExternalProject_Add(wamr-${variant}-source
                        URL "${isolineWamrUrl}"
                        URL_HASH "SHA256=${isolineWamr_source_sha256}"
                        DOWNLOAD_EXTRACT_TIMESTAMP ON
                        SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/wamr-${variant}-source"
                        PATCH_COMMAND ${patchCommand}
                        CONFIGURE_COMMAND ""
                        BUILD_COMMAND ""
                        INSTALL_COMMAND "")
    ExternalProject_Add(wamr-${variant}
                        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/runner"
                        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/wamr-${variant}"
                        DEPENDS wamr-${variant}-source
                        CMAKE_ARGS
                            -G "${CMAKE_GENERATOR}"
                            "-DCMAKE_BUILD_TYPE=Release"
                            # Its compiler checks take the Release flags: -pie links no object built without -fPIE.
                            "-DCMAKE_TRY_COMPILE_CONFIGURATION=Release"
                            "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
                            "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                            "-DCMAKE_C_FLAGS_RELEASE=${CMAKE_C_FLAGS_RELEASE}"
                            "-DCMAKE_CXX_FLAGS_RELEASE=${CMAKE_CXX_FLAGS_RELEASE}"
                            "-DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
                            "-DWAMR_SOURCE_DIR=${CMAKE_CURRENT_BINARY_DIR}/wamr-${variant}-source"
                            "-DISOLINE_WAMR_VARIANT=${variant}"
                            "-DISOLINE_WAMR_OPTIONS_FILE=${isolineWamrVariant_${variant}_options}"
                            "-DISOLINE_WAMR_OPTIONS_VARIABLE=${isolineWamrVariant_${variant}_optionsVariable}"
                        BUILD_COMMAND "${CMAKE_COMMAND}" --build . --target isoline-benchmark-wamr ${isolineWamrParallel}
                        INSTALL_COMMAND ""
                        BUILD_BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/wamr-${variant}/isoline-benchmark-wamr")
endforeach()
