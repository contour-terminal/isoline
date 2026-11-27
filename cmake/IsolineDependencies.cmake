# SPDX-License-Identifier: Apache-2.0
#
# Isoline's dependencies: core-cpp, and Catch2 for the tests.
#
# core-cpp is the parent project's when it already defines it (Contour vendors the same tag, in
# lockstep), otherwise the verbatim copy in vendor/core-cpp. Never edit that copy; it is checked
# against its MANIFEST (the vendor-check CI job).

include_guard(GLOBAL)

if(NOT TARGET core::base)
    # Only the modules Isoline links are wanted. Normal variables override core-cpp's option()
    # defaults (CMP0077) without writing into the cache.
    set(CORE_CPP_TESTING OFF)
    set(CORE_CPP_CATCH2_MAIN OFF)
    set(CORE_CPP_BUILD_EXAMPLES OFF)
    # Install rules for core-cpp's targets exactly when Isoline has its own: core-cpp's
    # CoreCppInstall.cmake says "a parent that exports targets of its own linking core-cpp's turns it
    # on", because CMake refuses an export set (isolineCoreTargets) whose targets link a target in no
    # export set. The rules never run with Isoline's: EXCLUDE_FROM_ALL below leaves them out of this
    # build's install, so Isoline does not install core-cpp; a packager installs it first.
    set(CORE_CPP_INSTALL ${ISOLINE_INSTALL})
    set(CORE_CPP_WITH_TUI OFF)
    set(CORE_CPP_WITH_TUI_OUTPUT OFF)
    set(CORE_CPP_WITH_TUI_CORE OFF)
    set(CORE_CPP_WITH_TLS OFF)
    set(CORE_CPP_WITH_SYSTEMD OFF)
    set(CORE_CPP_FETCH_DEPS ${ISOLINE_FETCH_DEPS})
    # EXCLUDE_FROM_ALL builds only the core-cpp targets Isoline links; SYSTEM keeps its headers out
    # of Isoline's warning set.
    add_subdirectory("${PROJECT_SOURCE_DIR}/vendor/core-cpp" "${PROJECT_BINARY_DIR}/vendor/core-cpp"
                     SYSTEM EXCLUDE_FROM_ALL)
    file(STRINGS "${PROJECT_SOURCE_DIR}/vendor/core-cpp/MANIFEST" _isolineCoreCppRef REGEX "^# ref ")
    string(REPLACE "# ref " "" _isolineCoreCppRef "${_isolineCoreCppRef}")
    message(STATUS "[isoline] core-cpp: vendored ${_isolineCoreCppRef} (vendor/core-cpp)")
    # IsolinePedantic.cmake instruments the vendored copy's targets with Isoline's sanitizers. A
    # parent project's core-cpp is the parent's to instrument.
    set(ISOLINE_VENDORED_CORE_CPP ON)
else()
    set(ISOLINE_VENDORED_CORE_CPP OFF)
    message(STATUS "[isoline] core-cpp: from the parent project")
endif()

if(ISOLINE_TESTING AND NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 QUIET)
    if(Catch2_FOUND)
        message(STATUS "[isoline] Catch2: found by find_package(Catch2 3), version ${Catch2_VERSION}")
    elseif(ISOLINE_FETCH_DEPS)
        if(NOT COMMAND CPMAddPackage)
            include("${PROJECT_SOURCE_DIR}/vendor/core-cpp/cmake/CPM.cmake")
        endif()
        CPMAddPackage(NAME Catch2 VERSION 3.8.0 GITHUB_REPOSITORY catchorg/Catch2
                      EXCLUDE_FROM_ALL YES SYSTEM YES)
        message(STATUS "[isoline] Catch2: fetched by CPM, version 3.8.0")
    else()
        message(FATAL_ERROR
            "ISOLINE_TESTING needs Catch2 v3, which find_package(Catch2 3) did not find. "
            "Install it, set ISOLINE_FETCH_DEPS=ON, or set ISOLINE_TESTING=OFF.")
    endif()
endif()
