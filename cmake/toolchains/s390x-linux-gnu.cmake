# SPDX-License-Identifier: Apache-2.0
#
# Cross-compiles big-endian s390x Linux executables on an x86_64 Debian or Ubuntu host with GCC 14's cross
# compiler (g++-14-s390x-linux-gnu), whose target libraries live in /usr/s390x-linux-gnu: the Debian layout,
# which is no sysroot but a prefix the cross compiler already searches.
#
# Test binaries run under qemu user mode (qemu-user or qemu-user-static): CMAKE_CROSSCOMPILING_EMULATOR is
# set, so ctest runs them as it would on an s390x machine. On a host without the packages, use
# scripts/s390x-podman.sh, which runs configure, build and tests in an Ubuntu container that has them.
#
# @see docs/cross-compiling.md

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR s390x)

set(ISOLINE_S390X_PREFIX "/usr/s390x-linux-gnu" CACHE PATH "The s390x target libraries of the cross compiler")
if(NOT IS_DIRECTORY "${ISOLINE_S390X_PREFIX}/lib")
    message(FATAL_ERROR "ISOLINE_S390X_PREFIX=${ISOLINE_S390X_PREFIX} holds no s390x libraries: install "
                        "g++-14-s390x-linux-gnu (Debian, Ubuntu), or run scripts/s390x-podman.sh")
endif()

# Dependencies come from the target prefix, never from the host; programs (Python, qemu) from the host.
set(CMAKE_FIND_ROOT_PATH "${ISOLINE_S390X_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

find_program(CMAKE_C_COMPILER NAMES s390x-linux-gnu-gcc-14 REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES s390x-linux-gnu-g++-14 REQUIRED)

find_program(ISOLINE_QEMU_S390X NAMES qemu-s390x-static qemu-s390x)
if(NOT ISOLINE_QEMU_S390X)
    message(FATAL_ERROR "qemu-s390x is not installed: the tests of an s390x cross build run under it "
                        "(Debian, Ubuntu: apt-get install qemu-user; see docs/cross-compiling.md)")
endif()
set(CMAKE_CROSSCOMPILING_EMULATOR "${ISOLINE_QEMU_S390X};-L;${ISOLINE_S390X_PREFIX}")
# The environment a test needs for the programs it starts itself: the kernel's binfmt handler runs them
# under qemu without the emulator's -L, so qemu reads the prefix from QEMU_LD_PREFIX instead.
set(ISOLINE_EMULATED_CHILD_ENVIRONMENT "QEMU_LD_PREFIX=${ISOLINE_S390X_PREFIX}" CACHE INTERNAL
    "The environment of a program a test starts itself under the emulator")
