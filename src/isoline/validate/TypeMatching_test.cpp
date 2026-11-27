// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/validate/TypeMatching.hpp>
#include <isoline/validate/ValidatedModule.hpp>

using isoline::FeatureSet;
using isoline::binary::AddressType;
using isoline::binary::concreteRef;
using isoline::binary::ExternKind;
using isoline::binary::ExternRef;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::I32;
using isoline::binary::I64;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::binary::Mutability;
using isoline::binary::NonNullFuncRef;
using isoline::binary::Nullable;
using isoline::binary::TableType;
using isoline::binary::ValueKind;
using isoline::binary::ValueType;
using isoline::binary::testing::ModuleBuilder;
using isoline::validate::CanonicalTypes;
using isoline::validate::CoreExternType;
using isoline::validate::matches;
using isoline::validate::matchesExtern;
using isoline::validate::TypeMatcher;
using isoline::validate::ValidatedModule;

namespace
{

/// What a row expects of matching.
enum class Expected : uint8_t
{
    Mismatch, ///< The import is not satisfied: validation refuses it, linking fails.
    Match,    ///< The import is satisfied.
};

/// One matching case: what one module provides, what another requires, and the verdict.
struct MatchRow
{
    std::string_view what;            ///< What the row checks.
    std::vector<FuncType> subTypes;   ///< The providing module's type section.
    CoreExternType sub;               ///< What it provides, in its indices.
    std::vector<FuncType> superTypes; ///< The requiring module's type section.
    CoreExternType super;             ///< What it requires, in its indices.
    Expected expected;                ///< The verdict.
};

/// (ref $index), non-null.
ValueType ref(uint32_t index)
{
    return concreteRef(index, Nullable::No);
}

/// (ref null $index).
ValueType refNull(uint32_t index)
{
    return concreteRef(index, Nullable::Yes);
}

/// A function type of @p params and @p results.
FuncType func(std::vector<ValueType> params, std::vector<ValueType> results = {})
{
    return FuncType { .params = std::move(params), .results = std::move(results) };
}

CoreExternType funcOf(uint32_t typeIndex)
{
    return CoreExternType { .kind = ExternKind::Func, .typeIndex = typeIndex };
}

CoreExternType tagOf(uint32_t typeIndex)
{
    return CoreExternType { .kind = ExternKind::Tag, .typeIndex = typeIndex };
}

CoreExternType tableOf(ValueType element, Limits limits)
{
    return CoreExternType { .kind = ExternKind::Table,
                            .table = TableType { .element = element, .limits = limits } };
}

CoreExternType memoryOf(Limits limits, uint8_t pageSizeLog2 = isoline::binary::DefaultPageSizeLog2)
{
    return CoreExternType { .kind = ExternKind::Memory,
                            .memory = MemoryType { .limits = limits, .pageSizeLog2 = pageSizeLog2 } };
}

CoreExternType globalOf(ValueType type, Mutability mutability)
{
    return CoreExternType { .kind = ExternKind::Global,
                            .global = GlobalType { .type = type, .mutability = mutability } };
}

/// Limits from @p min, with @p max if given, of 32-bit addresses.
Limits limits(uint64_t min,
              std::optional<uint64_t> max = std::nullopt,
              AddressType address = AddressType::I32)
{
    return Limits { .min = min, .max = max, .addressType = address };
}

/// Types equal across modules by canonical structure, whatever their index in each (the cross-module
/// form of the rec groups of the GC proposal, which Isoline does not implement: without it every type
/// is its own group).
std::vector<MatchRow> canonicalRows()
{
    return {
        { .what = "equal types at other indices",
          .subTypes = { func({ I32 }), func({ refNull(0) }) },
          .sub = funcOf(1),
          .superTypes = { func({}), func({ I32 }), func({ refNull(1) }) },
          .super = funcOf(2),
          .expected = Expected::Match },
        { .what = "a module's duplicate types are one",
          .subTypes = { func({}), func({}), func({ refNull(1) }) },
          .sub = funcOf(2),
          .superTypes = { func({}), func({ refNull(0) }) },
          .super = funcOf(1),
          .expected = Expected::Match },
        { .what = "the same shape, naming another type",
          .subTypes = { func({ I32 }), func({ refNull(0) }) },
          .sub = funcOf(1),
          .superTypes = { func({ I64 }), func({ refNull(0) }) },
          .super = funcOf(1),
          .expected = Expected::Mismatch },
        { .what = "the same index, another type",
          .subTypes = { func({ I32 }) },
          .sub = funcOf(0),
          .superTypes = { func({}, { I32 }) },
          .super = funcOf(0),
          .expected = Expected::Mismatch },
        { .what = "a referenced type's nullability",
          .subTypes = { func({}), func({ ref(0) }) },
          .sub = funcOf(1),
          .superTypes = { func({}), func({ refNull(0) }) },
          .super = funcOf(1),
          .expected = Expected::Mismatch },
        { .what = "a result count",
          .subTypes = { func({ I32 }, { I32, I32 }) },
          .sub = funcOf(0),
          .superTypes = { func({ I32 }, { I32 }) },
          .super = funcOf(0),
          .expected = Expected::Mismatch },
    };
}

/// core-type-out-of-group-deps.wast's shapes without GC: a function type naming types defined before
/// it, through a chain of them, as Wasmtime accepts them; and the same chain differing at its end.
std::vector<MatchRow> outOfGroupRows()
{
    return {
        { .what = "a function type naming an earlier one",
          .subTypes = { func({}), func({ refNull(0) }) },
          .sub = funcOf(1),
          .superTypes = { func({ I64 }), func({}), func({ refNull(1) }) },
          .super = funcOf(2),
          .expected = Expected::Match },
        { .what = "a chain of dependencies",
          .subTypes = { func({}), func({ refNull(0) }), func({ refNull(1), refNull(0) }) },
          .sub = funcOf(2),
          .superTypes = { func({ I32 }), func({}), func({ refNull(1) }), func({ refNull(2), refNull(1) }) },
          .super = funcOf(3),
          .expected = Expected::Match },
        { .what = "a chain differing at its end",
          .subTypes = { func({}), func({ refNull(0) }), func({ refNull(1), refNull(0) }) },
          .sub = funcOf(2),
          .superTypes = { func({ I32 }), func({ refNull(0) }), func({ refNull(1), refNull(0) }) },
          .super = funcOf(2),
          .expected = Expected::Mismatch },
    };
}

/// core-import-func-subtype.wast's shapes without GC: no function type declares a supertype, so a
/// function or tag matches only an equivalent type, typed references included; a global's value
/// type, when immutable, matches by subtyping.
std::vector<MatchRow> typedReferenceRows()
{
    return {
        { .what = "a function of an equivalent type with typed references",
          .subTypes = { func({}), func({ refNull(0) }, { ref(0) }) },
          .sub = funcOf(1),
          .superTypes = { func({ I32 }), func({}), func({ refNull(1) }, { ref(1) }) },
          .super = funcOf(2),
          .expected = Expected::Match },
        { .what = "(result (ref $t)) for (result (ref null $t)): no structural function subtyping",
          .subTypes = { func({}), func({}, { ref(0) }) },
          .sub = funcOf(1),
          .superTypes = { func({}), func({}, { refNull(0) }) },
          .super = funcOf(1),
          .expected = Expected::Mismatch },
        { .what = "(result (ref func)) for (result funcref)",
          .subTypes = { func({}, { NonNullFuncRef }) },
          .sub = funcOf(0),
          .superTypes = { func({}, { FuncRef }) },
          .super = funcOf(0),
          .expected = Expected::Mismatch },
        { .what = "a tag of an equivalent type",
          .subTypes = { func({}), func({ refNull(0) }) },
          .sub = tagOf(1),
          .superTypes = { func({ I32 }), func({}), func({ refNull(1) }) },
          .super = tagOf(2),
          .expected = Expected::Match },
        { .what = "a tag of another type",
          .subTypes = { func({ I32 }) },
          .sub = tagOf(0),
          .superTypes = { func({ I64 }) },
          .super = tagOf(0),
          .expected = Expected::Mismatch },
        { .what = "an immutable (ref $t) global for (ref null $t)",
          .subTypes = { func({}) },
          .sub = globalOf(ref(0), Mutability::Const),
          .superTypes = { func({ I32 }), func({}) },
          .super = globalOf(refNull(1), Mutability::Const),
          .expected = Expected::Match },
        { .what = "an immutable (ref null $t) global for funcref",
          .subTypes = { func({}) },
          .sub = globalOf(refNull(0), Mutability::Const),
          .superTypes = {},
          .super = globalOf(FuncRef, Mutability::Const),
          .expected = Expected::Match },
        { .what = "an immutable (ref null $t) global for (ref null $u) of another type",
          .subTypes = { func({}) },
          .sub = globalOf(refNull(0), Mutability::Const),
          .superTypes = { func({ I32 }) },
          .super = globalOf(refNull(0), Mutability::Const),
          .expected = Expected::Mismatch },
        { .what = "a mutable (ref $t) global for (ref null $t): mutable globals are invariant",
          .subTypes = { func({}) },
          .sub = globalOf(ref(0), Mutability::Var),
          .superTypes = { func({}) },
          .super = globalOf(refNull(0), Mutability::Var),
          .expected = Expected::Mismatch },
        { .what = "a mutable global of an equivalent type",
          .subTypes = { func({}) },
          .sub = globalOf(refNull(0), Mutability::Var),
          .superTypes = { func({ I32 }), func({}) },
          .super = globalOf(refNull(1), Mutability::Var),
          .expected = Expected::Match },
        { .what = "a table of an equivalent element type",
          .subTypes = { func({}) },
          .sub = tableOf(refNull(0), limits(1)),
          .superTypes = { func({ I32 }), func({}) },
          .super = tableOf(refNull(1), limits(1)),
          .expected = Expected::Match },
        { .what = "a (ref null $t) table for funcref: table elements are invariant",
          .subTypes = { func({}) },
          .sub = tableOf(refNull(0), limits(1)),
          .superTypes = {},
          .super = tableOf(FuncRef, limits(1)),
          .expected = Expected::Mismatch },
    };
}

/// The core spec's extern type matching for limits, page sizes, element types, mutability and kinds.
std::vector<MatchRow> externLimitRows()
{
    auto const row = [](std::string_view what, CoreExternType sub, CoreExternType super, Expected expected) {
        return MatchRow {
            .what = what, .subTypes = {}, .sub = sub, .superTypes = {}, .super = super, .expected = expected
        };
    };
    return {
        row("a larger minimum", memoryOf(limits(2)), memoryOf(limits(1)), Expected::Match),
        row("a smaller minimum", memoryOf(limits(1)), memoryOf(limits(2)), Expected::Mismatch),
        row("no maximum for a required one", memoryOf(limits(1)), memoryOf(limits(1, 5)), Expected::Mismatch),
        row("a smaller maximum", memoryOf(limits(1, 4)), memoryOf(limits(1, 5)), Expected::Match),
        row("an equal maximum", memoryOf(limits(1, 5)), memoryOf(limits(1, 5)), Expected::Match),
        row("a larger maximum", memoryOf(limits(1, 6)), memoryOf(limits(1, 5)), Expected::Mismatch),
        row("a maximum where none is required", memoryOf(limits(1, 3)), memoryOf(limits(1)), Expected::Match),
        row("another address type",
            memoryOf(limits(1, std::nullopt, AddressType::I64)),
            memoryOf(limits(1)),
            Expected::Mismatch),
        row("the same 64-bit address type",
            memoryOf(limits(1, std::nullopt, AddressType::I64)),
            memoryOf(limits(1, std::nullopt, AddressType::I64)),
            Expected::Match),
        row("another page size", memoryOf(limits(1), 0), memoryOf(limits(1)), Expected::Mismatch),
        row("the same custom page size", memoryOf(limits(1), 0), memoryOf(limits(1), 0), Expected::Match),
        row("a table's larger minimum",
            tableOf(FuncRef, limits(3)),
            tableOf(FuncRef, limits(2)),
            Expected::Match),
        row("a table's smaller minimum",
            tableOf(FuncRef, limits(1)),
            tableOf(FuncRef, limits(2)),
            Expected::Mismatch),
        row("a table's larger maximum",
            tableOf(FuncRef, limits(1, 9)),
            tableOf(FuncRef, limits(1, 8)),
            Expected::Mismatch),
        row("a table's other element type",
            tableOf(ExternRef, limits(1)),
            tableOf(FuncRef, limits(1)),
            Expected::Mismatch),
        row("a table's other address type",
            tableOf(FuncRef, limits(1, std::nullopt, AddressType::I64)),
            tableOf(FuncRef, limits(1)),
            Expected::Mismatch),
        row("an immutable global for a mutable one",
            globalOf(I32, Mutability::Const),
            globalOf(I32, Mutability::Var),
            Expected::Mismatch),
        row("a mutable global for an immutable one",
            globalOf(I32, Mutability::Var),
            globalOf(I32, Mutability::Const),
            Expected::Mismatch),
        row("a mutable global of the same type",
            globalOf(I64, Mutability::Var),
            globalOf(I64, Mutability::Var),
            Expected::Match),
        row("a global of another value type",
            globalOf(I32, Mutability::Const),
            globalOf(I64, Mutability::Const),
            Expected::Mismatch),
        row("a memory for a table", memoryOf(limits(1)), tableOf(FuncRef, limits(1)), Expected::Mismatch),
    };
}

/// Every row above.
std::vector<MatchRow> allRows()
{
    auto rows = canonicalRows();
    for (auto&& more: { outOfGroupRows(), typedReferenceRows(), externLimitRows() })
        rows.insert(rows.end(), more.begin(), more.end());
    return rows;
}

/// Checks matchesExtern() against each row's verdict.
void checkRows(std::vector<MatchRow> const& rows)
{
    for (auto const& row: rows)
    {
        INFO(row.what);
        CHECK(matchesExtern(row.subTypes, row.sub, row.superTypes, row.super)
              == (row.expected == Expected::Match));
    }
}

/// A module's bytes with one type section entry @p entry, raw.
std::vector<std::byte> moduleWithTypeEntry(std::vector<uint8_t> entry)
{
    auto payload = std::vector<uint8_t> { 0x01 };
    payload.insert(payload.end(), entry.begin(), entry.end());
    auto builder = ModuleBuilder {};
    builder.rawSection(0x01, std::move(payload));
    return builder.build();
}

/// Whether @p bytes fail to decode as a malformed function type, as a GC type form does without GC.
bool refusedAsMalformedFunctionType(std::vector<std::byte> const& bytes)
{
    auto const loaded = ValidatedModule::load(bytes, FeatureSet::all(), isoline::binary::DecodeLimits {});
    REQUIRE_FALSE(loaded.has_value());
    auto const* decode = std::get_if<isoline::binary::DecodeError>(&loaded.error());
    REQUIRE(decode != nullptr);
    return decode->code == isoline::binary::DecodeErrorCode::MalformedFunctionType;
}

} // namespace

TEST_CASE("TypeMatching.canonicalAcrossModules")
{
    checkRows(canonicalRows());

    // The type-index form says the same.
    auto const sub = std::vector { func({ I32 }), func({ refNull(0) }) };
    auto const super = std::vector { func({}), func({ I32 }), func({ refNull(1) }) };
    CHECK(matches(sub, 1, super, 2));
    CHECK(matches(sub, 0, super, 1));
    CHECK_FALSE(matches(sub, 1, super, 1));
    CHECK_FALSE(matches(sub, 0, super, 0));

    // Without GC a recursion group does not decode: `rec` is no function type's form.
    CHECK(refusedAsMalformedFunctionType(moduleWithTypeEntry({ 0x4E, 0x01, 0x60, 0x00, 0x00 })));
}

TEST_CASE("TypeMatching.outOfGroupDependencies")
{
    checkRows(outOfGroupRows());

    // The file's struct types and declared subtypes need GC, refused at decode.
    CHECK(refusedAsMalformedFunctionType(moduleWithTypeEntry({ 0x5F, 0x00 }))); // (struct)
    CHECK(refusedAsMalformedFunctionType(
        moduleWithTypeEntry({ 0x50, 0x00, 0x60, 0x00, 0x00 }))); // (sub (func))
}

TEST_CASE("TypeMatching.emptyRecGroups")
{
    // An empty recursion group defines no types; without GC it does not decode at all.
    CHECK(refusedAsMalformedFunctionType(moduleWithTypeEntry({ 0x4E, 0x00 })));

    // A module that defines no types matches what names none.
    auto const none = std::span<FuncType const> {};
    auto const canonical = CanonicalTypes { none };
    CHECK(canonical.types().empty());
    CHECK(matchesExtern(none, memoryOf(limits(1)), none, memoryOf(limits(0))));
    CHECK(matchesExtern(
        none, globalOf(FuncRef, Mutability::Const), none, globalOf(FuncRef, Mutability::Const)));
}

TEST_CASE("TypeMatching.funcSubtypeWithTypedRefs")
{
    checkRows(typedReferenceRows());

    // The file's `(sub $super (func ...))` declares a supertype, which needs GC.
    CHECK(refusedAsMalformedFunctionType(moduleWithTypeEntry({ 0x4F, 0x00, 0x60, 0x00, 0x01, 0x7F })));
}

TEST_CASE("TypeMatching.externLimits")
{
    checkRows(externLimitRows());

    // A store's table or memory matches by its current size, passed as the minimum.
    CHECK(isoline::validate::limitsMatch(limits(3, 4), limits(2, 4)));
    CHECK_FALSE(isoline::validate::limitsMatch(limits(1, 4), limits(2, 4)));
}

TEST_CASE("TypeMatching.externTypeOfImport")
{
    auto const import =
        isoline::binary::Import { .moduleIndex = 0,
                                  .name = "g",
                                  .kind = ExternKind::Global,
                                  .typeIndex = 0,
                                  .table = {},
                                  .memory = {},
                                  .global = GlobalType { .type = I64, .mutability = Mutability::Var } };
    CHECK(isoline::validate::externTypeOf(import) == globalOf(I64, Mutability::Var));
}

namespace
{

/// A constant expression of @p type for a global: zero, a null reference, or ref.func of function 0.
std::vector<uint8_t> initOf(ValueType const& type)
{
    switch (type.kind)
    {
        case ValueKind::I32: return { 0x41, 0x00 };
        case ValueKind::I64: return { 0x42, 0x00 };
        case ValueKind::Ref: break;
        default: FAIL("no initializer for this row's global type"); return {};
    }
    if (type.nullable == Nullable::No)
        return { 0xD2, 0x00 }; // ref.func 0
    auto bytes = std::vector<uint8_t> { 0xD0 };
    auto const heap = isoline::binary::testing::encodeHeapType(type.heap);
    bytes.insert(bytes.end(), heap.begin(), heap.end());
    return bytes;
}

/// A module of @p types that defines an entity of type @p provided and exports it as "e".
ModuleBuilder providing(std::vector<FuncType> const& types, CoreExternType const& provided)
{
    auto builder = ModuleBuilder {};
    for (auto const& type: types)
        builder.type(type);
    switch (provided.kind)
    {
        case ExternKind::Func: builder.function(provided.typeIndex, {}, { 0x00 }); break;
        case ExternKind::Table: builder.table(provided.table); break;
        case ExternKind::Memory: builder.memory(provided.memory); break;
        case ExternKind::Tag: builder.tag(provided.typeIndex); break;
        case ExternKind::Global:
            if (provided.global.type.kind == ValueKind::Ref && provided.global.type.nullable == Nullable::No)
            {
                // ref.func 0 initializes it: a function of the concrete type it names.
                REQUIRE(provided.global.type.heap.kind == isoline::binary::AbstractHeapType::Concrete);
                builder.function(provided.global.type.heap.typeIndex, {}, { 0x00 });
            }
            builder.global(provided.global, initOf(provided.global.type));
            break;
    }
    builder.exportEntity("e", provided.kind, 0);
    return builder;
}

/// A module of @p types that imports "a" "e" as @p required.
ModuleBuilder requiring(std::vector<FuncType> const& types, CoreExternType const& required)
{
    auto builder = ModuleBuilder {};
    for (auto const& type: types)
        builder.type(type);
    switch (required.kind)
    {
        case ExternKind::Func: builder.importFunction("a", "e", required.typeIndex); break;
        case ExternKind::Table: builder.importTable("a", "e", required.table); break;
        case ExternKind::Memory: builder.importMemory("a", "e", required.memory); break;
        case ExternKind::Global: builder.importGlobal("a", "e", required.global); break;
        case ExternKind::Tag: builder.importTag("a", "e", required.typeIndex); break;
    }
    return builder;
}

/// @p builder's module, which must be valid with every feature.
std::shared_ptr<ValidatedModule const> validated(ModuleBuilder const& builder)
{
    auto loaded = ValidatedModule::load(builder.build(), FeatureSet::all(), isoline::binary::DecodeLimits {});
    REQUIRE(loaded.has_value());
    return std::move(*loaded);
}

/// Whether the runtime links an instance of @p row's requiring module to its providing module's export.
bool linkerLinks(MatchRow const& row)
{
    auto allocator = isoline::runtime::HeapMemoryAllocator {};
    auto store = isoline::runtime::Store { isoline::runtime::StoreLimits {},
                                           isoline::runtime::StoreServices { .allocator = allocator } };
    auto interpreter = isoline::interpreter::ReferenceInterpreter {};
    auto const provider =
        isoline::runtime::instantiate(store, interpreter, validated(providing(row.subTypes, row.sub)), {});
    REQUIRE(provider.has_value());
    auto exported = std::optional<isoline::runtime::ExternValue> {};
    for (auto const& [name, value]: store.moduleInstance(*provider).exports)
        if (name == "e")
            exported = value;
    REQUIRE(exported.has_value());
    auto const imports = std::vector { *exported };
    auto const requirer = isoline::runtime::instantiate(
        store, interpreter, validated(requiring(row.superTypes, row.super)), imports);
    if (requirer.has_value())
        return true;
    CHECK(requirer.error()
          == isoline::runtime::InstantiationError {
              isoline::runtime::LinkError { isoline::runtime::LinkErrorCode::IncompatibleImportType, 0 } });
    return false;
}

} // namespace

TEST_CASE("TypeMatching.agreesWithLinker")
{
    for (auto const& row: allRows())
    {
        INFO(row.what);
        auto const verdict = matchesExtern(row.subTypes, row.sub, row.superTypes, row.super);
        CHECK(verdict == (row.expected == Expected::Match));
        CHECK(linkerLinks(row) == verdict);
    }
}

namespace
{

/// A chain of @p length types after @p prefix: (func), then each (func (param (ref null $previous))),
/// the last of them (func (param i32)) instead when @p bottom says so.
std::vector<FuncType> chain(std::vector<FuncType> prefix, std::size_t length, FuncType bottom = func({}))
{
    auto types = std::move(prefix);
    types.reserve(types.size() + length);
    types.push_back(std::move(bottom));
    for ([[maybe_unused]] auto const level: std::views::iota(std::size_t { 1 }, length))
        types.push_back(func({ refNull(static_cast<uint32_t>(types.size() - 1)) }));
    return types;
}

} // namespace

TEST_CASE("TypeMatching.deepChainsUseNoRecursion")
{
    // Deep enough that a recursive comparison would exhaust the stack under ASan.
    constexpr auto Length = std::size_t { 100'000 };
    auto const sub = chain({}, Length);
    auto const super = chain({ func({ I64 }) }, Length);
    auto const differentBottom = chain({ func({ I64 }) }, Length, func({ I32 }));
    auto const subTypes = CanonicalTypes { sub };
    auto const superTypes = CanonicalTypes { super };
    auto const otherTypes = CanonicalTypes { differentBottom };

    auto matcher = TypeMatcher { subTypes, superTypes };
    CHECK(matcher.matches(static_cast<uint32_t>(Length - 1), static_cast<uint32_t>(Length)));
    CHECK(matcher.comparisons() == Length);
    // Every other pair of the chain is known now: no further comparison.
    for (auto const level: std::views::iota(std::size_t { 0 }, Length))
        REQUIRE(matcher.matches(static_cast<uint32_t>(level), static_cast<uint32_t>(level + 1)));
    CHECK(matcher.comparisons() == Length);

    auto mismatching = TypeMatcher { subTypes, otherTypes };
    CHECK_FALSE(mismatching.matches(static_cast<uint32_t>(Length - 1), static_cast<uint32_t>(Length)));
    CHECK(mismatching.comparisons() == Length);
    // The pairs on the failed path are remembered as different.
    for (auto const level: std::views::iota(std::size_t { 0 }, Length))
        REQUIRE_FALSE(mismatching.matches(static_cast<uint32_t>(level), static_cast<uint32_t>(level + 1)));
    CHECK(mismatching.comparisons() == Length);
}

TEST_CASE("TypeMatching.duplicatesCompareOnce")
{
    // P copies of one type on the sub side, Q on the super side, and a root naming every pairing of
    // them: compared by index that is P * Q pairs; by canonical id, one.
    constexpr auto Copies = uint32_t { 200 };
    auto sub = std::vector { func({}) };
    auto super = std::vector { func({}) };
    for ([[maybe_unused]] auto const copy: std::views::iota(uint32_t { 0 }, Copies))
    {
        sub.push_back(func({ refNull(0) }));
        super.push_back(func({ refNull(0) }));
    }
    auto subRoot = func({});
    auto superRoot = func({});
    for (auto const i: std::views::iota(uint32_t { 1 }, Copies + 1))
        for (auto const j: std::views::iota(uint32_t { 1 }, Copies + 1))
        {
            subRoot.params.push_back(refNull(i));
            superRoot.params.push_back(refNull(j));
        }
    sub.push_back(std::move(subRoot));
    super.push_back(std::move(superRoot));

    auto const subTypes = CanonicalTypes { sub };
    auto const superTypes = CanonicalTypes { super };
    auto matcher = TypeMatcher { subTypes, superTypes };
    CHECK(matcher.matches(Copies + 1, Copies + 1));
    CHECK(matcher.comparisons() == 3); // the roots, one copy each, the (func) each copy names
}

TEST_CASE("TypeMatching.manyQueriesShareOneMemo")
{
    // Each import of a module naming one deep type: the chain is compared once for all of them.
    constexpr auto Length = std::size_t { 1'000 };
    constexpr auto Imports = std::size_t { 1'000 };
    auto const sub = chain({}, Length);
    auto const super = chain({}, Length, func({ I32 }));
    auto const subTypes = CanonicalTypes { sub };
    auto const superTypes = CanonicalTypes { super };
    auto matcher = TypeMatcher { subTypes, superTypes };
    for ([[maybe_unused]] auto const import: std::views::iota(std::size_t { 0 }, Imports))
        REQUIRE_FALSE(matcher.matchesExtern(funcOf(static_cast<uint32_t>(Length - 1)),
                                            funcOf(static_cast<uint32_t>(Length - 1))));
    CHECK(matcher.comparisons() == Length);
}

TEST_CASE("TypeMatching.onePartnerEach")
{
    // A module's canonical types are pairwise inequivalent, so a type equivalent to one of the other
    // module's is equivalent to no other of them: known without a comparison.
    auto const sub = std::vector { func({ I32 }), func({ I64 }) };
    auto const super = std::vector { func({ I32 }), func({ I64 }) };
    auto const subTypes = CanonicalTypes { sub };
    auto const superTypes = CanonicalTypes { super };
    auto matcher = TypeMatcher { subTypes, superTypes };
    CHECK(matcher.matches(0, 0));
    CHECK(matcher.comparisons() == 1);
    CHECK_FALSE(matcher.matches(1, 0)); // 0 of the super side has its partner
    CHECK_FALSE(matcher.matches(0, 1)); // 0 of the sub side has its partner
    CHECK(matcher.comparisons() == 1);
    CHECK(matcher.matches(1, 1));
    CHECK(matcher.comparisons() == 2);
}

TEST_CASE("TypeMatching.knownDifferenceEndsAComparison")
{
    // A pair found different ends every later comparison that reaches it, without comparing it again.
    auto const sub = std::vector { func({ I32 }), func({ refNull(0) }), func({ refNull(0), I64 }) };
    auto const super = std::vector { func({ I64 }), func({ refNull(0) }), func({ refNull(0), I64 }) };
    auto const subTypes = CanonicalTypes { sub };
    auto const superTypes = CanonicalTypes { super };
    auto matcher = TypeMatcher { subTypes, superTypes };
    CHECK_FALSE(matcher.matches(1, 1)); // (1, 1), then (0, 0), which differs
    CHECK(matcher.comparisons() == 2);
    CHECK_FALSE(matcher.matches(2, 2)); // (2, 2) alone: its child (0, 0) is known to differ
    CHECK(matcher.comparisons() == 3);
}
