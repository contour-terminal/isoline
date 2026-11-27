// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

using isoline::api::Abrupt;
using isoline::api::Engine;
using isoline::api::EngineConfig;
using isoline::api::ExternKind;
using isoline::api::FuncType;
using isoline::api::GlobalType;
using isoline::api::HeapMemoryAllocator;
using isoline::api::HostContext;
using isoline::api::IHostFunction;
using isoline::api::Imports;
using isoline::api::Linkage;
using isoline::api::makeHostFunction;
using isoline::api::Module;
using isoline::api::Store;
using isoline::api::Value;
using isoline::binary::AddressType;
using isoline::binary::Limits;
using isoline::binary::Mutability;
using isoline::binary::TableType;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::ModuleBuilder;

namespace
{

constexpr auto TypeI32 = isoline::binary::I32;
constexpr auto TypeI64 = isoline::binary::I64;

/// A size of @p count pages (or elements, for a table), unbounded, of 32-bit addresses.
Limits pages(uint64_t count)
{
    return Limits { .min = count, .max = std::nullopt, .addressType = isoline::binary::AddressType::I32 };
}

/// A host function that does nothing.
std::shared_ptr<IHostFunction> nothing()
{
    return makeHostFunction([](HostContext&, std::span<Value const>, std::span<Value>) noexcept {
        return std::expected<void, Abrupt> {};
    });
}

/// Loads @p builder's module with a default engine.
Module load(ModuleBuilder const& builder)
{
    auto module = Engine { EngineConfig {} }.load(builder.build());
    REQUIRE(module.has_value());
    return *std::move(module);
}

/// A module that exports one of each kind but tags: "f" () -> (i32), "table" (funcref, 1), "memory"
/// (1 page), "const" (immutable i32) and "var" (mutable i32).
ModuleBuilder exporter()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { TypeI32 } })
        .function(0, {}, i32Const(1))
        .table(TableType { .element = isoline::binary::FuncRef, .limits = pages(1) })
        .memory(pages(1))
        .global(GlobalType { .type = TypeI32, .mutability = Mutability::Const }, i32Const(2))
        .global(GlobalType { .type = TypeI32, .mutability = Mutability::Var }, i32Const(3))
        .exportFunction("f", 0)
        .exportEntity("table", ExternKind::Table, 0)
        .exportEntity("memory", ExternKind::Memory, 0)
        .exportEntity("const", ExternKind::Global, 0)
        .exportEntity("var", ExternKind::Global, 1);
    return builder;
}

} // namespace

TEST_CASE("Imports.hostFunctionsResolveByNameKindAndType")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32 }, .results = {} })
        .type(FuncType { .params = { TypeI64 }, .results = {} })
        .importFunction("env", "exact", 0)
        .importFunction("env", "wrong-type", 1)
        .importFunction("env", "unknown", 0)
        .importFunction("other", "exact", 0)
        .importMemory("env", "memory-named-by-a-function", pages(1))
        .importGlobal("env", "global-named-by-a-function", GlobalType { .type = TypeI32 });
    auto imports = Imports {};
    auto const takesI32 = FuncType { .params = { TypeI32 }, .results = {} };
    imports.define("env", "exact", takesI32, nothing())
        .define("env", "wrong-type", takesI32, nothing())
        .define("env", "memory-named-by-a-function", takesI32, nothing())
        .define("env", "global-named-by-a-function", takesI32, nothing());
    CHECK(imports.resolve(load(builder))
          == std::vector { Linkage::Satisfied,
                           Linkage::Unsatisfied,
                           Linkage::Unsatisfied,
                           Linkage::Unsatisfied,
                           Linkage::Unsatisfied,
                           Linkage::Unsatisfied });
}

TEST_CASE("Imports.laterDefinitionReplacesEarlier")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI64 }, .results = {} }).importFunction("env", "f", 0);
    auto imports = Imports {};
    imports.define("env", "f", FuncType { .params = { TypeI32 }, .results = {} }, nothing());
    CHECK(imports.resolve(load(builder)) == std::vector { Linkage::Unsatisfied });
    imports.define("env", "f", FuncType { .params = { TypeI64 }, .results = {} }, nothing());
    CHECK(imports.resolve(load(builder)) == std::vector { Linkage::Satisfied });
}

TEST_CASE("Imports.instanceExportsResolveByKindAndType")
{
    auto engine = Engine { EngineConfig {} };
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto const module = engine.load(exporter().build());
    REQUIRE(module.has_value());
    auto const instance = store.instantiate(*module, Imports {});
    REQUIRE(instance.has_value());
    auto imports = Imports {};
    for (auto const* name: { "f", "table", "memory", "const", "var" })
        imports.define("other", name, *instance, name);

    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { TypeI32 } })
        .type(FuncType { .params = {}, .results = { TypeI64 } })
        .importFunction("other", "f", 0) // fits
        .importFunction("other", "f", 1) // other type
        .importTable(
            "other", "table", TableType { .element = isoline::binary::FuncRef, .limits = pages(0) }) // fits
        .importMemory("other", "memory", pages(1))                                                   // fits
        .importMemory("other", "table", pages(1))                       // other kind
        .importGlobal("other", "const", GlobalType { .type = TypeI32 }) // fits
        .importGlobal("other", "const", GlobalType { .type = TypeI64 }) // other type
        .importGlobal("other", "var", GlobalType { .type = TypeI32 })   // mutability
        .importGlobal("other", "var", GlobalType { .type = TypeI32, .mutability = Mutability::Var }); // fits
    auto const importer = engine.load(builder.build());
    REQUIRE(importer.has_value());
    CHECK(imports.resolve(*importer)
          == std::vector { Linkage::Satisfied,
                           Linkage::Unsatisfied,
                           Linkage::Satisfied,
                           Linkage::Satisfied,
                           Linkage::Unsatisfied,
                           Linkage::Satisfied,
                           Linkage::Unsatisfied,
                           Linkage::Unsatisfied,
                           Linkage::Satisfied });
}

TEST_CASE("Imports.outliveTheStoreTheyName")
{
    // A definition of an instance keeps its store's identity, not the store: resolving after the store
    // is gone still answers, and a store made afterwards never takes the dead one's identity.
    auto engine = Engine { EngineConfig {} };
    auto allocator = HeapMemoryAllocator {};
    auto imports = Imports {};
    auto const module = engine.load(exporter().build());
    REQUIRE(module.has_value());
    {
        auto store = Store { engine, allocator };
        auto const instance = store.instantiate(*module, Imports {});
        REQUIRE(instance.has_value());
        imports.define("other", "f", *instance, "f");
    }
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = {}, .results = { TypeI32 } }).importFunction("other", "f", 0);
    auto const importer = engine.load(builder.build());
    REQUIRE(importer.has_value());
    CHECK(imports.resolve(*importer) == std::vector { Linkage::Satisfied });
    auto later = Store { engine, allocator };
    auto const linked = later.instantiate(*importer, imports);
    REQUIRE_FALSE(linked.has_value());
    CHECK(linked.error() == isoline::api::InstantiateError { isoline::api::ForeignImport { .index = 0 } });
}

TEST_CASE("Imports.tablesMemoriesAndTagsResolveByTypeButSize")
{
    // A funcref table of at most 4, an externref table, a memory of at most 2 pages, a tag (i32).
    auto exporting = ModuleBuilder {};
    exporting.type(FuncType { .params = { TypeI32 }, .results = {} })
        .table(TableType { .element = isoline::binary::FuncRef,
                           .limits = { .min = 1, .max = 4, .addressType = AddressType::I32 } })
        .table(TableType { .element = isoline::binary::ExternRef, .limits = pages(1) })
        .memory(Limits { .min = 1, .max = 2, .addressType = AddressType::I32 })
        .tag(0)
        .exportEntity("funcs", ExternKind::Table, 0)
        .exportEntity("externs", ExternKind::Table, 1)
        .exportEntity("memory", ExternKind::Memory, 0)
        .exportEntity("e", ExternKind::Tag, 0);
    auto engine = Engine { EngineConfig {} };
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto const module = engine.load(exporting.build());
    REQUIRE(module.has_value());
    auto const instance = store.instantiate(*module, Imports {});
    REQUIRE(instance.has_value());
    auto imports = Imports {};
    for (auto const* name: { "funcs", "externs", "memory", "e" })
        imports.define("other", name, *instance, name);

    auto const funcTable = [](std::optional<uint64_t> max) {
        return TableType { .element = isoline::binary::FuncRef,
                           .limits = { .min = 1, .max = max, .addressType = AddressType::I32 } };
    };
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32 }, .results = {} })
        .type(FuncType {})
        .importTable("other", "funcs", funcTable(8))    // fits: a maximum within the required
        .importTable("other", "funcs", funcTable(2))    // its maximum exceeds the required one
        .importTable("other", "externs", funcTable({})) // externref for funcref
        .importTable("other", "funcs", funcTable({}))   // fits: no maximum required
        .importMemory("other", "memory", pages(1))      // fits: no maximum required
        .importMemory("other", "memory", Limits { .min = 1, .max = 1, .addressType = AddressType::I32 })
        .importMemory(
            "other", "memory", Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I64 })
        .importTag("other", "e", 0)  // fits
        .importTag("other", "e", 1); // another type
    auto const importer = engine.load(builder.build());
    REQUIRE(importer.has_value());
    CHECK(imports.resolve(*importer)
          == std::vector { Linkage::Satisfied,
                           Linkage::Unsatisfied,
                           Linkage::Unsatisfied,
                           Linkage::Satisfied,
                           Linkage::Satisfied,
                           Linkage::Unsatisfied,
                           Linkage::Unsatisfied,
                           Linkage::Satisfied,
                           Linkage::Unsatisfied });
}

TEST_CASE("Imports.storeFunctionsResolveByType")
{
    auto engine = Engine { EngineConfig {} };
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto const function = store.define(FuncType { .params = { TypeI32 }, .results = {} }, nothing());
    REQUIRE(function.has_value());
    auto imports = Imports {};
    imports.define("env", "f", *function);
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { TypeI32 }, .results = {} })
        .type(FuncType {})
        .importFunction("env", "f", 0)
        .importFunction("env", "f", 1)
        .importGlobal("env", "f", GlobalType { .type = TypeI32 });
    CHECK(imports.resolve(load(builder))
          == std::vector { Linkage::Satisfied, Linkage::Unsatisfied, Linkage::Unsatisfied });
}

TEST_CASE("Imports.concreteTypesOnBothSidesAreLeftToTheStore")
{
    // A function and an immutable global whose types name a concrete type index of their own module, on
    // both sides: only a store can compare those indices, so resolve() reports them satisfied and
    // instantiation decides.
    using isoline::binary::AbstractHeapType;
    using isoline::binary::HeapType;
    using isoline::binary::ValueKind;
    auto const concrete = isoline::binary::ValueType { .kind = ValueKind::Ref,
                                                       .nullable = isoline::binary::Nullable::Yes,
                                                       .heap = HeapType { .kind = AbstractHeapType::Concrete,
                                                                          .typeIndex = 0 } };
    auto const takesConcrete = FuncType { .params = { concrete }, .results = {} };
    auto engine = Engine { EngineConfig {} };
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto exporting = ModuleBuilder {};
    exporting.type(FuncType {})
        .type(takesConcrete)
        .function(1, {}, {})
        .global(GlobalType { .type = concrete, .mutability = Mutability::Const },
                { 0xD0, 0x00 }) // ref.null 0
        .exportFunction("f", 0)
        .exportEntity("g", ExternKind::Global, 0);
    auto const exporter = engine.load(exporting.build());
    REQUIRE(exporter.has_value());
    auto const instance = store.instantiate(*exporter, Imports {});
    REQUIRE(instance.has_value());
    auto imports = Imports {};
    imports.define("other", "f", *instance, "f").define("other", "g", *instance, "g");

    auto importing = ModuleBuilder {};
    importing.type(FuncType {})
        .type(takesConcrete)
        .importFunction("other", "f", 1)
        .importGlobal("other", "g", GlobalType { .type = concrete, .mutability = Mutability::Const });
    auto const importer = engine.load(importing.build());
    REQUIRE(importer.has_value());
    CHECK(imports.resolve(*importer) == std::vector { Linkage::Satisfied, Linkage::Satisfied });
    // The store compares them: the same type, so it links.
    CHECK(store.instantiate(*importer, imports).has_value());

    // The same spelling, (ref null 0), names another type in a module whose type 0 differs: resolve()
    // cannot tell, and still reports it satisfied; the store can, and refuses to link it.
    auto differing = ModuleBuilder {};
    differing.type(FuncType { .params = { TypeI32 }, .results = {} })
        .type(takesConcrete)
        .importFunction("other", "f", 1);
    auto const mistyped = engine.load(differing.build());
    REQUIRE(mistyped.has_value());
    CHECK(imports.resolve(*mistyped) == std::vector { Linkage::Satisfied });
    auto const refused = store.instantiate(*mistyped, imports);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error()
          == isoline::api::InstantiateError {
              isoline::api::LinkError { isoline::api::LinkErrorCode::IncompatibleImportType, 0 } });
}
