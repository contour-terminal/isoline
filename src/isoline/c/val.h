/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Value types, and the types of memories and tables, beyond what wasm.h can say.
 *
 * Kinds. wasm.h's wasm_valkind_t names i32, i64, f32, f64 (0-3), externref and funcref (128, 129), and
 * nothing for v128 or exnref; it counts a kind below 128 as a number (wasm_valkind_is_num()) and every other
 * as a reference (wasm_valkind_is_ref()). Isoline names the two in its own numbers, each on the side of 128
 * that wasm.h's tests read right, until upstream numbers them: ISOLINE_VALKIND_V128 is 0x7B, v128's byte in
 * the binary format, a number to wasm_valkind_is_num(); ISOLINE_VALKIND_EXNREF is 0xF1, from the range
 * 0xF0-0xFF Isoline keeps for itself, a reference. wasm_valtype_new() of any other kind gives NULL.
 * ISOLINE_VALKIND_INVALID, what wasm_valtype_kind(NULL) gives, is 0xFF, which wasm_valkind_is_ref() calls a
 * reference: test for it before asking wasm.h; isoline_valtype_heap() is the reliable test of a type.
 *
 * References. A value type of a reference kind has a heap and a nullability, which wasm.h does not report:
 * wasm_valtype_kind() names its hierarchy (funcref, externref or exnref), and isoline_valtype_heap() and
 * isoline_valtype_nullability() the rest, so that (ref func), nullfuncref and (ref $t) of a module are
 * told apart. wasm_valtype_new() makes the nullable abstract references alone.
 *
 * Limits. wasm.h's wasm_limits_t holds 32-bit sizes. A memory or table whose sizes need more (memory64,
 * table64) reports through wasm_memorytype_limits() and wasm_tabletype_limits() each size past UINT32_MAX
 * as wasm_limits_max_default, and its maximum, when it has none, as wasm_limits_max_default too; the
 * isoline_*_limits64() functions give the sizes exactly.
 *
 * What a type function given NULL returns is named at each: NULL, or a value no type has.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <isoline/c/error.h>
#include <isoline/c/export.h>

/* After export.h, which gives wasm.h's WASM_API_EXTERN the C API's annotation. */
#include <wasm.h>

/** The kinds of value wasm.h has no number for, in Isoline's numbers of wasm_valkind_t. */
enum isoline_valkind_enum
{
    /** The 128-bit vector type: 0x7B, its byte in the binary format, below 128, so a number to wasm.h. */
    ISOLINE_VALKIND_V128 = 0x7B,
    /** exnref, a nullable reference to any exception. */
    ISOLINE_VALKIND_EXNREF = 0xF1,
    /**
     * What wasm_valtype_kind() gives for NULL: no value type has it, and wasm_valtype_new() refuses it. Being
     * 0xFF, wasm_valkind_is_ref() reports it as a reference.
     */
    ISOLINE_VALKIND_INVALID = 0xFF,
};

/** What wasm_externtype_kind() gives for NULL, of wasm_externkind_t: no extern type has it. */
#define ISOLINE_EXTERNKIND_INVALID ((wasm_externkind_t) 0xFF)

/** What wasm_globaltype_mutability() gives for NULL, of wasm_mutability_t: neither WASM_CONST nor WASM_VAR.
 */
#define ISOLINE_MUTABILITY_INVALID ((wasm_mutability_t) 0xFF)

/** The heap a reference type points into: one of the ISOLINE_HEAP_ constants. */
typedef uint8_t isoline_heap_t;

/** The heaps of reference types. */
enum isoline_heap_enum
{
    /** Any function: funcref's heap. */
    ISOLINE_HEAP_FUNC,
    /** Any host object: externref's heap. */
    ISOLINE_HEAP_EXTERN,
    /** Any exception: exnref's heap. */
    ISOLINE_HEAP_EXN,
    /** A function type of a module, named by isoline_valtype_concrete_index(). */
    ISOLINE_HEAP_CONCRETE,
    /** No function: the heap of nullfuncref, whose one value is null. */
    ISOLINE_HEAP_NOFUNC,
    /** No host object: the heap of nullexternref. */
    ISOLINE_HEAP_NOEXTERN,
    /** No exception: the heap of nullexnref. */
    ISOLINE_HEAP_NOEXN,
    /** No heap: the type is a number or vector type, not a reference type (and what NULL gives). */
    ISOLINE_HEAP_NOT_REFERENCE,
};

/** Whether a reference type admits null: one of the ISOLINE_ nullability constants. */
typedef uint8_t isoline_nullability_t;

/** Whether a reference type admits null. */
enum isoline_nullability_enum
{
    /** It does not; numbers and vectors, which are never null, report this too. */
    ISOLINE_NOT_NULLABLE,
    /** It does. */
    ISOLINE_NULLABLE,
};

/** Whether limits have a maximum: one of the ISOLINE_MAXIMUM_ constants. */
typedef uint8_t isoline_maximum_kind_t;

/** Whether limits have a maximum. */
enum isoline_maximum_kind_enum
{
    /** They have none: the size may grow to what the address type reaches. maximum is 0. */
    ISOLINE_MAXIMUM_NONE,
    /** maximum is the largest size. */
    ISOLINE_MAXIMUM_GIVEN,
};

/**
 * The size bounds of a memory, in pages, or of a table, in elements, at 64 bits.
 *
 * Size first: a caller sets struct_size to sizeof(isoline_limits64_t) of the header it was built with. The
 * library accepts any struct_size from 32, the size this struct was first published with, up to 4096; any
 * other is ISOLINE_ERROR_INVALID_ARGUMENT. It reads and writes only the fields that fit in struct_size, so a
 * caller built against an older header, whose struct is smaller, gets the prefix it knows. Read as input, a
 * nonzero reserved byte, a nonzero maximum without ISOLINE_MAXIMUM_GIVEN, and, in a struct larger than the
 * library's, a nonzero byte past its fields (a later minor's field it does not know) are
 * ISOLINE_ERROR_INVALID_ARGUMENT. Written as output, every byte past the library's fields, up to
 * struct_size, is zeroed, and nothing past struct_size is written.
 */
typedef struct isoline_limits64_t
{
    size_t struct_size; /**< sizeof(isoline_limits64_t), as the caller was built with it: 32 to 4096. */
    uint64_t minimum;   /**< The initial size. */
    uint64_t maximum;   /**< The largest size, when maximum_kind is ISOLINE_MAXIMUM_GIVEN. */
    isoline_maximum_kind_t maximum_kind; /**< Whether maximum is given. */
    uint8_t reserved[7];                 /**< Zero. */
} isoline_limits64_t;

#if defined(__cplusplus)
extern "C"
{
#endif

    /**
     * Whether the reference type @p type admits null.
     * @return ISOLINE_NULLABLE or ISOLINE_NOT_NULLABLE; ISOLINE_NOT_NULLABLE for a number or vector type and
     *         for NULL.
     */
    ISOLINE_API isoline_nullability_t ISOLINE_CALL isoline_valtype_nullability(wasm_valtype_t const* type)
        ISOLINE_NOEXCEPT;

    /**
     * The heap the reference type @p type points into.
     * @return An ISOLINE_HEAP_ constant; ISOLINE_HEAP_NOT_REFERENCE for a number or vector type and for NULL.
     */
    ISOLINE_API isoline_heap_t ISOLINE_CALL isoline_valtype_heap(wasm_valtype_t const* type) ISOLINE_NOEXCEPT;

    /**
     * The index of the function type the reference type @p type points to, in the type section of the
     * module that gave it.
     * @return The index when isoline_valtype_heap() is ISOLINE_HEAP_CONCRETE; UINT32_MAX otherwise, and for
     *         NULL.
     */
    ISOLINE_API uint32_t ISOLINE_CALL isoline_valtype_concrete_index(wasm_valtype_t const* type)
        ISOLINE_NOEXCEPT;

    /**
     * Makes a memory type of any address type and page size, which wasm_memorytype_new() cannot.
     * @param limits Its size bounds, in pages (read as isoline_limits64_t says).
     * @param addressBits The width of its addresses: 32, or 64 for memory64.
     * @param pageSizeLog2 The base-2 logarithm of its page size in bytes: 16, or 0 for 1-byte pages
     *        (custom-page-sizes).
     * @param outType Receives the type, owned by the caller; untouched on an error.
     * @return NULL on success. ISOLINE_ERROR_INVALID_ARGUMENT for a NULL argument, an address width or page
     *         size there is none of (detail ISOLINE_INVALID_TYPE_PAGE_SIZE, 2, for the page size), a
     *         minimum above the maximum, or a size past the most pages the address type reaches; out of
     *         host memory, the immortal ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED.
     */
    ISOLINE_API isoline_error_t* ISOLINE_CALL isoline_memorytype_new(isoline_limits64_t const* limits,
                                                                     uint32_t addressBits,
                                                                     uint32_t pageSizeLog2,
                                                                     wasm_memorytype_t** outType)
        ISOLINE_NOEXCEPT;

    /**
     * The exact size bounds of the memory type @p type.
     * @param outLimits Receives them; its struct_size is set by the caller, and only the fields that fit in
     *        it are written (isoline_limits64_t).
     * @return NULL on success; ISOLINE_ERROR_INVALID_ARGUMENT for a NULL argument or a struct_size below 32
     *         or above 4096, with @p outLimits untouched.
     */
    ISOLINE_API isoline_error_t* ISOLINE_CALL isoline_memorytype_limits64(
        wasm_memorytype_t const* type, isoline_limits64_t* outLimits) ISOLINE_NOEXCEPT;

    /**
     * The base-2 logarithm of the page size of the memory type @p type, in bytes.
     * @return 16 for 64 KiB pages, 0 for 1-byte ones; UINT32_MAX for NULL.
     */
    ISOLINE_API uint32_t ISOLINE_CALL isoline_memorytype_page_size_log2(wasm_memorytype_t const* type)
        ISOLINE_NOEXCEPT;

    /**
     * The width of the addresses of the memory type @p type.
     * @return 32, or 64 for memory64; 0 for NULL.
     */
    ISOLINE_API uint32_t ISOLINE_CALL isoline_memorytype_address_bits(wasm_memorytype_t const* type)
        ISOLINE_NOEXCEPT;

    /**
     * Makes a table type of any address type, which wasm_tabletype_new() cannot.
     * @param element The type of its elements, a reference type, which the table type takes on success; on
     *        an error it stays the caller's.
     * @param limits Its size bounds, in elements (read as isoline_limits64_t says).
     * @param addressBits The width of its indices: 32, or 64 for table64.
     * @param outType Receives the type, owned by the caller; untouched on an error.
     * @return NULL on success. ISOLINE_ERROR_INVALID_ARGUMENT for a NULL argument, an element type that is
     *         no reference type (detail ISOLINE_INVALID_TYPE_NON_REFERENCE_ELEMENT, 4), an address width
     *         there is none of, or a minimum above the maximum or a size past what the address type
     *         reaches; out of host memory, the immortal ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED.
     */
    ISOLINE_API isoline_error_t* ISOLINE_CALL isoline_tabletype_new(wasm_valtype_t* element,
                                                                    isoline_limits64_t const* limits,
                                                                    uint32_t addressBits,
                                                                    wasm_tabletype_t** outType)
        ISOLINE_NOEXCEPT;

    /**
     * The exact size bounds of the table type @p type.
     * @param outLimits Receives them; its struct_size is set by the caller, and only the fields that fit in
     *        it are written (isoline_limits64_t).
     * @return NULL on success; ISOLINE_ERROR_INVALID_ARGUMENT for a NULL argument or a struct_size below 32
     *         or above 4096, with @p outLimits untouched.
     */
    ISOLINE_API isoline_error_t* ISOLINE_CALL
    isoline_tabletype_limits64(wasm_tabletype_t const* type, isoline_limits64_t* outLimits) ISOLINE_NOEXCEPT;

    /**
     * The width of the indices of the table type @p type.
     * @return 32, or 64 for table64; 0 for NULL.
     */
    ISOLINE_API uint32_t ISOLINE_CALL isoline_tabletype_address_bits(wasm_tabletype_t const* type)
        ISOLINE_NOEXCEPT;

#if defined(__cplusplus)
}
#endif
