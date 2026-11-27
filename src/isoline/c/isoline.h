/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Isoline's C API: the standard WebAssembly C API (wasm.h, wasm-c-api revision 9d6b937) and the
 * isoline_ functions for what wasm.h lacks.
 *
 * Include this header rather than wasm.h alone: it gives wasm.h's functions the same export annotation
 * as Isoline's own (ISOLINE_API, through export.h), so one definition of ISOLINE_C_STATIC selects the
 * static library for both. A program that includes wasm.h by itself, before this header or instead of
 * it, gets wasm.h's own annotation, which LIBWASM_STATIC switches the same way.
 */
#pragma once

#include <isoline/c/abi.h>
#include <isoline/c/error.h>
#include <isoline/c/export.h>

/* After export.h, which gives wasm.h's WASM_API_EXTERN the C API's annotation. */
#include <wasm.h>
