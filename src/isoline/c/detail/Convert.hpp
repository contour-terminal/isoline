// SPDX-License-Identifier: Apache-2.0
//
// Between the C API's types and the C++ ones: each conversion reads the rows of detail/EnumRows.hpp, so a C
// value without a row is refused (std::nullopt, or a TypeRefusal), never cast to an enumeration it may not
// name. What wasm.h's 32-bit wasm_limits_t cannot hold is clamped one way only, to the C side.
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

#include <isoline/api/Store.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/c/detail/Handles.hpp>
#include <isoline/c/isoline.h>

namespace isoline::c::detail
{

// --- kinds -----------------------------------------------------------------------

/// The C kind of @p type: its row's, or for a reference type the row of its hierarchy's top (HeapRows).
/// @param type A value type whose heap, if it is a reference type, has a row of HeapRows.
/// @return The kind; ISOLINE_VALKIND_INVALID for a heap without a row.
[[nodiscard]] auto toCKind(binary::ValueType const& type) noexcept -> wasm_valkind_t;

/// The value type wasm_valtype_new() makes of @p kind.
/// @param kind Any value.
/// @return The type of @p kind's row; std::nullopt when no row has it.
[[nodiscard]] auto fromCKind(wasm_valkind_t kind) noexcept -> std::optional<binary::ValueType>;

/// The C heap of @p type.
/// @return Its heap's ISOLINE_HEAP_ constant; ISOLINE_HEAP_NOT_REFERENCE for a number or vector type, and
///         for a heap without a row.
[[nodiscard]] auto toCHeap(binary::ValueType const& type) noexcept -> isoline_heap_t;

/// The C nullability of @p type.
/// @return ISOLINE_NULLABLE for a reference type that admits null, ISOLINE_NOT_NULLABLE otherwise.
[[nodiscard]] auto toCNullability(binary::ValueType const& type) noexcept -> isoline_nullability_t;

/// wasm.h's kind of extern for @p kind.
[[nodiscard]] auto toCExternKind(binary::ExternKind kind) noexcept -> wasm_externkind_t;

/// The kind of extern @p kind names.
/// @return The kind of its row; std::nullopt when no row has it.
[[nodiscard]] auto fromCExternKind(wasm_externkind_t kind) noexcept -> std::optional<binary::ExternKind>;

/// wasm.h's mutability for @p mutability.
[[nodiscard]] auto toCMutability(binary::Mutability mutability) noexcept -> wasm_mutability_t;

/// The mutability @p mutability names.
/// @return The mutability of its row; std::nullopt when no row has it.
[[nodiscard]] auto fromCMutability(wasm_mutability_t mutability) noexcept
    -> std::optional<binary::Mutability>;

/// The width of the addresses of @p addressType: 32 or 64.
[[nodiscard]] auto toCAddressBits(binary::AddressType addressType) noexcept -> uint32_t;

/// The address type of addresses @p bits wide.
/// @return The address type of its row; std::nullopt when no row has it.
[[nodiscard]] auto fromCAddressBits(uint32_t bits) noexcept -> std::optional<binary::AddressType>;

// --- value and function types ----------------------------------------------------

/// The C API's object of @p type.
/// @param type Any value type.
/// @return The object, owned by the caller; null when @p type has no C form (the bottom heap, which no
///         module declares). Throws std::bad_alloc when the host has no memory for it.
[[nodiscard]] auto toCType(binary::ValueType const& type) -> std::unique_ptr<wasm_valtype_t>;

/// The value type @p type is.
[[nodiscard]] auto fromCType(wasm_valtype_t const& type) noexcept -> binary::ValueType;

/// The C API's object of @p type.
/// @param type Any function type.
/// @return The object, owned by the caller; null when one of its value types has no C form. Throws
///         std::bad_alloc when the host has no memory for it.
[[nodiscard]] auto toCType(binary::FuncType const& type) -> std::unique_ptr<wasm_functype_t>;

/// The function type @p type is. Throws std::bad_alloc when the host has no memory for it.
[[nodiscard]] auto fromCType(wasm_functype_t const& type) -> binary::FuncType;

// --- limits ----------------------------------------------------------------------

/// Why a type the host describes is refused: what an isoline_ function reports as INVALID_ARGUMENT, and a
/// wasm_ one as NULL.
enum class TypeRefusal : uint8_t
{
    NullArgument,        ///< A pointer the function needs is NULL.
    StructSize,          ///< An isoline_limits64_t's struct_size is below this header's or above 4 KiB.
    NonzeroReserved,     ///< An isoline_limits64_t's reserved byte is not zero.
    UnknownField,        ///< An isoline_limits64_t holds a nonzero byte past the fields the library knows.
    UnknownMaximumKind,  ///< An isoline_limits64_t's maximum_kind is no ISOLINE_MAXIMUM_ constant.
    MaximumWithoutKind,  ///< An isoline_limits64_t's maximum is nonzero with ISOLINE_MAXIMUM_NONE.
    AddressBits,         ///< An address width other than 32 and 64.
    PageSize,            ///< A page size custom-page-sizes does not admit.
    MinimumAboveMaximum, ///< The minimum exceeds the maximum.
    SizePastAddressType, ///< A size past the most its address type reaches.
    NonReferenceElement, ///< A table's element type is not a reference type.
};

/// The C API's error for @p refusal, of code ISOLINE_ERROR_INVALID_ARGUMENT, and of the detail of the
/// api::InvalidTypeReason that names it, if one does.
/// @param refusal The refusal.
/// @return The error, owned by the caller; never null.
[[nodiscard]] auto typeRefusalError(TypeRefusal refusal) noexcept -> isoline_error_t*;

/// (toCLimits(), the one clamp to wasm_limits_t, is detail/Handles.hpp's: the types hold its result.)

/// The 32-bit limits @p limits stand for: a maximum of wasm_limits_max_default is none.
[[nodiscard]] auto fromCLimits(wasm_limits_t const& limits) noexcept -> binary::Limits;

/// Reads @p limits, as isoline_limits64_t says an input is read, for an entity of @p addressType.
/// @param limits The caller's limits; null is refused.
/// @param addressType The address type of the memory or table they bound.
/// @return The limits; their refusal otherwise.
[[nodiscard]] auto fromCLimits64(isoline_limits64_t const* limits, binary::AddressType addressType) noexcept
    -> std::expected<binary::Limits, TypeRefusal>;

/// Writes @p limits to @p out, as isoline_limits64_t says an output is written: its known fields, and every
/// byte past them up to its struct_size zeroed.
/// @param limits The limits.
/// @param out The caller's struct; null, or a struct_size refused, is refused with nothing written.
/// @return Nothing; the refusal otherwise.
[[nodiscard]] auto toCLimits64(binary::Limits const& limits, isoline_limits64_t* out) noexcept
    -> std::expected<void, TypeRefusal>;

/// @p type, if it is a memory type a module may declare: a page size custom-page-sizes admits, and limits
/// within the pages its address type reaches, the minimum not above the maximum.
/// @return The type; its refusal otherwise.
[[nodiscard]] auto checkMemoryType(binary::MemoryType const& type) noexcept
    -> std::expected<binary::MemoryType, TypeRefusal>;

/// @p limits, if they are a table's a module may declare: within the elements their address type reaches, the
/// minimum not above the maximum.
/// @return The limits; their refusal otherwise.
[[nodiscard]] auto checkTableLimits(binary::Limits const& limits) noexcept
    -> std::expected<binary::Limits, TypeRefusal>;

} // namespace isoline::c::detail
