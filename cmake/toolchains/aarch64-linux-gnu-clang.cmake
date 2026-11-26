# SPDX-License-Identifier: Apache-2.0
#
# Cross-compiles aarch64 Linux executables on an x86_64 Linux host with the host's clang and lld, against a
# Debian sysroot that scripts/make-aarch64-sysroot.sh creates.
#
# Where the sysroot lives is ISOLINE_AARCH64_SYSROOT -- a cache variable, else the environment variable of
# the same name, else ~/.cache/isoline/sysroot-aarch64-trixie (the script's default).
#
# Test binaries run under qemu user mode: CMAKE_CROSSCOMPILING_EMULATOR is set, so ctest runs them as it
# would on an arm64 machine. The emulated CPU is QEMU_CPU from the environment (e.g. max, cortex-a53).
#
# @see docs/cross-compiling.md

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED ISOLINE_AARCH64_SYSROOT)
    if(DEFINED ENV{ISOLINE_AARCH64_SYSROOT})
        set(ISOLINE_AARCH64_SYSROOT "$ENV{ISOLINE_AARCH64_SYSROOT}")
    else()
        set(ISOLINE_AARCH64_SYSROOT "$ENV{HOME}/.cache/isoline/sysroot-aarch64-trixie")
    endif()
endif()
set(ISOLINE_AARCH64_SYSROOT "${ISOLINE_AARCH64_SYSROOT}" CACHE PATH "The aarch64 Linux sysroot")
if(NOT IS_DIRECTORY "${ISOLINE_AARCH64_SYSROOT}/usr/include/aarch64-linux-gnu")
    message(FATAL_ERROR "ISOLINE_AARCH64_SYSROOT=${ISOLINE_AARCH64_SYSROOT} is no aarch64 sysroot; "
                        "create it with scripts/make-aarch64-sysroot.sh")
endif()
set(CMAKE_SYSROOT "${ISOLINE_AARCH64_SYSROOT}")

# Dependencies come from the sysroot, never from the host; programs (Python, the tools) from the host,
# the compilers and qemu included: the sysroot has aarch64 binaries in usr/bin.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

find_program(CMAKE_C_COMPILER NAMES clang REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang++ REQUIRED)
set(CMAKE_C_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
set(CMAKE_LINKER_TYPE LLD)

find_program(ISOLINE_QEMU_AARCH64 NAMES qemu-aarch64-static qemu-aarch64)
if(NOT ISOLINE_QEMU_AARCH64)
    message(FATAL_ERROR "qemu-aarch64-static is not installed: the tests of an aarch64 cross build run under it "
                        "(Fedora: dnf install qemu-user-static-aarch64; see docs/cross-compiling.md)")
endif()
set(CMAKE_CROSSCOMPILING_EMULATOR "${ISOLINE_QEMU_AARCH64};-L;${ISOLINE_AARCH64_SYSROOT}")
