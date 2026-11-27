/* SPDX-License-Identifier: Apache-2.0 */
/*
 * A callback compiled as C, as an embedder's is, for the C++ test that calls it through the C API's
 * trampoline (detail/CallingConvention_test.cpp): were the convention the C side defines it with not the one
 * the trampoline calls it with, its arguments or its result would arrive garbled there. Part of
 * isoline_c_test alone; the library has no such function.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <isoline/c/export.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /** What callingConventionProbe() received, written through its context. */
    struct CallingConventionReceived
    {
        void* context;   /**< The context it was called with. */
        uint8_t kind;    /**< Its one-byte argument. */
        bool isSet;      /**< Its bool argument. */
        uint32_t narrow; /**< Its 32-bit argument. */
        uint64_t wide;   /**< Its 64-bit argument. */
        double real;     /**< Its floating-point argument. */
        int calls;       /**< How often it was called. */
    };

    /**
     * Records its arguments in the struct CallingConventionReceived its context points to, as a host
     * function's callback would read them.
     * @return wide ^ narrow, so that the result carries both.
     */
    uint64_t ISOLINE_CALL callingConventionProbe(
        void* context, uint8_t kind, bool isSet, uint32_t narrow, uint64_t wide, double real);

#ifdef __cplusplus
}
#endif
