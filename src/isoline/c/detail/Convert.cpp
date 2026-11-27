// SPDX-License-Identifier: Apache-2.0
#include <isoline/c/detail/Convert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include <isoline/api/Store.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/c/detail/EnumRows.hpp>
#include <isoline/c/detail/Error.hpp>
#include <isoline/c/detail/ErrorRows.hpp>
#include <isoline/c/detail/Handles.hpp>
#include <isoline/c/detail/Vector.hpp>
#include <isoline/c/isoline.h>

namespace isoline::c::detail
{

namespace
{
    /// The most bytes a caller's isoline_limits64_t may say it holds.
    constexpr auto MaxStructSize = std::size_t { 4096 };

    /// The size isoline_limits64_t was first published with (ABI 0.1), frozen: the least struct_size
    /// accepted, whatever the struct grows to. A field a later minor appends lies past it, and is read or
    /// written only for a caller whose struct_size holds it.
    constexpr auto Limits64FirstSize = std::size_t { 32 };
    static_assert(offsetof(isoline_limits64_t, reserved) + sizeof(isoline_limits64_t::reserved)
                      == Limits64FirstSize,
                  "every field of the first published isoline_limits64_t lies within its first size");
    static_assert(sizeof(isoline_limits64_t) >= Limits64FirstSize, "the struct only ever grows");

    /// The row of the heap of the reference type @p type.
    [[nodiscard]] auto heapRowOf(binary::ValueType const& type) noexcept -> HeapRow const*
    {
        if (type.kind != binary::ValueKind::Ref)
            return nullptr;
        return findRow(HeapRows, &HeapRow::heap, type.heap.kind);
    }

    /// One refusal of a type the host describes: the api::InvalidTypeReason whose detail it carries, if one
    /// names it, and its message.
    struct TypeRefusalRow
    {
        TypeRefusal refusal;                          ///< The refusal.
        std::optional<api::InvalidTypeReason> reason; ///< The reason whose detail it is reported with.
        std::string_view message;                     ///< Its message.
    };

    /// The refusals, one row each, in enumerator order.
    constexpr auto TypeRefusalRows = std::to_array<TypeRefusalRow>({
        { TypeRefusal::NullArgument, std::nullopt, "an argument the function needs is NULL" },
        { TypeRefusal::StructSize,
          std::nullopt,
          "the isoline_limits64_t's struct_size is below 32 (its first published size) or above 4096" },
        { TypeRefusal::NonzeroReserved,
          std::nullopt,
          "the isoline_limits64_t's reserved bytes are not zero" },
        { TypeRefusal::UnknownField,
          std::nullopt,
          "the isoline_limits64_t sets a field past those this library knows" },
        { TypeRefusal::UnknownMaximumKind,
          std::nullopt,
          "the isoline_limits64_t's maximum_kind is no ISOLINE_MAXIMUM_ constant" },
        { TypeRefusal::MaximumWithoutKind,
          std::nullopt,
          "the isoline_limits64_t's maximum is not zero, and its maximum_kind ISOLINE_MAXIMUM_NONE" },
        { TypeRefusal::AddressBits, std::nullopt, "the address width is neither 32 nor 64" },
        { TypeRefusal::PageSize,
          api::InvalidTypeReason::PageSize,
          "the memory's page size is not one admitted" },
        { TypeRefusal::MinimumAboveMaximum, std::nullopt, "the minimum size is above the maximum" },
        { TypeRefusal::SizePastAddressType,
          std::nullopt,
          "a size is past the most the address type reaches" },
        { TypeRefusal::NonReferenceElement,
          api::InvalidTypeReason::NonReferenceElement,
          "the table's element type is not a reference type" },
    });
    static_assert(TypeRefusalRows.back().refusal == TypeRefusal::NonReferenceElement,
                  "one row per TypeRefusal, the last enumerator last");
    static_assert(
        [] {
            for (auto const index: std::views::iota(std::size_t { 0 }, TypeRefusalRows.size()))
                if (static_cast<std::size_t>(TypeRefusalRows[index].refusal) != index)
                    return false;
            return true;
        }(),
        "the rows are in enumerator order, one per TypeRefusal");

    /// @p limits, if no size of them is past @p reach and the minimum is not above the maximum.
    [[nodiscard]] auto checkSizes(binary::Limits const& limits, uint64_t reach) noexcept
        -> std::expected<binary::Limits, TypeRefusal>
    {
        if (limits.max.has_value() && limits.min > *limits.max)
            return std::unexpected { TypeRefusal::MinimumAboveMaximum };
        if (limits.min > reach || limits.max.value_or(0) > reach)
            return std::unexpected { TypeRefusal::SizePastAddressType };
        return limits;
    }

    /// The vector of the C objects of @p types.
    /// @return The vector; std::nullopt when one of @p types has no C form. Throws std::bad_alloc when the
    ///         host has no memory for it, having made nothing.
    [[nodiscard]] auto valueTypesOf(std::span<binary::ValueType const> types)
        -> std::optional<ValueTypeVector>
    {
        // Zeroed, so that what is deleted if an element fails is what was made so far.
        auto array =
            ElementArray<ElementOwnership::Owned, wasm_valtype_t*> { types.size(), FreshElements::Zeroed };
        if (array.empty() && !types.empty())
            throw std::bad_alloc {};
        auto const slots = array.elements();
        for (auto const index: std::views::iota(std::size_t { 0 }, types.size()))
        {
            auto element = toCType(types[index]);
            if (element == nullptr)
                return std::nullopt;
            slots[index] = element.release();
        }
        auto vector = wasm_valtype_vec_t {};
        array.releaseInto(vector);
        return ValueTypeVector { vector };
    }

    /// Whether @p structSize is a struct_size of an isoline_limits64_t the library accepts: its first
    /// published size, or any up to 4 KiB.
    [[nodiscard]] auto isAcceptedStructSize(std::size_t structSize) noexcept -> bool
    {
        return structSize >= Limits64FirstSize && structSize <= MaxStructSize;
    }

    /// The bytes of a caller's struct of @p structSize bytes at @p bytes past the fields this library knows:
    /// a later minor's fields, empty unless the caller's struct is larger than this header's.
    /// @param bytes The caller's struct, whose struct_size was accepted.
    /// @param structSize Its struct_size.
    template <typename Byte>
    [[nodiscard]] auto unknownTail(Byte* bytes, std::size_t structSize) noexcept -> std::span<Byte>
    {
        auto const whole = std::span { bytes, structSize };
        return whole.subspan(std::min(structSize, sizeof(isoline_limits64_t)));
    }
} // namespace

auto toCKind(binary::ValueType const& type) noexcept -> wasm_valkind_t
{
    auto canonical = type;
    if (type.kind == binary::ValueKind::Ref)
    {
        auto const* const heap = heapRowOf(type);
        if (heap == nullptr)
            return ISOLINE_VALKIND_INVALID;
        // The kind of a reference type is its hierarchy's: the row of the nullable reference to the top.
        canonical = binary::ValueType { .kind = binary::ValueKind::Ref,
                                        .nullable = binary::Nullable::Yes,
                                        .heap = { .kind = heap->top } };
    }
    auto const* const row = findRow(ValueKindRows, &ValueKindRow::type, canonical);
    return row == nullptr ? wasm_valkind_t { ISOLINE_VALKIND_INVALID } : row->kind;
}

auto fromCKind(wasm_valkind_t kind) noexcept -> std::optional<binary::ValueType>
{
    auto const* const row = findRow(ValueKindRows, &ValueKindRow::kind, kind);
    if (row == nullptr)
        return std::nullopt;
    return row->type;
}

auto toCHeap(binary::ValueType const& type) noexcept -> isoline_heap_t
{
    auto const* const row = heapRowOf(type);
    return row == nullptr ? isoline_heap_t { ISOLINE_HEAP_NOT_REFERENCE } : row->cHeap;
}

auto toCNullability(binary::ValueType const& type) noexcept -> isoline_nullability_t
{
    return type.kind == binary::ValueKind::Ref && type.nullable == binary::Nullable::Yes
               ? isoline_nullability_t { ISOLINE_NULLABLE }
               : isoline_nullability_t { ISOLINE_NOT_NULLABLE };
}

auto toCExternKind(binary::ExternKind kind) noexcept -> wasm_externkind_t
{
    // ExternKindRows has a row for every enumerator, at its index (its static_asserts).
    return rowAt(ExternKindRows, kind).cKind;
}

auto fromCExternKind(wasm_externkind_t kind) noexcept -> std::optional<binary::ExternKind>
{
    auto const* const row = findRow(ExternKindRows, &ExternKindRow::cKind, kind);
    if (row == nullptr)
        return std::nullopt;
    return row->kind;
}

auto toCMutability(binary::Mutability mutability) noexcept -> wasm_mutability_t
{
    return rowAt(MutabilityRows, mutability).cMutability;
}

auto fromCMutability(wasm_mutability_t mutability) noexcept -> std::optional<binary::Mutability>
{
    auto const* const row = findRow(MutabilityRows, &MutabilityRow::cMutability, mutability);
    if (row == nullptr)
        return std::nullopt;
    return row->mutability;
}

auto toCAddressBits(binary::AddressType addressType) noexcept -> uint32_t
{
    return rowAt(AddressTypeRows, addressType).bits;
}

auto fromCAddressBits(uint32_t bits) noexcept -> std::optional<binary::AddressType>
{
    auto const* const row = findRow(AddressTypeRows, &AddressTypeRow::bits, bits);
    if (row == nullptr)
        return std::nullopt;
    return row->addressType;
}

auto toCType(binary::ValueType const& type) -> std::unique_ptr<wasm_valtype_t>
{
    if (type.kind == binary::ValueKind::Ref && heapRowOf(type) == nullptr)
        return nullptr;
    return std::make_unique<wasm_valtype_t>(type);
}

auto fromCType(wasm_valtype_t const& type) noexcept -> binary::ValueType
{
    return type.type();
}

auto toCType(binary::FuncType const& type) -> std::unique_ptr<wasm_functype_t>
{
    auto params = valueTypesOf(type.params);
    auto results = valueTypesOf(type.results);
    if (!params.has_value() || !results.has_value())
        return nullptr;
    return std::make_unique<wasm_functype_t>(std::move(*params), std::move(*results));
}

auto fromCType(wasm_functype_t const& type) -> binary::FuncType
{
    auto result = binary::FuncType {};
    result.params.reserve(type.params().size);
    for (auto const* const param: std::span { type.params().data, type.params().size })
        result.params.push_back(param->type());
    result.results.reserve(type.results().size);
    for (auto const* const resultType: std::span { type.results().data, type.results().size })
        result.results.push_back(resultType->type());
    return result;
}

auto typeRefusalError(TypeRefusal refusal) noexcept -> isoline_error_t*
{
    // Every enumerator has its row (the static_asserts above); a value of none is refused as an argument all
    // the same, never read past the table.
    auto const* const row = findRow(TypeRefusalRows, &TypeRefusalRow::refusal, refusal);
    if (row == nullptr)
        return newError(ISOLINE_ERROR_INVALID_ARGUMENT, 0, "the type is refused");
    auto const detail =
        row->reason.has_value() ? detailOf(ISOLINE_ERROR_INVALID_ARGUMENT, *row->reason) : uint32_t { 0 };
    return newError(ISOLINE_ERROR_INVALID_ARGUMENT, detail, row->message);
}

auto fromCLimits(wasm_limits_t const& limits) noexcept -> binary::Limits
{
    auto result =
        binary::Limits { .min = limits.min, .max = std::nullopt, .addressType = binary::AddressType::I32 };
    if (limits.max != wasm_limits_max_default)
        result.max = limits.max;
    return result;
}

auto fromCLimits64(isoline_limits64_t const* limits, binary::AddressType addressType) noexcept
    -> std::expected<binary::Limits, TypeRefusal>
{
    if (limits == nullptr)
        return std::unexpected { TypeRefusal::NullArgument };
    if (!isAcceptedStructSize(limits->struct_size))
        return std::unexpected { TypeRefusal::StructSize };
    if (std::ranges::any_of(limits->reserved, [](uint8_t byte) { return byte != 0; }))
        return std::unexpected { TypeRefusal::NonzeroReserved };
    // Every field read below lies within the first published size, which an accepted struct_size holds. A
    // later minor's fields, past this header's, must be zero: the caller's struct holds struct_size bytes.
    auto const unknown = unknownTail(reinterpret_cast<unsigned char const*>(limits), limits->struct_size);
    if (std::ranges::any_of(unknown, [](unsigned char byte) { return byte != 0; }))
        return std::unexpected { TypeRefusal::UnknownField };
    auto result = binary::Limits { .min = limits->minimum, .max = std::nullopt, .addressType = addressType };
    if (limits->maximum_kind == ISOLINE_MAXIMUM_GIVEN)
        result.max = limits->maximum;
    else if (limits->maximum_kind != ISOLINE_MAXIMUM_NONE)
        return std::unexpected { TypeRefusal::UnknownMaximumKind };
    else if (limits->maximum != 0)
        return std::unexpected { TypeRefusal::MaximumWithoutKind };
    return result;
}

auto toCLimits64(binary::Limits const& limits, isoline_limits64_t* out) noexcept
    -> std::expected<void, TypeRefusal>
{
    if (out == nullptr)
        return std::unexpected { TypeRefusal::NullArgument };
    if (!isAcceptedStructSize(out->struct_size))
        return std::unexpected { TypeRefusal::StructSize };
    // Every field written below lies within the first published size, which an accepted struct_size
    // holds; past the struct, nothing is written. A later minor's fields this library does not know are
    // zeroed.
    std::ranges::fill(unknownTail(reinterpret_cast<unsigned char*>(out), out->struct_size),
                      static_cast<unsigned char>(0));
    out->minimum = limits.min;
    out->maximum = limits.max.value_or(0);
    out->maximum_kind = limits.max.has_value() ? isoline_maximum_kind_t { ISOLINE_MAXIMUM_GIVEN }
                                               : isoline_maximum_kind_t { ISOLINE_MAXIMUM_NONE };
    std::ranges::fill(out->reserved, uint8_t { 0 });
    return {};
}

auto checkMemoryType(binary::MemoryType const& type) noexcept
    -> std::expected<binary::MemoryType, TypeRefusal>
{
    if (std::ranges::find(binary::ValidPageSizesLog2, type.pageSizeLog2) == binary::ValidPageSizesLog2.end())
        return std::unexpected { TypeRefusal::PageSize };
    return checkSizes(type.limits, binary::maxPages(type)).transform([&](binary::Limits const&) {
        return type;
    });
}

auto checkTableLimits(binary::Limits const& limits) noexcept -> std::expected<binary::Limits, TypeRefusal>
{
    return checkSizes(limits, binary::maxTableSize(binary::TableType { .limits = limits }));
}

} // namespace isoline::c::detail
