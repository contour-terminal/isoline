// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/CoreTypes.hpp>
#include <isoline/validate/ValidationError.hpp>

using isoline::binary::AddressType;
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
using isoline::binary::TableType;
using isoline::component::types::CoreEntityType;
using isoline::component::types::TypeArena;
using isoline::component::validate::CoreMismatchCode;
using isoline::component::validate::coreModuleTypeOf;
using isoline::component::validate::coreValueTypeText;
using isoline::component::validate::matchCoreEntity;

namespace
{

[[nodiscard]] CoreEntityType table(TableType type)
{
    return CoreEntityType { .kind = ExternKind::Table, .table = type };
}

[[nodiscard]] CoreEntityType memory(MemoryType type)
{
    return CoreEntityType { .kind = ExternKind::Memory, .memory = type };
}

[[nodiscard]] CoreEntityType global(GlobalType type)
{
    return CoreEntityType { .kind = ExternKind::Global, .global = type };
}

} // namespace

TEST_CASE("CoreTypes.everyEntityRule")
{
    // SubtypeCx::entity_type's rules for tables, memories and globals, one row per mismatch, in the
    // order wasm-tools checks them, with its texts.
    auto arena = TypeArena { isoline::component::ComponentLimits {} };
    auto const unary = arena.intern(FuncType { .params = { I32 }, .results = {} });
    auto const nullary = arena.intern(FuncType {});
    auto const sameIds = [](uint32_t sub, uint32_t super) {
        return sub == super;
    };

    struct Row
    {
        std::string_view name;
        CoreEntityType sub;
        CoreEntityType required;
        CoreMismatchCode code;
        std::string_view text;
    };
    auto const ten = Limits { .min = 10, .max = 20 };
    auto const rows = std::vector<Row> {
        { .name = "table element",
          .sub = table(TableType { .element = FuncRef, .limits = ten }),
          .required = table(TableType { .element = ExternRef, .limits = ten }),
          .code = CoreMismatchCode::TableElementType,
          .text = "expected table element type externref, found funcref" },
        { .name = "table index type",
          .sub = table(TableType { .limits = ten }),
          .required =
              table(TableType { .limits = Limits { .min = 10, .max = 20, .addressType = AddressType::I64 } }),
          .code = CoreMismatchCode::TableIndexType,
          .text = "mismatch in index type used for tables" },
        { .name = "table limits",
          .sub = table(TableType { .limits = Limits { .min = 5, .max = 20 } }),
          .required = table(TableType { .limits = ten }),
          .code = CoreMismatchCode::TableLimits,
          .text = "mismatch in table limits" },
        { .name = "memory index type",
          .sub = memory(MemoryType {
              .limits = Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I64 } }),
          .required = memory(MemoryType { .limits = Limits { .min = 1, .max = std::nullopt } }),
          .code = CoreMismatchCode::MemoryIndexType,
          .text = "mismatch in index type used for memories" },
        { .name = "memory page size",
          .sub = memory(MemoryType { .limits = Limits { .min = 1, .max = std::nullopt }, .pageSizeLog2 = 0 }),
          .required = memory(MemoryType { .limits = Limits { .min = 1, .max = std::nullopt } }),
          .code = CoreMismatchCode::MemoryPageSize,
          .text = "mismatch in page size for memories" },
        { .name = "memory limits",
          .sub = memory(MemoryType { .limits = Limits { .min = 1, .max = std::nullopt } }),
          .required = memory(MemoryType { .limits = Limits { .min = 1, .max = 2 } }),
          .code = CoreMismatchCode::MemoryLimits,
          .text = "mismatch in memory limits" },
        { .name = "global mutability",
          .sub = global(GlobalType { .type = I32, .mutability = Mutability::Var }),
          .required = global(GlobalType { .type = I32 }),
          .code = CoreMismatchCode::GlobalMutability,
          .text = "global types differ in mutability" },
        { .name = "global type",
          .sub = global(GlobalType { .type = I64 }),
          .required = global(GlobalType { .type = I32 }),
          .code = CoreMismatchCode::GlobalType,
          .text = "expected global type i32, found i64" },
        { .name = "immutable global of a supertype",
          .sub = global(GlobalType { .type = FuncRef }),
          .required = global(GlobalType { .type = NonNullFuncRef }),
          .code = CoreMismatchCode::GlobalType,
          .text = "expected global type (ref func), found funcref" },
        { .name = "mutable global of a subtype",
          .sub = global(GlobalType { .type = NonNullFuncRef, .mutability = Mutability::Var }),
          .required = global(GlobalType { .type = FuncRef, .mutability = Mutability::Var }),
          .code = CoreMismatchCode::GlobalType,
          .text = "expected global type funcref, found (ref func)" },
        { .name = "kind",
          .sub = memory(MemoryType {}),
          .required = global(GlobalType { .type = I32 }),
          .code = CoreMismatchCode::KindMismatch,
          .text = "expected global, found memory" },
        { .name = "function type",
          .sub = CoreEntityType { .kind = ExternKind::Func, .typeIndex = nullary.value },
          .required = CoreEntityType { .kind = ExternKind::Func, .typeIndex = unary.value },
          .code = CoreMismatchCode::FunctionType,
          .text = "expected: (func (param i32))\nfound:    (func)" },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const matched = matchCoreEntity(arena, row.sub, arena, row.required, sameIds);
        REQUIRE_FALSE(matched.has_value());
        CHECK(matched.error().code == row.code);
        CHECK(matched.error().message == row.text);
    }

    // What each rule admits: equal entities, a larger table and memory minimum within the maximum, an
    // immutable global of a subtype.
    auto const admitted = std::vector<std::pair<CoreEntityType, CoreEntityType>> {
        { table(TableType { .limits = Limits { .min = 15, .max = 20 } }),
          table(TableType { .limits = ten }) },
        { memory(MemoryType { .limits = Limits { .min = 2, .max = 2 } }),
          memory(MemoryType { .limits = Limits { .min = 1, .max = 4 } }) },
        { global(GlobalType { .type = NonNullFuncRef }), global(GlobalType { .type = FuncRef }) },
        { global(GlobalType { .type = I32, .mutability = Mutability::Var }),
          global(GlobalType { .type = I32, .mutability = Mutability::Var }) },
    };
    for (auto const& [sub, required]: admitted)
        CHECK(matchCoreEntity(arena, sub, arena, required, sameIds).has_value());
}

TEST_CASE("CoreTypes.valueTypeTexts")
{
    // AbstractHeapType::as_str's spellings, nullable as `<word>ref`, non-null as `(ref <word>)`.
    CHECK(coreValueTypeText(I64) == "i64");
    CHECK(coreValueTypeText(FuncRef) == "funcref");
    CHECK(coreValueTypeText(ExternRef) == "externref");
    CHECK(coreValueTypeText(NonNullFuncRef) == "(ref func)");
    CHECK(coreValueTypeText(isoline::binary::NullExnRef) == "nullexnref");
    CHECK(coreValueTypeText(isoline::binary::concreteRef(3, isoline::binary::Nullable::Yes))
          == "(ref null 3)");
    CHECK(coreValueTypeText(isoline::binary::concreteRef(3, isoline::binary::Nullable::No)) == "(ref 3)");
}

TEST_CASE("CoreTypes.moduleExportsOutsideTheirIndexSpace")
{
    // A module with one definition of each kind: exporting each is read, and exporting the index after it
    // is the core validator's unknown-index error, never an out-of-range read.
    using isoline::validate::ValidationErrorCode;
    auto module = isoline::binary::Module {};
    module.types.push_back(FuncType {});
    module.functions.push_back(0);
    module.tables.push_back(
        isoline::binary::Table { .type = TableType { .element = FuncRef, .limits = Limits {} }, .init = {} });
    module.memories.push_back(MemoryType {});
    module.globals.push_back(isoline::binary::Global { .type = GlobalType { .type = I32 }, .init = {} });
    module.tags.push_back(isoline::binary::Tag { .typeIndex = 0 });
    struct Row
    {
        ExternKind kind;
        ValidationErrorCode unknown;
    };
    auto const rows = std::vector<Row> {
        { .kind = ExternKind::Func, .unknown = ValidationErrorCode::UnknownFunction },
        { .kind = ExternKind::Table, .unknown = ValidationErrorCode::UnknownTable },
        { .kind = ExternKind::Memory, .unknown = ValidationErrorCode::UnknownMemory },
        { .kind = ExternKind::Global, .unknown = ValidationErrorCode::UnknownGlobal },
        { .kind = ExternKind::Tag, .unknown = ValidationErrorCode::UnknownTag },
    };
    for (auto const& row: rows)
    {
        auto arena = TypeArena { isoline::component::ComponentLimits {} };
        module.exports = { isoline::binary::Export { .name = "in", .kind = row.kind, .index = 0 } };
        auto const read = coreModuleTypeOf(arena, module);
        REQUIRE(read.has_value());
        REQUIRE(read->exports.size() == 1);
        CHECK(read->exports.front().type.kind == row.kind);
        module.exports = { isoline::binary::Export { .name = "out", .kind = row.kind, .index = 1 } };
        auto const refused = coreModuleTypeOf(arena, module);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == row.unknown);
        CHECK(refused.error().subject == 1U);
    }
}
