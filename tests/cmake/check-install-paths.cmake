# SPDX-License-Identifier: Apache-2.0
#
# How check-install (tests/cmake/check-install.cmake, step 5) tells whether an installed file names a path
# of the build: pure functions of their arguments (and, for check_install_is_binary, of a file), so that
# tests/cmake/check-install-paths_test.cmake drives each in script mode.

include_guard(GLOBAL)

## @brief The form of @p text in which a path is compared: lower case, and every run of slashes and
## backslashes one `/`. A path is found in it whatever its separators (native, CMake's, or doubled as a
## CMake or JSON string escapes them) and whatever its case (Windows compares paths without it).
## @param outputVar Receives the comparable form.
function(check_install_comparable_paths text outputVar)
    string(TOLOWER "${text}" text)
    string(REGEX REPLACE "[/\\\\]+" "/" text "${text}")
    set(${outputVar} "${text}" PARENT_SCOPE)
endfunction()

## @brief Whether @p text, in the comparable form, names @p path, also comparable: an occurrence that no
## character of a path component adjoins, on either side, so that a source directory /work is not found in
## /WorkerIdentity, nor /src in .../src/ of another directory (".../tests/src/" holds "t/src").
## @param outputVar Receives ON or OFF.
function(check_install_names_path text path outputVar)
    string(LENGTH "${path}" pathLength)
    set(names OFF)
    while(TRUE)
        string(FIND "${text}" "${path}" at)
        if(at EQUAL -1)
            break()
        endif()
        math(EXPR after "${at} + ${pathLength}")
        string(SUBSTRING "${text}" ${after} 1 next)
        set(previous "")
        if(at GREATER 0)
            math(EXPR before "${at} - 1")
            string(SUBSTRING "${text}" ${before} 1 previous)
        endif()
        if(NOT next MATCHES "[a-z0-9._~+-]" AND NOT previous MATCHES "[a-z0-9._~+-]")
            set(names ON)
            break()
        endif()
        string(SUBSTRING "${text}" ${after} -1 text)
    endwhile()
    set(${outputVar} ${names} PARENT_SCOPE)
endfunction()

## @brief Whether @p file is binary: a NUL byte in its first 8000 bytes, which no text file has, and
## git's test for the same question. A binary, such as the static library, may name the build's paths
## (its debug information does), and is not read.
## @param outputVar Receives ON for a binary, OFF for a text file.
function(check_install_is_binary file outputVar)
    file(READ "${file}" head LIMIT 8000 HEX)
    # One byte per two hex digits: space them, so "00" is found at a byte's boundary only.
    string(REGEX REPLACE "(..)" " \\1" spaced "${head}")
    string(FIND "${spaced} " " 00 " at)
    if(at EQUAL -1)
        set(${outputVar} OFF PARENT_SCOPE)
    else()
        set(${outputVar} ON PARENT_SCOPE)
    endif()
endfunction()
