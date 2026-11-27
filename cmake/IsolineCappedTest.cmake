# SPDX-License-Identifier: Apache-2.0
#
# isoline_add_capped_test(): a test that runs under a hard memory cap, so that a memory-bound case whose
# bound is broken fails small and fast instead of starving a shared machine.
#
# How a build caps a test is data: one row of ISOLINE_MEMORY_CAP_ROWS per kind of build,
#     kind|mechanism|value|reason
# where the mechanism is `environment` (value is the variable assignment the test runs with), `prlimit`
# (value is prlimit's limit argument) or `none` (value is empty, and the reason says why). The build's
# kind is the first that applies: cross (CMAKE_CROSSCOMPILING, with or without an emulator), then the
# sanitizer (address, thread), then native (prlimit found), then uncapped. A new kind of build is a row and a selection rule.

set(ISOLINE_MEMORY_CAP_ROWS
    "cross|none||a cross build runs its tests under an emulator (wine, qemu) or on another machine: a host prlimit would cap the emulator, not the test, and the build has no sanitizer"
    "address|environment|ASAN_OPTIONS=hard_rss_limit_mb=4096:allocator_may_return_null=1|AddressSanitizer's RSS cap, refused allocations returning null"
    "thread|environment|TSAN_OPTIONS=hard_rss_limit_mb=4096:allocator_may_return_null=1|ThreadSanitizer's RSS cap, refused allocations returning null"
    "native|prlimit|--as=12884901888|a 12 GiB address-space limit: room for two Wasmtime-parity reservations (4 GiB + 32 MiB each) beside the process"
    "uncapped|none||the host has no prlimit (Windows, macOS): the test runs without a cap")

find_program(ISOLINE_PRLIMIT prlimit)

# The kind of this build, as ISOLINE_MEMORY_CAP_ROWS names it.
if(CMAKE_CROSSCOMPILING)
    set(isolineMemoryCapKind cross)
elseif(ISOLINE_SANITIZE STREQUAL "address" OR ISOLINE_SANITIZE STREQUAL "thread")
    set(isolineMemoryCapKind "${ISOLINE_SANITIZE}")
elseif(ISOLINE_PRLIMIT)
    set(isolineMemoryCapKind native)
else()
    set(isolineMemoryCapKind uncapped)
endif()

set(isolineMemoryCapRow "")
foreach(row IN LISTS ISOLINE_MEMORY_CAP_ROWS)
    if(row MATCHES "^${isolineMemoryCapKind}[|]([a-z]+)[|]([^|]*)[|](.+)$")
        set(isolineMemoryCapMechanism "${CMAKE_MATCH_1}")
        set(isolineMemoryCapValue "${CMAKE_MATCH_2}")
        set(isolineMemoryCapReason "${CMAKE_MATCH_3}")
        set(isolineMemoryCapRow "${row}")
    endif()
endforeach()
if(isolineMemoryCapRow STREQUAL "")
    message(FATAL_ERROR "ISOLINE_MEMORY_CAP_ROWS has no row for the build kind '${isolineMemoryCapKind}'")
endif()
message(STATUS "[isoline] memory-capped tests: ${isolineMemoryCapKind} (${isolineMemoryCapMechanism}"
               " ${isolineMemoryCapValue}): ${isolineMemoryCapReason}")

# Adds test NAME, running TARGET with ARGS, under the build's memory cap (ISOLINE_MEMORY_CAP_ROWS). The
# test carries the label `memory` beside LABELS, and TIMEOUT (default 600 seconds).
function(isoline_add_capped_test)
    cmake_parse_arguments(PARSE_ARGV 0 capped "" "NAME;TARGET;TIMEOUT" "ARGS;LABELS")
    if(NOT capped_NAME OR NOT capped_TARGET)
        message(FATAL_ERROR "isoline_add_capped_test: NAME and TARGET are required")
    endif()
    if(NOT capped_TIMEOUT)
        set(capped_TIMEOUT 600)
    endif()
    if(isolineMemoryCapMechanism STREQUAL "prlimit")
        add_test(NAME ${capped_NAME}
                 COMMAND "${ISOLINE_PRLIMIT}" ${isolineMemoryCapValue} -- $<TARGET_FILE:${capped_TARGET}>
                         ${capped_ARGS})
    else()
        add_test(NAME ${capped_NAME} COMMAND ${capped_TARGET} ${capped_ARGS})
    endif()
    if(isolineMemoryCapMechanism STREQUAL "environment")
        set_tests_properties(${capped_NAME} PROPERTIES ENVIRONMENT "${isolineMemoryCapValue}")
    endif()
    set_tests_properties(${capped_NAME} PROPERTIES LABELS "${capped_LABELS};memory" TIMEOUT ${capped_TIMEOUT})
endfunction()
