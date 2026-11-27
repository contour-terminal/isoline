/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The annotations every function of Isoline's C API carries.
 *
 * ISOLINE_API     makes a declaration part of the library's exported interface: dllexport while the
 *                 library itself is built (ISOLINE_C_BUILDING), dllimport for a program using the DLL,
 *                 and nothing for a program linking the static library (ISOLINE_C_STATIC). On ELF and
 *                 Mach-O it is default visibility; the shared library's version script or exported
 *                 symbols list hides every symbol it does not name.
 * ISOLINE_CALL    the calling convention of every isoline_ function, every isoline_ callback type and
 *                 every finalizer parameter: __cdecl on Windows, the platform's only one elsewhere.
 *                 A program compiled with /Gv (vectorcall by default) is not supported: wasm.h's own
 *                 functions cannot carry a convention.
 * ISOLINE_NOEXCEPT what every isoline_ declaration ends with: noexcept in C++, nothing in C. No C++
 *                 exception ever crosses the boundary.
 * WASM_API_EXTERN wasm.h's annotation of its own functions, defined here to ISOLINE_API unless already
 *                 defined, so one definition of ISOLINE_C_STATIC selects the static library for both.
 *                 Every header of the C API includes this one before wasm.h. A program that includes
 *                 wasm.h first, or alone, gets wasm.h's own annotation, which LIBWASM_STATIC switches
 *                 the same way.
 *
 * Like wasm.h's WASM_API_EXTERN, ISOLINE_API may be defined before this header is included; the
 * library's symbol lint (scripts/c-api-symbols.py) does so to find every declaration.
 */
#pragma once

#if !defined(ISOLINE_API)
    #if defined(ISOLINE_C_STATIC)
        #define ISOLINE_API
    #elif defined(_WIN32)
        #if defined(ISOLINE_C_BUILDING)
            #define ISOLINE_API __declspec(dllexport)
        #else
            #define ISOLINE_API __declspec(dllimport)
        #endif
    #else
        #define ISOLINE_API __attribute__((visibility("default")))
    #endif
#endif

#if defined(_WIN32)
    #define ISOLINE_CALL __cdecl
#else
    #define ISOLINE_CALL
#endif

#if defined(__cplusplus)
    #define ISOLINE_NOEXCEPT noexcept
#else
    #define ISOLINE_NOEXCEPT
#endif

#if !defined(WASM_API_EXTERN)
    #define WASM_API_EXTERN ISOLINE_API
#endif
