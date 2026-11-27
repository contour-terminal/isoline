# SPDX-License-Identifier: Apache-2.0

# The host-stack rows: unit cases that must pass on a small main-thread stack, because Wasm calls never
# recurse on the host stack (fastinterpreter/FastInterpreter.hpp). Each row runs one case of isoline_test
# under `prlimit --stack`, which sets the main thread's stack limit before the process starts. Linux only:
# prlimit is util-linux's, and a cross build's emulator gives its guest a stack of its own choosing.
#
# A row is the case, then its stack limit in bytes.
set(isolineStackLimitRows
    "Calls.deepRecursionDoesNotRecurseOnTheHostStack|1048576")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    find_program(ISOLINE_PRLIMIT prlimit)
    if(ISOLINE_PRLIMIT)
        foreach(row IN LISTS isolineStackLimitRows)
            string(REPLACE "|" ";" fields "${row}")
            list(GET fields 0 stackCase)
            list(GET fields 1 stackBytes)
            add_test(NAME stack.${stackCase}
                     COMMAND "${ISOLINE_PRLIMIT}" --stack=${stackBytes} -- $<TARGET_FILE:isoline_test> "${stackCase}")
            set_tests_properties(stack.${stackCase} PROPERTIES LABELS unit TIMEOUT 600)
        endforeach()
    else()
        message(STATUS "[isoline] no prlimit: the host-stack rows (tests/stack/StackLimit.cmake) do not run")
    endif()
endif()
