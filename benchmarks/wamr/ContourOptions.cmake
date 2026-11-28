# SPDX-License-Identifier: Apache-2.0
#
# The options Contour builds its WAMR with (cmake/ContourThirdParties.cmake's CPMAddPackage at the commit
# benchmarks/wamr/patches.lock names), one `<cache variable> <value>` row each, as CPM's OPTIONS take them:
# the wamr-contour runner (runner/CMakeLists.txt) sets each before it adds WAMR's source tree, so that it
# runs the interpreter Contour ships. The wamr-default runner sets none of them and takes WAMR's own defaults.
#
# What they mean, as Contour's comments put it: the fast interpreter alone (no AOT, JIT or fast JIT);
# libc-wasi on and libc-builtin off; the thread manager on, without which the interpreter never polls for
# wasm_runtime_terminate() (as Isoline's interpreter polls its interrupt); no hardware bounds checks for
# memory or the native stack, and no wakeup of blocking operations, which install process-wide signal
# handlers; the WebAssembly features of Contour's EngineFeatures.hpp (bulk memory, reference types and
# extended constant expressions on; SIMD, shared memory, tail calls, multi-memory, memory64, GC, stringref
# and exception handling off); and no write of the GS base, which only AOT code reads.
set(ISOLINE_WAMR_CONTOUR_OPTIONS
    "WAMR_BUILD_INTERP 1"
    "WAMR_BUILD_FAST_INTERP 1"
    "WAMR_BUILD_AOT 0"
    "WAMR_BUILD_JIT 0"
    "WAMR_BUILD_FAST_JIT 0"
    "WAMR_BUILD_LIBC_WASI 1"
    "WAMR_BUILD_LIBC_BUILTIN 0"
    "WAMR_BUILD_THREAD_MGR 1"
    "WAMR_BUILD_LIB_PTHREAD 0"
    "WAMR_BUILD_LIB_WASI_THREADS 0"
    "WAMR_BUILD_MULTI_MODULE 0"
    "WAMR_BUILD_BULK_MEMORY 1"
    "WAMR_BUILD_REF_TYPES 1"
    "WAMR_BUILD_EXTENDED_CONST_EXPR 1"
    "WAMR_BUILD_SIMD 0"
    "WAMR_BUILD_SHARED_MEMORY 0"
    "WAMR_BUILD_TAIL_CALL 0"
    "WAMR_BUILD_MULTI_MEMORY 0"
    "WAMR_BUILD_MEMORY64 0"
    "WAMR_BUILD_GC 0"
    "WAMR_BUILD_STRINGREF 0"
    "WAMR_BUILD_EXCE_HANDLING 0"
    "WAMR_DISABLE_HW_BOUND_CHECK 1"
    "WAMR_DISABLE_STACK_HW_BOUND_CHECK 1"
    "WAMR_DISABLE_WAKEUP_BLOCKING_OP 1"
    "WAMR_DISABLE_WRITE_GS_BASE 1"
    "BUILD_SHARED_LIBS OFF")

# The wamr-classic runner: Contour's build with WAMR's classic interpreter in place of its fast one, the
# interpreter the reference interpreter's design resembles (a stack machine over the decoded bytecode).
set(ISOLINE_WAMR_CLASSIC_OPTIONS ${ISOLINE_WAMR_CONTOUR_OPTIONS})
list(TRANSFORM ISOLINE_WAMR_CLASSIC_OPTIONS REPLACE "^WAMR_BUILD_FAST_INTERP 1$" "WAMR_BUILD_FAST_INTERP 0")
