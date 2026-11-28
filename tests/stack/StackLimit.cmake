# SPDX-License-Identifier: Apache-2.0

# The host-stack and memory rows: unit cases hidden from the unit run ([.memory]) that run alone, each under
# a cap of the memory it may take, and, on Linux, under a small main-thread stack, because Wasm calls never
# recurse on the host stack (interpreter/Interpreter.hpp).
#
# The memory cap is the build's: under AddressSanitizer its own RSS limit (an address-space limit breaks its
# shadow memory), elsewhere on Linux `prlimit --as`. ThreadSanitizer, which reserves its address space too,
# and the platforms without prlimit run the case uncapped. The stack limit is `prlimit --stack`, which sets
# the main thread's limit before the process starts: Linux only, util-linux's, and not under a cross build's
# emulator, which gives its guest a stack of its own choosing.
#
# A row is the case, its stack limit in bytes, and its memory cap in MiB.
set(isolineStackLimitRows
    "Calls.deepRecursionDoesNotRecurseOnTheHostStack|1048576|4096")

find_program(ISOLINE_PRLIMIT prlimit)
set(isolinePrlimitUsable OFF)
if(ISOLINE_PRLIMIT AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    set(isolinePrlimitUsable ON)
endif()
foreach(row IN LISTS isolineStackLimitRows)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 stackCase)
    list(GET fields 1 stackBytes)
    list(GET fields 2 memoryMiB)
    set(limits)
    if(isolinePrlimitUsable)
        list(APPEND limits --stack=${stackBytes})
        if(NOT ISOLINE_SANITIZE)
            math(EXPR memoryBytes "${memoryMiB} * 1024 * 1024")
            list(APPEND limits --as=${memoryBytes})
        endif()
        # The binary by its path under prlimit; a cross build never comes here.
        add_test(NAME stack.${stackCase}
                 COMMAND "${ISOLINE_PRLIMIT}" ${limits} -- $<TARGET_FILE:isoline_test> "${stackCase}")
    else()
        # The target's name: CMake runs it through CMAKE_CROSSCOMPILING_EMULATOR (qemu, wine).
        add_test(NAME stack.${stackCase} COMMAND isoline_test "${stackCase}")
    endif()
    set_tests_properties(stack.${stackCase} PROPERTIES LABELS unit TIMEOUT 600)
    if(ISOLINE_SANITIZE STREQUAL "address")
        set_tests_properties(stack.${stackCase} PROPERTIES
                             ENVIRONMENT "ASAN_OPTIONS=hard_rss_limit_mb=${memoryMiB}:allocator_may_return_null=1")
    endif()
endforeach()
