/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The ABI version of Isoline's C API: the version a program is compiled against, and the library's.
 *
 * ISOLINE_ABI_VERSION is (major << 16) | minor. Before 1.0 every minor may break the one before it, so
 * a library serves exactly the minor it was built with; from 1.0 a library serves its own major at
 * every minor up to its own. The library's file names carry the same version (libisoline_c.so.0.<minor>,
 * libisoline_c.0.<minor>.dylib, isoline_c-0.<minor>.dll before 1.0), so the loader refuses a library of
 * another ABI; isoline_abi_compatible() is the check for a program that loads it by hand (dlopen,
 * ctypes), where no file name is checked.
 *
 * The C API is 64-bit only, and its layout is the same on every target it supports (SysV x86-64,
 * AAPCS64, Windows x64): a pointer and size_t are 8 bytes, bool and wasm.h's kind enumerations 1. This
 * header asserts those, so a program compiled for another data model does not compile. The layout of
 * each struct is pinned by the rows of abi/layout.def, which the tests check.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <isoline/c/export.h>

/* After export.h, which gives wasm.h's WASM_API_EXTERN the C API's annotation. */
#include <wasm.h>

#define ISOLINE_ABI_VERSION_MAJOR 0
#define ISOLINE_ABI_VERSION_MINOR 1
#define ISOLINE_ABI_VERSION       ((ISOLINE_ABI_VERSION_MAJOR << 16) | ISOLINE_ABI_VERSION_MINOR)

#if defined(__cplusplus)
static_assert(sizeof(void*) == 8, "Isoline's C API is 64-bit only");
static_assert(sizeof(bool) == 1, "Isoline's C API passes bool as one byte");
static_assert(sizeof(wasm_valkind_t) == 1, "wasm.h's wasm_valkind_t is one byte");
#else
_Static_assert(sizeof(void*) == 8, "Isoline's C API is 64-bit only");
_Static_assert(sizeof(bool) == 1, "Isoline's C API passes bool as one byte");
_Static_assert(sizeof(wasm_valkind_t) == 1, "wasm.h's wasm_valkind_t is one byte");
#endif

#if defined(__cplusplus)
extern "C"
{
#endif

    /** The ABI version of the library loaded: ISOLINE_ABI_VERSION as the library was built. */
    ISOLINE_API uint32_t ISOLINE_CALL isoline_abi_version(void) ISOLINE_NOEXCEPT;

    /**
     * Whether the library loaded serves a program compiled against @p headerVersion: the majors are equal
     * and the library's minor is at least the header's, and before 1.0 the minors are equal.
     * Pass ISOLINE_ABI_VERSION.
     */
    ISOLINE_API bool ISOLINE_CALL isoline_abi_compatible(uint32_t headerVersion) ISOLINE_NOEXCEPT;

#if defined(__cplusplus)
}
#endif
