# SPDX-License-Identifier: Apache-2.0
#
# One find_package(isoline) as a consumer might write it, configured by tests/cmake/check-install.cmake
# against an install prefix, once per row of its probe table:
#
#   -DPROBE_VERSION=<version> -DPROBE_COMPONENTS=<components, |-separated, - for none>
#   -DPROBE_TARGETS=<targets the package must define, |-separated>
#
# It builds nothing. It succeeds when the package was found and defines every target of PROBE_TARGETS,
# and fails as find_package() does otherwise, for check-install to judge.

cmake_minimum_required(VERSION 3.25...4.3)
# CXX: core-cpp's package re-finds Threads, which needs a language.
project(isoline_package_probe LANGUAGES CXX)

if(PROBE_COMPONENTS STREQUAL "-")
    find_package(isoline ${PROBE_VERSION} REQUIRED)
else()
    string(REPLACE "|" ";" components "${PROBE_COMPONENTS}")
    find_package(isoline ${PROBE_VERSION} REQUIRED COMPONENTS ${components})
endif()

string(REPLACE "|" ";" targets "${PROBE_TARGETS}")
foreach(target IN LISTS targets)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "package-probe: find_package(isoline) defined no ${target}")
    endif()
endforeach()
message(STATUS "package-probe: isoline ${isoline_VERSION} defines ${targets}")
