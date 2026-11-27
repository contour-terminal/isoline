/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Errors of Isoline's C API, as values.
 *
 * Every fallible isoline_ function returns an owned isoline_error_t*, NULL on success, and writes its
 * results through out parameters. An error is an immutable value: it may be read from any thread, and
 * is deleted once, with isoline_error_delete(), by whoever received it.
 *
 * An error carries:
 *   - a code, an isoline_error_code_t, stable across versions: a code is never renumbered or reused, and
 *     new codes are appended;
 *   - a detail, a stable sub-code within the code: each code has at most one family of details, a C++
 *     enumeration named at the code below ("detail: ..."), and the detail is that enumerator's number
 *     plus one, so that 0 always means none. An error its family does not name, and every error of a
 *     code without a family, has detail 0. Details are appended within a family, never renumbered;
 *   - a message, UTF-8 text of at most 4 KiB, for people, not for programs to match;
 *   - a cause, the error it arose from, if any: a chain at most eight errors long.
 *
 * Out of host memory, an error cannot be allocated. Every function that would return one returns the
 * one immortal error of code ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED instead, which isoline_error_delete()
 * leaves alone: no error path needs memory, and no error reads as success.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <isoline/c/export.h>

/** The code of an error: one of the ISOLINE_ERROR_ constants. */
typedef uint32_t isoline_error_code_t;

/** The codes of errors. 0 is no error; the numbers are stable, and new codes are appended. */
enum isoline_error_code_enum
{
    /** An argument is not one the function accepts; detail: an ISOLINE_INVALID_TYPE_ constant for a type
     * refused, else 0. */
    ISOLINE_ERROR_INVALID_ARGUMENT = 1,
    /** The host ran out of memory. */
    ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED = 2,
    /** An option key the library does not know. */
    ISOLINE_ERROR_UNKNOWN_OPTION = 3,
    /** A feature enabled without one it needs. */
    ISOLINE_ERROR_UNMET_FEATURE_PREREQUISITE = 4,
    /** No engine of the kind asked for is available. */
    ISOLINE_ERROR_ENGINE_UNAVAILABLE = 5,
    /** A module past the engine's size limit. */
    ISOLINE_ERROR_MODULE_TOO_LARGE = 6,
    /** A module that is malformed; detail: the number docs/c-api.md (Errors) gives the decode error. */
    ISOLINE_ERROR_DECODE = 7,
    /** A module that is invalid; detail: the number docs/c-api.md (Errors) gives the validation error. */
    ISOLINE_ERROR_VALIDATION = 8,
    /** A module past what the engine compiles. */
    ISOLINE_ERROR_COMPILE_LIMIT_EXCEEDED = 9,
    /** A module loaded by an engine of another identity. */
    ISOLINE_ERROR_ENGINE_MISMATCH = 10,
    /** An entity or a handle of another store. */
    ISOLINE_ERROR_FOREIGN_STORE = 11,
    /** A handle whose store is deleted. */
    ISOLINE_ERROR_STORE_DELETED = 12,
    /** An import nothing was given for. */
    ISOLINE_ERROR_UNKNOWN_IMPORT = 13,
    /** An import given an extern of another type. */
    ISOLINE_ERROR_INCOMPATIBLE_IMPORT = 14,
    /** Not one extern per import. */
    ISOLINE_ERROR_IMPORT_COUNT_MISMATCH = 15,
    /** One of the store's limits would be exceeded; detail: the limit's number in docs/c-api.md (Errors). */
    ISOLINE_ERROR_LIMIT_EXCEEDED = 16,
    /** A memory or table could not be allocated; detail: the refusal's number in docs/c-api.md (Errors). */
    ISOLINE_ERROR_ALLOCATION_FAILURE = 17,
    /** A call into the guest failed; detail: the failure's number in docs/c-api.md (Errors). */
    ISOLINE_ERROR_CALL = 18,
    /** The thread could not be prepared; detail: the reason's number in docs/c-api.md (Errors). */
    ISOLINE_ERROR_THREAD_PREPARATION_UNAVAILABLE = 19,
    /** The host CPU lacks what the engine needs. */
    ISOLINE_ERROR_CPU_FEATURES_MISSING = 20,
    /** A value not of the type it must have; detail: the mismatch's number in docs/c-api.md (Errors). */
    ISOLINE_ERROR_VALUE_MISMATCH = 21,
    /** An index or range outside its entity. */
    ISOLINE_ERROR_OUT_OF_BOUNDS = 22,
    /** A memory or table was not grown; detail: the refusal's number in docs/c-api.md (Errors). */
    ISOLINE_ERROR_GROWTH_REFUSED = 23,
    /** A const global that was to be set. */
    ISOLINE_ERROR_IMMUTABLE_GLOBAL = 24,
    /** Precompiled code that was refused. */
    ISOLINE_ERROR_PRECOMPILED_REFUSED = 25,
    /** A cache key that could not be made. */
    ISOLINE_ERROR_CACHE_KEY = 26,
    /** The cache's storage failed. */
    ISOLINE_ERROR_CACHE_STORAGE = 27,
    /** A value the function cannot pass. */
    ISOLINE_ERROR_UNSUPPORTED_VALUE = 28,
    /** The embedder's own error (isoline_error_new). */
    ISOLINE_ERROR_EMBEDDER = 29,
    /** A call not allowed from inside a callback. */
    ISOLINE_ERROR_REENTRANT_CALL = 30,
    /** The highest code. */
    ISOLINE_ERROR_LAST = ISOLINE_ERROR_REENTRANT_CALL
};

/**
 * The details of an ISOLINE_ERROR_INVALID_ARGUMENT that refuses a type, naming why; stable, as the codes are.
 * Every family of details, these included, is published in docs/c-api.md (Errors).
 */
enum isoline_invalid_type_detail_enum
{
    /** The type names a concrete type index, which only a module's own types may name. */
    ISOLINE_INVALID_TYPE_CONCRETE_TYPE_INDEX = 1,
    /** A memory's page size is not one custom-page-sizes admits (1 byte, 64 KiB). */
    ISOLINE_INVALID_TYPE_PAGE_SIZE = 2,
    /** The type names the bottom heap, which no module may declare. */
    ISOLINE_INVALID_TYPE_BOTTOM_HEAP = 3,
    /** A table's element type is not a reference type. */
    ISOLINE_INVALID_TYPE_NON_REFERENCE_ELEMENT = 4,
};

#if defined(__cplusplus)
extern "C"
{
#endif

    /** An error: opaque, immutable, deleted once with isoline_error_delete(). */
    typedef struct isoline_error_t isoline_error_t;

    /**
     * Makes an error, as an embedder's callback reports one.
     * @param code Its code. An unknown one (0, or above ISOLINE_ERROR_LAST) gives an error of code
     *        ISOLINE_ERROR_INVALID_ARGUMENT instead, which says so.
     * @param message Its message's bytes, copied, NUL bytes included; NULL with @p length 0 for none.
     *        A message longer than 4 KiB is cut to the longest prefix of at most 4 KiB that ends at a
     *        UTF-8 character boundary. NULL with a nonzero @p length gives an error of code
     *        ISOLINE_ERROR_INVALID_ARGUMENT instead, which says so.
     * @param length The bytes of @p message.
     * @return The error, owned by the caller; never NULL. Out of host memory, the immortal error of code
     *         ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED.
     */
    ISOLINE_API isoline_error_t* ISOLINE_CALL isoline_error_new(isoline_error_code_t code,
                                                                char const* message,
                                                                size_t length) ISOLINE_NOEXCEPT;

    /**
     * Deletes @p error and its causes. Does nothing for NULL and for the immortal error.
     * @param error An error the caller owns, or NULL.
     */
    ISOLINE_API void ISOLINE_CALL isoline_error_delete(isoline_error_t* error) ISOLINE_NOEXCEPT;

    /** The code of @p error; 0 for NULL. */
    ISOLINE_API isoline_error_code_t ISOLINE_CALL isoline_error_code(isoline_error_t const* error)
        ISOLINE_NOEXCEPT;

    /**
     * The detail of @p error: a number, stable as the codes are, that its code's family of details gives the
     * error's cause (docs/c-api.md, Errors, lists every family); 0 when its code has no family, when the
     * family names no cause of it, and for NULL.
     */
    ISOLINE_API uint32_t ISOLINE_CALL isoline_error_detail(isoline_error_t const* error) ISOLINE_NOEXCEPT;

    /**
     * The name of @p code: the constant's name after ISOLINE_ERROR_, in lower case ("decode" for
     * ISOLINE_ERROR_DECODE). A static string; NULL for a code the library does not know.
     */
    ISOLINE_API const char* ISOLINE_CALL isoline_error_code_name(isoline_error_code_t code) ISOLINE_NOEXCEPT;

    /**
     * The message of @p error: UTF-8, NUL-terminated, at most 4 KiB before the terminator, borrowed for
     * as long as @p error lives. It may hold NUL bytes of its own: isoline_error_message_length() is its
     * length. "" for NULL.
     */
    ISOLINE_API const char* ISOLINE_CALL isoline_error_message(isoline_error_t const* error) ISOLINE_NOEXCEPT;

    /** The bytes of @p error's message, without the terminator; 0 for NULL. */
    ISOLINE_API size_t ISOLINE_CALL isoline_error_message_length(isoline_error_t const* error)
        ISOLINE_NOEXCEPT;

    /**
     * The error @p error arose from, borrowed for as long as @p error lives; NULL when there is none, and
     * for NULL. Following causes ends after at most eight errors, @p error included.
     */
    ISOLINE_API const isoline_error_t* ISOLINE_CALL isoline_error_cause(isoline_error_t const* error)
        ISOLINE_NOEXCEPT;

#if defined(__cplusplus)
}
#endif
