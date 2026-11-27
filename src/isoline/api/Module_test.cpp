// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/testing/ModuleAccess.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/validate/ValidatedModule.hpp>

using isoline::FeatureSet;
using isoline::api::Dispatch;
using isoline::api::Engine;
using isoline::api::EngineConfig;
using isoline::api::EngineMismatch;
using isoline::api::ExecutionEngine;
using isoline::api::ExecutionMode;
using isoline::api::ExternType;
using isoline::api::Module;
using isoline::api::TagType;
using isoline::api::testing::ModuleAccess;
using isoline::binary::ExternKind;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::I32;
using isoline::binary::I64;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::binary::Mutability;
using isoline::binary::TableType;
using isoline::binary::testing::ModuleBuilder;

namespace
{

/// Loads @p bytes with a default engine, requiring that it succeeds.
Module loaded(std::vector<std::byte> const& bytes)
{
    auto module = Engine { EngineConfig {} }.load(bytes);
    REQUIRE(module.has_value());
    return *std::move(module);
}

/// The type a description names, required to be of alternative @p T.
template <typename T>
T typeAs(ExternType const& type)
{
    REQUIRE(std::holds_alternative<T>(type));
    return std::get<T>(type);
}

/// Whether a module of reference type @p T reads its imports, exports and engine.
template <typename T>
concept DescribesWhenTemporary = requires(T&& module) {
    std::forward<T>(module).imports();
    std::forward<T>(module).exports();
    std::forward<T>(module).engine();
};

/// A copy of @p module, as an embedder hands a module to another owner.
Module copyOf(Module const& module)
{
    return module;
}

/// The validated module @p module describes.
auto const& validatedOf(Module const& module)
{
    return ModuleAccess::validatedOf(module);
}

} // namespace

TEST_CASE("Module.describesImportsTruthfully")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { I64, I32 }, .results = {} }).importFunction("nowhere", "f", 0);
    // Nothing satisfies the import, and loading does not ask (spec §7).
    auto const module = loaded(builder.build());

    REQUIRE(module.imports().size() == 1);
    auto const& import = module.imports()[0];
    CHECK(import.module == "nowhere");
    CHECK(import.name == "f");
    CHECK(*typeAs<FuncType const*>(import.type) == FuncType { .params = { I64, I32 }, .results = {} });
    CHECK(module.exports().empty());
}

TEST_CASE("Module.describesEveryKindOfImport")
{
    auto const table = TableType { .element = isoline::binary::FuncRef, .limits = { .min = 1, .max = {} } };
    auto const memory = MemoryType { .limits = { .min = 2, .max = 3 } };
    auto const global = GlobalType { .type = I64, .mutability = Mutability::Var };
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { I32 }, .results = {} })
        .importFunction("a", "func", 0)
        .importTable("b", "table", table)
        .importMemory("c", "memory", memory)
        .importGlobal("d", "global", global)
        .importTag("e", "tag", 0);
    auto const module = loaded(builder.build());
    auto const& decoded = validatedOf(module)->module();

    auto const imports = module.imports();
    REQUIRE(imports.size() == 5);
    CHECK(imports[0].module == "a");
    CHECK(typeAs<FuncType const*>(imports[0].type) == decoded.types.data());
    CHECK(imports[1].name == "table");
    CHECK(typeAs<TableType const*>(imports[1].type)->limits.min == 1);
    CHECK(typeAs<MemoryType const*>(imports[2].type)->limits.max == 3);
    CHECK(typeAs<GlobalType const*>(imports[3].type)->mutability == Mutability::Var);
    CHECK(imports[4].module == "e");
    CHECK(typeAs<TagType>(imports[4].type).type == decoded.types.data());
}

TEST_CASE("Module.describesExportsOfImportedAndDefinedEntities")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { I32 }, .results = {} })
        .type(FuncType { .params = { I64 }, .results = {} })
        .importFunction("m", "f", 0)
        .importGlobal("m", "g", GlobalType { .type = I32 })
        .importTag("m", "t", 0)
        .function(1, {}, {})
        .table(TableType { .element = isoline::binary::FuncRef, .limits = { .min = 4, .max = {} } })
        .memory(Limits { .min = 5, .max = {} })
        .global(GlobalType { .type = I64, .mutability = Mutability::Var }, { 0x42, 0x00 })
        .tag(1)
        .exportEntity("importedFunc", ExternKind::Func, 0)
        .exportEntity("definedFunc", ExternKind::Func, 1)
        .exportEntity("table", ExternKind::Table, 0)
        .exportEntity("memory", ExternKind::Memory, 0)
        .exportEntity("importedGlobal", ExternKind::Global, 0)
        .exportEntity("definedGlobal", ExternKind::Global, 1)
        .exportEntity("importedTag", ExternKind::Tag, 0)
        .exportEntity("definedTag", ExternKind::Tag, 1);
    auto const module = loaded(builder.build());
    auto const& decoded = validatedOf(module)->module();

    auto const exports = module.exports();
    REQUIRE(exports.size() == 8);
    CHECK(exports[0].name == "importedFunc");
    CHECK(typeAs<FuncType const*>(exports[0].type) == decoded.types.data());
    CHECK(exports[1].name == "definedFunc");
    CHECK(typeAs<FuncType const*>(exports[1].type) == &decoded.types[1]);
    CHECK(typeAs<TableType const*>(exports[2].type) == &decoded.tables.front().type);
    CHECK(typeAs<MemoryType const*>(exports[3].type) == decoded.memories.data());
    CHECK(typeAs<GlobalType const*>(exports[4].type)->type == I32);
    CHECK(typeAs<GlobalType const*>(exports[5].type) == &decoded.globals.front().type);
    CHECK(typeAs<TagType>(exports[6].type).type == decoded.types.data());
    CHECK(typeAs<TagType>(exports[7].type).type == &decoded.types[1]);
}

TEST_CASE("Module.copiesShareTheirDescriptions")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).importFunction("m", "f", 0).exportFunction("f", 0);
    auto const module = loaded(builder.build());
    auto const copy = copyOf(module);

    CHECK(validatedOf(copy) == validatedOf(module));
    CHECK(copy.imports().data() == module.imports().data());
    CHECK(copy.exports().data() == module.exports().data());
    CHECK(copy.imports()[0].name.data() == validatedOf(module)->module().imports[0].name.data());
    CHECK(copy.features() == FeatureSet::wasm3());
}

TEST_CASE("Module.carriesItsEngineIdentity")
{
    auto const bytes = ModuleBuilder {}.build();
    auto const production = EngineConfig {};
    auto const deterministic = EngineConfig { .mode = ExecutionMode::Deterministic };

    auto const first = Engine { production }.load(bytes);
    auto const second = Engine { production }.load(bytes);
    auto const other = Engine { deterministic }.load(bytes);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(other.has_value());

    CHECK(first->engine() == production.identity());
    CHECK(first->engine() == second->engine());
    CHECK(first->engine() != other->engine());
    CHECK(other->engine().mode == ExecutionMode::Deterministic);

    // Features are part of it: a wasm2 engine's module is not a wasm3 engine's.
    auto const wasm2 = Engine { EngineConfig { .features = FeatureSet::wasm2() } }.load(bytes);
    REQUIRE(wasm2.has_value());
    CHECK(wasm2->engine() != first->engine());
    CHECK(wasm2->engine().features == FeatureSet::wasm2());
    CHECK(wasm2->features() == FeatureSet::wasm2());

    // Limits are not: they decide whether a module loads, not what its code is.
    auto const narrower = Engine { EngineConfig { .load = { .decode = { .maxImports = 1 } } } }.load(bytes);
    REQUIRE(narrower.has_value());
    CHECK(narrower->engine() == first->engine());
    auto const capped = Engine { EngineConfig { .store = { .maxMemoryBytes = 1 } } }.load(bytes);
    REQUIRE(capped.has_value());
    CHECK(capped->engine() == first->engine());
}

TEST_CASE("Module.identityIncludesDispatchOnlyForFast")
{
    // The fast engine lowers its code for its dispatch, so a store of one dispatch refuses a module of the
    // other; the reference engine lowers nothing, so two of them that differ only in the dispatch share
    // their modules.
    auto const bytes = ModuleBuilder {}.build();
    auto const fastOf = [](Dispatch dispatch) {
        return EngineConfig { .engine = ExecutionEngine::Fast, .dispatch = dispatch };
    };
    CHECK(fastOf(Dispatch::Switch).identity() != fastOf(Dispatch::TailCall).identity());
    CHECK(fastOf(Dispatch::Switch).identity().dispatch == Dispatch::Switch);
    auto allocator = isoline::api::HeapMemoryAllocator {};
    // Every ordered pair of distinct dispatches this build compiled: none where it compiled Switch alone.
    for (auto const loading: isoline::api::availableDispatches())
    {
        for (auto const instantiating: isoline::api::availableDispatches())
        {
            if (loading == instantiating)
                continue;
            auto const module = Engine { fastOf(loading) }.load(bytes);
            REQUIRE(module.has_value());
            auto const engine = Engine { fastOf(instantiating) };
            auto store = isoline::api::Store { engine, allocator };
            auto const instance = store.instantiate(*module, isoline::api::Imports {});
            REQUIRE_FALSE(instance.has_value());
            CHECK(std::holds_alternative<EngineMismatch>(instance.error()));
        }
    }
    auto const referenceOf = [](Dispatch dispatch) {
        return EngineConfig { .engine = ExecutionEngine::Reference, .dispatch = dispatch };
    };
    CHECK(referenceOf(Dispatch::Switch).identity() == referenceOf(Dispatch::TailCall).identity());
    CHECK_FALSE(referenceOf(Dispatch::TailCall).identity().dispatch.has_value());
    auto const module = Engine { referenceOf(Dispatch::Switch) }.load(bytes);
    REQUIRE(module.has_value());
    auto const engine = Engine { referenceOf(Dispatch::TailCall) };
    auto store = isoline::api::Store { engine, allocator };
    CHECK(store.instantiate(*module, isoline::api::Imports {}).has_value());
}

TEST_CASE("Module.movingLeavesTheSourceWhole")
{
    static_assert(std::is_copy_constructible_v<Module> && std::is_copy_assignable_v<Module>);
    // Rvalue accessors are deleted: a temporary's descriptions would dangle.
    static_assert(!DescribesWhenTemporary<Module>);
    static_assert(DescribesWhenTemporary<Module const&>);

    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).importFunction("m", "f", 0).exportFunction("f", 0);
    auto const bytes = builder.build();
    // Moved by an algorithm, as a container moves its elements: each source keeps its contents.
    auto sources = std::vector<Module> { loaded(bytes) };
    auto targets = std::vector<Module> { loaded(ModuleBuilder {}.build()) };
    std::ranges::move(sources, targets.begin());
    CHECK(targets.front().imports().size() == 1);
    CHECK(sources.front().imports().size() == 1);
    CHECK(sources.front().exports().size() == 1);
    CHECK(validatedOf(sources.front()) == validatedOf(targets.front()));
}
