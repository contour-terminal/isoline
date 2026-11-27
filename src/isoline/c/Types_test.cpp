// SPDX-License-Identifier: Apache-2.0
//
// The rows that map the C API's kinds onto the C++ ones (detail/EnumRows.hpp), and the conversions that read
// them (detail/Convert.hpp): every row round-trips, every C value without a row is refused, every value type
// a module may declare has a C form and comes back unchanged, and the bottom heap, which none may, has none.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/c/detail/Convert.hpp>
#include <isoline/c/detail/EnumRows.hpp>
#include <isoline/c/detail/Handles.hpp>
#include <isoline/c/detail/Vector.hpp>
#include <isoline/c/isoline.h>

namespace binary = isoline::binary;
namespace detail = isoline::c::detail;

namespace
{

/// Every value of an 8-bit C enumeration.
[[nodiscard]] auto everyByte()
{
    return std::views::iota(0U, 256U)
           | std::views::transform([](unsigned value) { return static_cast<uint8_t>(value); });
}

/// Every value type a module may declare, of each heap with a row, each nullability and a few concrete
/// indices.
[[nodiscard]] auto declarableValueTypes() -> std::vector<binary::ValueType>
{
    auto types =
        std::vector<binary::ValueType> { binary::I32, binary::I64, binary::F32, binary::F64, binary::V128 };
    for (auto const& row: detail::HeapRows)
        for (auto const nullable: { binary::Nullable::No, binary::Nullable::Yes })
            for (auto const index: { uint32_t { 0 }, uint32_t { 5 }, UINT32_MAX - 1 })
            {
                auto const isConcrete = row.heap == binary::AbstractHeapType::Concrete;
                if (!isConcrete && index != 0)
                    continue;
                types.push_back(binary::ValueType { .kind = binary::ValueKind::Ref,
                                                    .nullable = nullable,
                                                    .heap = { .kind = row.heap, .typeIndex = index } });
            }
    return types;
}

} // namespace

TEST_CASE("Types.isolineKindsOutsideUpstreamSpace", "[c-api][types]")
{
    // Upstream numbers 0-3 and 128-129. Isoline's numbers are none of upstream's, and every row lies on the
    // side of 128 that wasm.h's wasm_valkind_is_num() and wasm_valkind_is_ref() read right: v128 at its
    // binary-format byte, below 128, and Isoline's references in its own range.
    auto const upstream = std::to_array<wasm_valkind_t>(
        { WASM_I32, WASM_I64, WASM_F32, WASM_F64, WASM_EXTERNREF, WASM_FUNCREF });
    for (auto const& row: detail::ValueKindRows)
    {
        INFO("kind " << int { row.kind });
        auto const isUpstream = std::ranges::find(upstream, row.kind) != upstream.end();
        CHECK(isUpstream == (row.origin == detail::KindOrigin::Upstream));
        CHECK(wasm_valkind_is_num(row.kind) == (row.type.kind != binary::ValueKind::Ref));
        CHECK(wasm_valkind_is_ref(row.kind) == (row.type.kind == binary::ValueKind::Ref));
        if (row.origin == detail::KindOrigin::Isoline && row.type.kind == binary::ValueKind::Ref)
            CHECK((row.kind >= detail::IsolineKindFirst && row.kind <= detail::IsolineKindLast));
    }
    CHECK(ISOLINE_VALKIND_V128 == 0x7B);
    CHECK(ISOLINE_VALKIND_INVALID >= detail::IsolineKindFirst);
    CHECK(detail::findRow(
              detail::ValueKindRows, &detail::ValueKindRow::kind, wasm_valkind_t { ISOLINE_VALKIND_INVALID })
          == nullptr);
}

TEST_CASE("Types.everyRefusalIsAnInvalidArgument", "[c-api][types]")
{
    // Each refusal is INVALID_ARGUMENT; the two an api::InvalidTypeReason names carry its detail. A value no
    // enumerator has is refused all the same, never read past the rows.
    for (auto const value: std::views::iota(0U, 256U))
    {
        auto* const error = detail::typeRefusalError(static_cast<detail::TypeRefusal>(value));
        CHECK(isoline_error_code(error) == ISOLINE_ERROR_INVALID_ARGUMENT);
        if (value == static_cast<unsigned>(detail::TypeRefusal::PageSize))
            CHECK(isoline_error_detail(error) == 2);
        else if (value == static_cast<unsigned>(detail::TypeRefusal::NonReferenceElement))
            CHECK(isoline_error_detail(error) == 4);
        else
            CHECK(isoline_error_detail(error) == 0);
        isoline_error_delete(error);
    }
}

TEST_CASE("Types.everyValueKindRoundTrips", "[c-api][types]")
{
    // Every row's kind makes its type, and the type reports the kind; every other byte makes nothing.
    for (auto const value: everyByte())
    {
        INFO("kind " << int { value });
        auto const* const row = detail::findRow(detail::ValueKindRows, &detail::ValueKindRow::kind, value);
        auto const type = detail::fromCKind(value);
        REQUIRE(type.has_value() == (row != nullptr));
        if (row != nullptr)
        {
            CHECK(*type == row->type);
            CHECK(detail::toCKind(*type) == value);
        }
    }
    // Each value kind of binary::ValueKind but the references has exactly one row; the references one per
    // top.
    for (auto const kind: { binary::ValueKind::I32,
                            binary::ValueKind::I64,
                            binary::ValueKind::F32,
                            binary::ValueKind::F64,
                            binary::ValueKind::V128 })
        CHECK(std::ranges::count(detail::ValueKindRows, kind, [](auto const& row) { return row.type.kind; })
              == 1);
}

TEST_CASE("Types.referenceValueKindsAreTheReferenceRows", "[c-api][types]")
{
    // A wasm_val_t owns its of.ref exactly for the kinds of reference rows: exnref with externref and
    // funcref, and never v128.
    for (auto const& row: detail::ValueKindRows)
    {
        INFO("kind " << int { row.kind });
        CHECK(detail::holdsReference(row.kind) == (row.type.kind == binary::ValueKind::Ref));
    }
    CHECK(detail::holdsReference(ISOLINE_VALKIND_EXNREF));
    CHECK_FALSE(detail::holdsReference(ISOLINE_VALKIND_V128));
    CHECK_FALSE(detail::holdsReference(ISOLINE_VALKIND_INVALID));
    CHECK(detail::ReferenceValueKinds.size() == 3);
}

TEST_CASE("Types.everyDeclarableValueTypeRoundTrips", "[c-api][types]")
{
    for (auto const& type: declarableValueTypes())
    {
        INFO("kind " << int(type.kind) << " heap " << int(type.heap.kind) << " index "
                     << type.heap.typeIndex);
        auto const object = detail::toCType(type);
        REQUIRE(object != nullptr);
        CHECK(detail::fromCType(*object) == type);
        // A reference's kind is its hierarchy's, its heap and nullability its own.
        auto const kind = detail::toCKind(type);
        CHECK(kind != ISOLINE_VALKIND_INVALID);
        if (type.kind != binary::ValueKind::Ref)
        {
            CHECK(detail::toCHeap(type) == ISOLINE_HEAP_NOT_REFERENCE);
            CHECK(detail::toCNullability(type) == ISOLINE_NOT_NULLABLE);
            continue;
        }
        auto const* const heap = detail::findRow(detail::HeapRows, &detail::HeapRow::heap, type.heap.kind);
        REQUIRE(heap != nullptr);
        CHECK(detail::toCHeap(type) == heap->cHeap);
        CHECK(detail::fromCKind(kind)->heap.kind == heap->top);
        CHECK((detail::toCNullability(type) == ISOLINE_NULLABLE) == (type.nullable == binary::Nullable::Yes));
        CHECK(isoline_valtype_concrete_index(object.get())
              == (type.heap.kind == binary::AbstractHeapType::Concrete ? type.heap.typeIndex : UINT32_MAX));
    }
}

TEST_CASE("Types.referenceKindIsItsHierarchys", "[c-api][types]")
{
    // Spelled out, not read from the rows: each heap's kind is its hierarchy's top, and its C heap its own.
    struct Expected
    {
        binary::AbstractHeapType heap;
        wasm_valkind_t kind;
        isoline_heap_t cHeap;
    };
    for (auto const& expected:
         { Expected { binary::AbstractHeapType::Func, WASM_FUNCREF, ISOLINE_HEAP_FUNC },
           Expected { binary::AbstractHeapType::NoFunc, WASM_FUNCREF, ISOLINE_HEAP_NOFUNC },
           Expected { binary::AbstractHeapType::Concrete, WASM_FUNCREF, ISOLINE_HEAP_CONCRETE },
           Expected { binary::AbstractHeapType::Extern, WASM_EXTERNREF, ISOLINE_HEAP_EXTERN },
           Expected { binary::AbstractHeapType::NoExtern, WASM_EXTERNREF, ISOLINE_HEAP_NOEXTERN },
           Expected { binary::AbstractHeapType::Exn, ISOLINE_VALKIND_EXNREF, ISOLINE_HEAP_EXN },
           Expected { binary::AbstractHeapType::NoExn, ISOLINE_VALKIND_EXNREF, ISOLINE_HEAP_NOEXN } })
    {
        INFO("heap " << int(expected.heap));
        auto const type = binary::ValueType { .kind = binary::ValueKind::Ref,
                                              .nullable = binary::Nullable::No,
                                              .heap = { .kind = expected.heap } };
        CHECK(detail::toCKind(type) == expected.kind);
        CHECK(detail::toCHeap(type) == expected.cHeap);
    }
    CHECK(detail::HeapRows.size() == 7);
}

TEST_CASE("Types.bottomHeapHasNoCForm", "[c-api][types]")
{
    auto const bottom = binary::ValueType { .kind = binary::ValueKind::Ref,
                                            .nullable = binary::Nullable::No,
                                            .heap = { .kind = binary::AbstractHeapType::Bot } };
    CHECK(detail::toCType(bottom) == nullptr);
    CHECK(detail::toCKind(bottom) == ISOLINE_VALKIND_INVALID);
    CHECK(detail::toCHeap(bottom) == ISOLINE_HEAP_NOT_REFERENCE);
    // A function type naming it has none either.
    CHECK(detail::toCType(binary::FuncType { .params = { binary::I32, bottom }, .results = {} }) == nullptr);
    CHECK(detail::toCType(binary::FuncType { .params = {}, .results = { bottom } }) == nullptr);
}

TEST_CASE("Types.functionTypeRoundTrips", "[c-api][types]")
{
    auto const declarable = declarableValueTypes();
    auto const type = binary::FuncType { .params = declarable, .results = { binary::V128, binary::ExnRef } };
    auto const object = detail::toCType(type);
    REQUIRE(object != nullptr);
    CHECK(detail::fromCType(*object) == type);
    CHECK(object->kind() == binary::ExternKind::Func);
    auto const empty = detail::toCType(binary::FuncType {});
    REQUIRE(empty != nullptr);
    CHECK(empty->params().size == 0);
    CHECK(detail::fromCType(*empty) == binary::FuncType {});
}

TEST_CASE("Types.externKindsAndMutabilitiesRoundTrip", "[c-api][types]")
{
    for (auto const& row: detail::ExternKindRows)
    {
        CHECK(detail::toCExternKind(row.kind) == row.cKind);
        CHECK(detail::fromCExternKind(row.cKind) == row.kind);
    }
    for (auto const& row: detail::MutabilityRows)
    {
        CHECK(detail::toCMutability(row.mutability) == row.cMutability);
        CHECK(detail::fromCMutability(row.cMutability) == row.mutability);
    }
    for (auto const& row: detail::AddressTypeRows)
    {
        CHECK(detail::toCAddressBits(row.addressType) == row.bits);
        CHECK(detail::fromCAddressBits(row.bits) == row.addressType);
    }
    // No other value names one: an unknown one is refused, never cast.
    for (auto const value: everyByte())
    {
        INFO("value " << int { value });
        CHECK(detail::fromCExternKind(value).has_value()
              == (detail::findRow(detail::ExternKindRows, &detail::ExternKindRow::cKind, value) != nullptr));
        CHECK(detail::fromCMutability(value).has_value()
              == (detail::findRow(detail::MutabilityRows, &detail::MutabilityRow::cMutability, value)
                  != nullptr));
    }
    CHECK_FALSE(detail::fromCAddressBits(0).has_value());
    CHECK_FALSE(detail::fromCAddressBits(UINT32_MAX).has_value());
}

TEST_CASE("Types.limitsClampOneWay", "[c-api][types]")
{
    // To wasm_limits_t a size past UINT32_MAX clamps, and no maximum is wasm_limits_max_default; from it,
    // wasm_limits_max_default as a maximum is none, and every other size is itself.
    auto const big = detail::toCLimits(binary::Limits {
        .min = uint64_t { 1 } << 33, .max = uint64_t { 1 } << 34, .addressType = binary::AddressType::I64 });
    CHECK(big.min == wasm_limits_max_default);
    CHECK(big.max == wasm_limits_max_default);
    auto const unbounded = detail::toCLimits(binary::Limits { .min = 3, .max = std::nullopt });
    CHECK(unbounded.min == 3);
    CHECK(unbounded.max == wasm_limits_max_default);
    auto const exact = detail::fromCLimits(wasm_limits_t { .min = 0, .max = 0xFFFF'FFFE });
    CHECK(exact.max == uint64_t { 0xFFFF'FFFE });
    CHECK_FALSE(
        detail::fromCLimits(wasm_limits_t { .min = 0, .max = wasm_limits_max_default }).max.has_value());
}
