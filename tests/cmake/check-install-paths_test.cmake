# SPDX-License-Identifier: Apache-2.0
#
# check-install's path tests (tests/cmake/check-install-paths.cmake), in script mode:
#
#   cmake -DWORK_DIRECTORY=<scratch directory> -P tests/cmake/check-install-paths_test.cmake
#
# Each function is a table of inputs and the answer it must give; any row that gets another fails the script.

cmake_minimum_required(VERSION 3.25)

include("${CMAKE_CURRENT_LIST_DIR}/check-install-paths.cmake")

if(NOT WORK_DIRECTORY)
    message(FATAL_ERROR "check-install-paths: -DWORK_DIRECTORY=... is required")
endif()

set(failures "")

# (text → its comparable form): lower case, every run of `/` and `\` one `/`. A `\` in a row is written
# `\\`: a native Windows path, and one doubled as a CMake or JSON string escapes it.
set(comparableCases
    "/home/x/Isoline"                    "/home/x/isoline"
    "\\HOME\\X\\ISOLINE\\a"              "/home/x/isoline/a"
    "C:\\\\home\\\\x\\\\isoline"         "c:/home/x/isoline"
    "//home//X/isoline"                  "/home/x/isoline"
    "D:/a/Isoline/Isoline"               "d:/a/isoline/isoline")
while(comparableCases)
    list(POP_FRONT comparableCases text expected)
    check_install_comparable_paths("${text}" actual)
    if(NOT actual STREQUAL expected)
        list(APPEND failures "comparable form of '${text}': '${actual}', expected '${expected}'")
    endif()
endwhile()

# (text, path → whether the text names the path), both taken in their comparable form, as check-install does.
set(namesCases
    "x /work/src"                        "/work"            ON
    "\"/work\""                          "/work"            ON
    "/work"                              "/work"            ON
    "/workx /work)"                      "/work"            ON
    "a /WorkerIdentity b"                "/work"            OFF
    "/work-tree"                         "/work"            OFF
    "/work.d"                            "/work"            OFF
    "include \"/src/a.hpp\""             "/src"             ON
    "isoline/tests/src/a.hpp"            "/src"             OFF
    "tests/src/a.hpp \"/src/b.hpp\""     "/src"             ON
    "/mnt/home/x/isoline"                "/home/x/isoline"  OFF
    "set(dir \"D:\\A\\Isoline\\src\")"   "d:/a/isoline"     ON
    "set(dir \"/HOME/X/ISOLINE/lib\")"   "/home/x/isoline"  ON
    "/home/x/isolinf"                    "/home/x/isoline"  OFF
    "/home/x/isoline-p18"                "/home/x/isoline"  OFF
    "no path here"                       "/home/x/isoline"  OFF)
while(namesCases)
    list(POP_FRONT namesCases text path expected)
    check_install_comparable_paths("${text}" comparableText)
    check_install_comparable_paths("${path}" comparablePath)
    check_install_names_path("${comparableText}" "${comparablePath}" actual)
    if(NOT actual STREQUAL expected)
        list(APPEND failures "'${text}' names '${path}': ${actual}, expected ${expected}")
    endif()
endwhile()

# (file → whether it is binary): a NUL byte in its first 8000 bytes. A tar archive pads its header with them.
file(REMOVE_RECURSE "${WORK_DIRECTORY}")
file(MAKE_DIRECTORY "${WORK_DIRECTORY}")
file(WRITE "${WORK_DIRECTORY}/text.cmake" "set(x \"/home/x/isoline\")\n")
file(WRITE "${WORK_DIRECTORY}/empty" "")
# 50 0a: its hex holds "00", but across two bytes, neither of them NUL.
file(WRITE "${WORK_DIRECTORY}/zero-across-bytes" "P\n")
string(REPEAT "a" 9000 long)
file(WRITE "${WORK_DIRECTORY}/long-text" "${long}")
file(ARCHIVE_CREATE OUTPUT "${WORK_DIRECTORY}/binary.tar" PATHS "${WORK_DIRECTORY}/text.cmake" FORMAT gnutar)
set(binaryCases
    "text.cmake"         OFF
    "empty"              OFF
    "zero-across-bytes"  OFF
    "long-text"          OFF
    "binary.tar"         ON)
while(binaryCases)
    list(POP_FRONT binaryCases name expected)
    check_install_is_binary("${WORK_DIRECTORY}/${name}" actual)
    if(NOT actual STREQUAL expected)
        list(APPEND failures "${name} is binary: ${actual}, expected ${expected}")
    endif()
endwhile()

if(failures)
    list(JOIN failures "\n  " listed)
    message(FATAL_ERROR "check-install-paths: failed:\n  ${listed}")
endif()
message(STATUS "check-install-paths: passed")
