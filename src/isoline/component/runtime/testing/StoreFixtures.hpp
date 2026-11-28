// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the ComponentStore and Calls tests share: a store over a loaded component on any engine the
// build provides, core modules written as text, host functions written as lambdas, and a call of an
// export that hands back its lifted results. Uses Catch2's assertions outside every callback (a failing
// REQUIRE throws, and the store's calls are noexcept).

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Engine.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/api/Component.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/binary/testing/Builder.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/Calls.hpp>
#include <isoline/component/runtime/ComponentStore.hpp>
#include <isoline/component/validate/Validator.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

namespace isoline::component::runtime::testing
{

/// Every execution engine the build provides: what the store and call tests GENERATE over (R16).
inline constexpr auto Engines = std::to_array<isoline::api::ExecutionEngine>({
    isoline::api::ExecutionEngine::Reference,
});

/// The binary of a core module written in the text format (REQUIREd to compile).
/// @param source The module's text.
/// @return Its bytes.
[[nodiscard]] inline std::vector<std::byte> coreModule(std::string_view source)
{
    auto bytes = isoline::text::compileText(source, isoline::text::TextLimits {}, isoline::text::CoreForms);
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

/// A canonical option naming an index.
/// @param kind The option.
/// @param index Its index.
/// @return It.
[[nodiscard]] inline binary::CanonOption option(binary::CanonOptionKind kind, uint32_t index = 0)
{
    return binary::CanonOption { .kind = kind, .index = index };
}

/// A host function from a callable of IHostCallee::call()'s signature.
template <typename F>
class HostFunction final: public IHostCallee
{
  public:
    explicit HostFunction(F body): _body { std::move(body) } {}

    [[nodiscard]] std::expected<void, isoline::api::HostFailure> call(
        ComponentStore& store,
        canonical::Values const& args,
        canonical::ValueBuilder& results) noexcept override
    {
        ++calls;
        return _body(store, args, results);
    }

    uint32_t calls = 0; ///< How often it ran.

  private:
    F _body;
};

/// A host function running @p body.
/// @param body Its body.
/// @return It, shared.
template <typename F>
[[nodiscard]] std::shared_ptr<HostFunction<F>> hostFunction(F body)
{
    return std::make_shared<HostFunction<F>>(std::move(body));
}

/// An embedder's failure, as a host function reports one.
/// @param code Its code.
/// @return It.
[[nodiscard]] inline isoline::api::HostFailure embedderFailure(uint32_t code = 1)
{
    return isoline::api::HostFailure { .kind = isoline::api::HostFailureKind::Embedder,
                                       .code = code,
                                       .mismatch = std::nullopt };
}

/// What a test configures of a harness.
struct HarnessOptions
{
    isoline::api::ExecutionEngine engine = isoline::api::ExecutionEngine::Reference; ///< The engine.
    isoline::api::StoreLimits storeLimits {};                                        ///< Its store caps.
    ComponentLimits limits {}; ///< The store's caps; it loads under the defaults.
};

/// An engine, a component loaded on it, and a component store of that engine.
class Harness
{
  public:
    /// Loads @p bytes (REQUIREd to load) and makes an empty store.
    /// @param bytes The component.
    /// @param options The engine, its store caps and the component caps.
    explicit Harness(std::span<std::byte const> bytes, HarnessOptions const& options = {}):
        engine { isoline::api::EngineConfig { .engine = options.engine, .store = options.storeLimits } },
        loader { engine },
        validator { engine }
    {
        auto loaded = api::Component::load(engine, bytes, api::ComponentConfig {}, loader, validator);
        if (!loaded)
        {
            if (auto const* invalid = std::get_if<validate::ValidationError>(&loaded.error()))
                FAIL("invalid: " << validate::specMessage(*invalid));
            if (auto const* malformed = std::get_if<binary::DecodeError>(&loaded.error()))
                FAIL("malformed: " << binary::specMessage(*malformed));
            FAIL("refused at load");
        }
        component.emplace(std::move(*loaded));
        store = std::make_unique<ComponentStore>(engine, allocator, options.limits);
    }

    /// Instantiates the component with @p imports.
    /// @param imports The root's imports.
    /// @return The root instance, or why it failed.
    [[nodiscard]] std::expected<uint32_t, InstantiationFailure> instantiate(
        ResolvedImports const& imports = {})
    {
        return store->instantiate(component->code(), imports);
    }

    /// Instantiates the component with @p imports, REQUIREd to succeed.
    /// @param imports The root's imports.
    /// @return The root instance.
    [[nodiscard]] uint32_t instance(ResolvedImports const& imports = {})
    {
        auto instantiated = instantiate(imports);
        REQUIRE(instantiated.has_value());
        return *instantiated;
    }

    isoline::api::Engine engine;
    api::EngineCoreModuleLoader loader;
    api::EngineCoreModuleValidator validator;
    isoline::api::HeapMemoryAllocator allocator {};
    std::optional<api::Component> component {};
    std::unique_ptr<ComponentStore> store {};
};

/// The imports of a root plan that imports @p functions, in order.
/// @param functions The host functions.
/// @return The imports.
[[nodiscard]] inline ResolvedImports importsOf(std::vector<std::shared_ptr<IHostCallee>> functions)
{
    auto imports = ResolvedImports {};
    for (auto& function: functions)
    {
        imports.imports.push_back(static_cast<uint32_t>(imports.values.size()));
        imports.values.emplace_back(std::move(function));
    }
    return imports;
}

/// The lifted export @p name of @p instance (REQUIREd).
/// @param store The store.
/// @param instance The instance.
/// @param name The export.
/// @return The lifted function.
[[nodiscard]] inline LiftedFunction const& liftedExport(ComponentStore& store,
                                                        uint32_t instance,
                                                        std::string_view name)
{
    auto const function = store.function(instance, name);
    REQUIRE(function.has_value());
    auto const* lifted = std::get_if<LiftedRef>(&*function);
    REQUIRE(lifted != nullptr);
    return store.lifted(*lifted);
}

/// What one call of an export did.
struct CallOutcome
{
    std::expected<void, CallError> result;      ///< How it ended.
    std::optional<canonical::Values> values {}; ///< Its results, when on_resolve ran.
};

/// How a call's arguments are handed over: owned by the call (destroyed once lowered), or lent.
enum class ArgumentOwnership : uint8_t
{
    Owned, ///< Moved into the call.
    Lent,  ///< Kept by the embedder.
};

/// Calls @p function with the arguments @p build makes in its parameter types.
/// @param store The store.
/// @param function The function.
/// @param build Builds the arguments (a ValueBuilder& to nothing).
/// @param ownership Whether the call owns them.
/// @param onResolved Runs inside on_resolve, before post-return, with the results.
/// @return How it ended, and its results.
template <typename Build, typename Resolved>
[[nodiscard]] CallOutcome callWith(ComponentStore& store,
                                   LiftedFunction const& function,
                                   Build build,
                                   ArgumentOwnership ownership,
                                   Resolved onResolved)
{
    auto const& arena = *function.side.arena;
    auto const& type = arena[function.type];
    auto params = std::vector<types::TypeId> {};
    for (auto const& param: type.params)
        params.push_back(param.second);
    auto lent = std::optional<canonical::Values> {};
    auto outcome = CallOutcome { .result = {} };
    auto start = [&]() -> std::expected<CallArguments, CallError> {
        auto builder = canonical::ValueBuilder { arena, params, store.liveBytes() };
        build(builder);
        auto built = builder.finish();
        if (!built)
            return std::unexpected { CallError { embedderFailure(99) } };
        if (ownership == ArgumentOwnership::Owned)
            return CallArguments { std::move(*built) };
        lent.emplace(std::move(*built));
        return CallArguments { std::cref(*lent) };
    };
    auto resolve = [&](canonical::Values values) -> std::expected<void, CallError> {
        onResolved(values);
        outcome.values.emplace(std::move(values));
        return {};
    };
    outcome.result = canonLift(store, function, start, resolve);
    return outcome;
}

/// Calls the export @p name of @p instance with owned arguments.
/// @param store The store.
/// @param instance The instance.
/// @param name The export.
/// @param build Builds the arguments.
/// @return How it ended, and its results.
template <typename Build>
[[nodiscard]] CallOutcome call(ComponentStore& store, uint32_t instance, std::string_view name, Build build)
{
    return callWith(store,
                    liftedExport(store, instance, name),
                    std::move(build),
                    ArgumentOwnership::Owned,
                    [](canonical::Values const&) {});
}

/// Calls the export @p name of @p instance without arguments.
[[nodiscard]] inline CallOutcome call(ComponentStore& store, uint32_t instance, std::string_view name)
{
    return call(store, instance, name, [](canonical::ValueBuilder&) {});
}

/// The u32 result of a call that succeeded (REQUIREd).
/// @param outcome The call.
/// @return Its first result.
[[nodiscard]] inline uint32_t u32Result(CallOutcome const& outcome)
{
    REQUIRE(outcome.result.has_value());
    REQUIRE(outcome.values.has_value());
    REQUIRE(outcome.values->rootCount() == 1);
    return outcome.values->root(0).scalar<uint32_t>();
}

/// The trap a call or an instantiation failed with (REQUIREd).
/// @param error The error.
/// @return Its trap code.
[[nodiscard]] inline canonical::TrapCode trapCodeOf(CallError const& error)
{
    auto const* trap = std::get_if<canonical::Trap>(&error);
    REQUIRE(trap != nullptr);
    return trap->code;
}

/// The trap code a failed call ended in (REQUIREd to have failed with a trap).
[[nodiscard]] inline canonical::TrapCode trapCodeOf(CallOutcome const& outcome)
{
    REQUIRE_FALSE(outcome.result.has_value());
    return trapCodeOf(outcome.result.error());
}

/// The component type `func(params…) -> result?` of a builder, as a type index.
/// @param builder The builder.
/// @param params The parameters' types, labelled `p0`, `p1`, ….
/// @param result The result's type, if any.
/// @return Its type index.
template <typename Builder>
[[nodiscard]] uint32_t functionType(Builder& builder,
                                    std::vector<binary::ValueTypeRef> const& params,
                                    std::optional<binary::ValueTypeRef> result)
{
    auto labelled = std::vector<binary::LabeledType> {};
    for (auto& param: params)
        labelled.push_back(
            binary::LabeledType { .label = "p" + std::to_string(labelled.size()), .type = param });
    return builder.type(binary::TypeDefinition {
        .type = binary::FunctionTypeDefinition {
            .asyncness = binary::Asyncness::Sync, .params = std::move(labelled), .result = result } });
}

/// B and A, two instances of one root, both importing the host resource `r` and `make: func() -> own<r>`.
/// B exports take(own<r>), take-str(own<r>, string) whose `realloc` traps, and give() -> own<r> (an own
/// make returned to B). A's run-take passes make's own to take, run-str passes it to take-str with "x",
/// and run-give drops what give returns.
[[nodiscard]] inline std::vector<std::byte> guestToGuest()
{
    using binary::CanonBuiltin;
    using binary::CanonDefinition;
    using binary::CanonOptionKind;
    using binary::CoreInlineExport;
    using binary::CoreSort;
    using binary::DefinedValueType;
    using binary::ExternDesc;
    using binary::OwnType;
    using binary::PrimitiveValueType;
    using binary::Sort;
    using binary::SortIndex;
    using binary::TypeBound;
    using binary::TypeDefinition;
    using binary::TypeIndex;
    using binary::testing::ComponentBuilder;
    auto const subResource = ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource };
    auto const ownOf = [](ComponentBuilder& builder, uint32_t resource) {
        return builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = resource } } });
    };
    auto b = ComponentBuilder {};
    {
        auto const own = ownOf(b, b.import("r", subResource));
        auto const make = b.import(
            "make",
            ExternDesc { .sort = Sort::Func, .index = functionType(b, {}, TypeIndex { .index = own }) });
        auto const memory = b.coreInstantiate(b.coreModule(coreModule(R"((module
          (memory (export "mem") 1)
          (func (export "realloc") (param i32 i32 i32 i32) (result i32) unreachable)))")),
                                              {});
        auto const lowered = b.coreInlineExports(
            { CoreInlineExport { .name = "make", .sort = CoreSort::Func, .index = b.canonLower(make, {}) } });
        auto const core = b.coreInstantiate(b.coreModule(coreModule(R"((module
          (import "h" "make" (func $make (result i32)))
          (func (export "take") (param i32))
          (func (export "take-str") (param i32 i32 i32))
          (func (export "give") (result i32) (call $make))))")),
                                            { { "h", lowered } });
        auto const options = std::vector {
            option(CanonOptionKind::Memory, b.aliasCoreExport(memory, "mem", CoreSort::Memory)),
            option(CanonOptionKind::Realloc, b.aliasCoreExport(memory, "realloc", CoreSort::Func)),
        };
        std::ignore =
            b.exportItem("take",
                         Sort::Func,
                         b.canonLift(b.aliasCoreExport(core, "take", CoreSort::Func),
                                     {},
                                     functionType(b, { TypeIndex { .index = own } }, std::nullopt)));
        std::ignore = b.exportItem(
            "take-str",
            Sort::Func,
            b.canonLift(
                b.aliasCoreExport(core, "take-str", CoreSort::Func),
                options,
                functionType(b, { TypeIndex { .index = own }, PrimitiveValueType::String }, std::nullopt)));
        std::ignore = b.exportItem("give",
                                   Sort::Func,
                                   b.canonLift(b.aliasCoreExport(core, "give", CoreSort::Func),
                                               {},
                                               functionType(b, {}, TypeIndex { .index = own })));
    }
    auto a = ComponentBuilder {};
    {
        auto const resource = a.import("r", subResource);
        auto const own = ownOf(a, resource);
        auto const make = a.import(
            "make",
            ExternDesc { .sort = Sort::Func, .index = functionType(a, {}, TypeIndex { .index = own }) });
        auto const take =
            a.import("take",
                     ExternDesc { .sort = Sort::Func,
                                  .index = functionType(a, { TypeIndex { .index = own } }, std::nullopt) });
        auto const takeStr = a.import(
            "take-str",
            ExternDesc { .sort = Sort::Func,
                         .index = functionType(
                             a, { TypeIndex { .index = own }, PrimitiveValueType::String }, std::nullopt) });
        auto const give = a.import(
            "give",
            ExternDesc { .sort = Sort::Func, .index = functionType(a, {}, TypeIndex { .index = own }) });
        auto const memory =
            a.coreInstantiate(a.coreModule(coreModule(R"((module (memory (export "mem") 1)))")), {});
        auto const memoryOption =
            option(CanonOptionKind::Memory, a.aliasCoreExport(memory, "mem", CoreSort::Memory));
        auto const lowered = a.coreInlineExports({
            CoreInlineExport { .name = "make", .sort = CoreSort::Func, .index = a.canonLower(make, {}) },
            CoreInlineExport { .name = "take", .sort = CoreSort::Func, .index = a.canonLower(take, {}) },
            CoreInlineExport { .name = "take-str",
                               .sort = CoreSort::Func,
                               .index = a.canonLower(takeStr, { memoryOption }) },
            CoreInlineExport { .name = "give", .sort = CoreSort::Func, .index = a.canonLower(give, {}) },
            CoreInlineExport { .name = "drop",
                               .sort = CoreSort::Func,
                               .index = a.canon(CanonDefinition { .builtin = CanonBuiltin::ResourceDrop,
                                                                  .immediates = { resource },
                                                                  .options = {},
                                                                  .result = std::nullopt }) },
        });
        auto const memoryExports = a.coreInlineExports(
            { CoreInlineExport { .name = "mem",
                                 .sort = CoreSort::Memory,
                                 .index = a.aliasCoreExport(memory, "mem", CoreSort::Memory) } });
        auto const core = a.coreInstantiate(a.coreModule(coreModule(R"((module
          (import "h" "make" (func $make (result i32)))
          (import "h" "take" (func $take (param i32)))
          (import "h" "take-str" (func $take-str (param i32 i32 i32)))
          (import "h" "give" (func $give (result i32)))
          (import "h" "drop" (func $drop (param i32)))
          (import "m" "mem" (memory 1))
          (data (i32.const 16) "x")
          (func (export "run-take") (call $take (call $make)))
          (func (export "run-str") (call $take-str (call $make) (i32.const 16) (i32.const 1)))
          (func (export "run-give") (call $drop (call $give)))))")),
                                            { { "h", lowered }, { "m", memoryExports } });
        for (auto const* name: { "run-take", "run-str", "run-give" })
            std::ignore = a.exportItem(name,
                                       Sort::Func,
                                       a.canonLift(a.aliasCoreExport(core, name, CoreSort::Func),
                                                   {},
                                                   functionType(a, {}, std::nullopt)));
    }
    auto root = ComponentBuilder {};
    auto const resource = root.import("r", subResource);
    auto const own = ownOf(root, resource);
    auto const make = root.import(
        "make",
        ExternDesc { .sort = Sort::Func, .index = functionType(root, {}, TypeIndex { .index = own }) });
    auto const bComponent = root.nested(std::move(b));
    auto const aComponent = root.nested(std::move(a));
    auto const shared = std::vector<std::pair<std::string, SortIndex>> {
        { "r", SortIndex { .sort = Sort::Type, .index = resource } },
        { "make", SortIndex { .sort = Sort::Func, .index = make } },
    };
    auto const bInstance = root.instantiate(bComponent, shared);
    auto arguments = shared;
    for (auto const* name: { "take", "take-str", "give" })
        arguments.emplace_back(
            name, SortIndex { .sort = Sort::Func, .index = root.aliasExport(bInstance, name, Sort::Func) });
    auto const aInstance = root.instantiate(aComponent, arguments);
    for (auto const* name: { "run-take", "run-str", "run-give" })
        std::ignore = root.exportItem(name, Sort::Func, root.aliasExport(aInstance, name, Sort::Func));
    return root.build();
}

/// The names of liftFailures()'s exports, which each fail a lift after an own lifted before it.
inline constexpr auto LiftFailureExports = std::to_array<std::string_view>({
    "run-guest-str",
    "run-guest-handle",
    "run-guest-record",
    "run-host-str",
    "run-host-handle",
    "run-host-record",
    "run-give-str",
    "run-give-handle",
    "run-give-record",
});

/// B and A, two instances of one root importing the host resource `r`, `make: func() -> own<r>`, the
/// record `rec` {h: own<r>, s: string} and three host functions: take-str(own<r>, string),
/// take2(own<r>, own<r>) and take-rec(rec). B exports those three, and give-str() -> tuple<own<r>,
/// string>, give-handle() -> tuple<own<r>, own<r>> and give-record() -> rec, each returning make's own
/// followed by a string out of bounds or the handle index 99. A's run-guest-* pass make's own followed by
/// such a value to B's take-*, its run-host-* to the host's, and its run-give-* call B's give-*
/// (LiftFailureExports).
[[nodiscard]] inline std::vector<std::byte> liftFailures()
{
    using binary::CoreInlineExport;
    using binary::CoreSort;
    using binary::DefinedValueType;
    using binary::ExternDesc;
    using binary::LabeledType;
    using binary::OwnType;
    using binary::PrimitiveValueType;
    using binary::RecordType;
    using binary::Sort;
    using binary::SortIndex;
    using binary::TupleType;
    using binary::TypeBound;
    using binary::TypeDefinition;
    using binary::TypeIndex;
    using binary::testing::ComponentBuilder;
    auto const subResource = ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource };
    // The types every scope declares over its own import of r.
    struct Scope
    {
        uint32_t resource;
        uint32_t own;
        uint32_t record;
    };
    using Signatures = std::array<std::pair<std::string, uint32_t>, 3>;
    auto const declare = [&](ComponentBuilder& builder) {
        auto const resource = builder.import("r", subResource);
        auto const own =
            builder.type(TypeDefinition { .type = DefinedValueType { OwnType { .resource = resource } } });
        auto const definition = builder.type(TypeDefinition {
            .type = DefinedValueType { RecordType {
                .fields = { LabeledType { .label = "h", .type = TypeIndex { .index = own } },
                            LabeledType { .label = "s", .type = PrimitiveValueType::String } } } } });
        auto const record = builder.import(
            "rec", ExternDesc { .sort = Sort::Type, .index = definition, .bound = TypeBound::Equal });
        return Scope { .resource = resource, .own = own, .record = record };
    };
    auto const takes = [](ComponentBuilder& builder, Scope const& scope) {
        auto const own = TypeIndex { .index = scope.own };
        return Signatures {
            std::pair { std::string { "take-str" },
                        functionType(builder, { own, PrimitiveValueType::String }, std::nullopt) },
            std::pair { std::string { "take2" }, functionType(builder, { own, own }, std::nullopt) },
            std::pair { std::string { "take-rec" },
                        functionType(builder, { TypeIndex { .index = scope.record } }, std::nullopt) },
        };
    };
    auto const gives = [](ComponentBuilder& builder, Scope const& scope) {
        auto const own = TypeIndex { .index = scope.own };
        auto const tupleOf = [&](binary::ValueTypeRef second) {
            return TypeIndex {
                .index = builder.type(
                    TypeDefinition { .type = DefinedValueType { TupleType { .elements = { own, second } } } })
            };
        };
        return Signatures {
            std::pair { std::string { "give-str" },
                        functionType(builder, {}, tupleOf(PrimitiveValueType::String)) },
            std::pair { std::string { "give-handle" }, functionType(builder, {}, tupleOf(own)) },
            std::pair { std::string { "give-record" },
                        functionType(builder, {}, TypeIndex { .index = scope.record }) },
        };
    };
    auto const memoryOptions = [](ComponentBuilder& builder, uint32_t instance) {
        return std::vector {
            option(binary::CanonOptionKind::Memory,
                   builder.aliasCoreExport(instance, "mem", CoreSort::Memory)),
            option(binary::CanonOptionKind::Realloc,
                   builder.aliasCoreExport(instance, "realloc", CoreSort::Func)),
        };
    };
    constexpr auto MemoryModule = R"((module
      (memory (export "mem") 1)
      (func (export "realloc") (param i32 i32 i32 i32) (result i32) (i32.const 1024))))";
    auto const importMake = [](ComponentBuilder& builder, Scope const& scope) {
        return builder.import(
            "make",
            ExternDesc { .sort = Sort::Func,
                         .index = functionType(builder, {}, TypeIndex { .index = scope.own }) });
    };

    auto b = ComponentBuilder {};
    {
        auto const scope = declare(b);
        auto const make = importMake(b, scope);
        auto const memory = b.coreInstantiate(b.coreModule(coreModule(MemoryModule)), {});
        auto const options = memoryOptions(b, memory);
        auto const lowered = b.coreInlineExports({
            CoreInlineExport { .name = "make", .sort = CoreSort::Func, .index = b.canonLower(make, {}) },
            CoreInlineExport { .name = "mem",
                               .sort = CoreSort::Memory,
                               .index = b.aliasCoreExport(memory, "mem", CoreSort::Memory) },
        });
        auto const core = b.coreInstantiate(b.coreModule(coreModule(R"((module
          (import "h" "make" (func $make (result i32)))
          (import "h" "mem" (memory 1))
          (func (export "take-str") (param i32 i32 i32))
          (func (export "take2") (param i32 i32))
          (func (export "take-rec") (param i32 i32 i32))
          (func (export "give-str") (result i32)
            (i32.store (i32.const 64) (call $make))
            (i32.store (i32.const 68) (i32.const 0xFFFF0000))
            (i32.store (i32.const 72) (i32.const 16))
            (i32.const 64))
          (func (export "give-handle") (result i32)
            (i32.store (i32.const 64) (call $make))
            (i32.store (i32.const 68) (i32.const 99))
            (i32.const 64))
          (func (export "give-record") (result i32)
            (i32.store (i32.const 64) (call $make))
            (i32.store (i32.const 68) (i32.const 0xFFFF0000))
            (i32.store (i32.const 72) (i32.const 16))
            (i32.const 64))))")),
                                            { { "h", lowered } });
        for (auto const& signatures: { takes(b, scope), gives(b, scope) })
            for (auto const& [name, type]: signatures)
                std::ignore =
                    b.exportItem(name,
                                 Sort::Func,
                                 b.canonLift(b.aliasCoreExport(core, name, CoreSort::Func), options, type));
    }
    auto a = ComponentBuilder {};
    {
        auto const scope = declare(a);
        auto const make = importMake(a, scope);
        auto const memory = a.coreInstantiate(a.coreModule(coreModule(MemoryModule)), {});
        auto const options = memoryOptions(a, memory);
        auto exports = std::vector<CoreInlineExport> {
            CoreInlineExport { .name = "make", .sort = CoreSort::Func, .index = a.canonLower(make, {}) },
            CoreInlineExport { .name = "mem",
                               .sort = CoreSort::Memory,
                               .index = a.aliasCoreExport(memory, "mem", CoreSort::Memory) },
        };
        auto const importLowered = [&](std::string const& name, uint32_t type) {
            auto const imported = a.import(name, ExternDesc { .sort = Sort::Func, .index = type });
            exports.push_back(CoreInlineExport {
                .name = name, .sort = CoreSort::Func, .index = a.canonLower(imported, options) });
        };
        for (auto const* prefix: { "guest-", "host-" })
            for (auto const& [name, type]: takes(a, scope))
                importLowered(prefix + name, type);
        for (auto const& [name, type]: gives(a, scope))
            importLowered(name, type);
        auto const lowered = a.coreInlineExports(std::move(exports));
        auto const core = a.coreInstantiate(a.coreModule(coreModule(R"((module
          (import "h" "make" (func $make (result i32)))
          (import "h" "mem" (memory 1))
          (import "h" "guest-take-str" (func $guest-str (param i32 i32 i32)))
          (import "h" "guest-take2" (func $guest-handle (param i32 i32)))
          (import "h" "guest-take-rec" (func $guest-record (param i32 i32 i32)))
          (import "h" "host-take-str" (func $host-str (param i32 i32 i32)))
          (import "h" "host-take2" (func $host-handle (param i32 i32)))
          (import "h" "host-take-rec" (func $host-record (param i32 i32 i32)))
          (import "h" "give-str" (func $give-str (param i32)))
          (import "h" "give-handle" (func $give-handle (param i32)))
          (import "h" "give-record" (func $give-record (param i32)))
          (func (export "run-guest-str") (call $guest-str (call $make) (i32.const 0xFFFF0000) (i32.const 16)))
          (func (export "run-guest-handle") (call $guest-handle (call $make) (i32.const 99)))
          (func (export "run-guest-record") (call $guest-record (call $make) (i32.const 0xFFFF0000) (i32.const 16)))
          (func (export "run-host-str") (call $host-str (call $make) (i32.const 0xFFFF0000) (i32.const 16)))
          (func (export "run-host-handle") (call $host-handle (call $make) (i32.const 99)))
          (func (export "run-host-record") (call $host-record (call $make) (i32.const 0xFFFF0000) (i32.const 16)))
          (func (export "run-give-str") (call $give-str (i32.const 256)))
          (func (export "run-give-handle") (call $give-handle (i32.const 256)))
          (func (export "run-give-record") (call $give-record (i32.const 256)))))")),
                                            { { "h", lowered } });
        for (auto const name: LiftFailureExports)
            std::ignore =
                a.exportItem(std::string { name },
                             Sort::Func,
                             a.canonLift(a.aliasCoreExport(core, std::string { name }, CoreSort::Func),
                                         {},
                                         functionType(a, {}, std::nullopt)));
    }
    auto root = ComponentBuilder {};
    auto const scope = declare(root);
    auto const make = importMake(root, scope);
    auto hostFunctions = std::vector<std::pair<std::string, uint32_t>> {};
    for (auto const& [name, type]: takes(root, scope))
        hostFunctions.emplace_back(name, root.import(name, ExternDesc { .sort = Sort::Func, .index = type }));
    auto const bComponent = root.nested(std::move(b));
    auto const aComponent = root.nested(std::move(a));
    auto const shared = std::vector<std::pair<std::string, SortIndex>> {
        { "r", SortIndex { .sort = Sort::Type, .index = scope.resource } },
        { "rec", SortIndex { .sort = Sort::Type, .index = scope.record } },
        { "make", SortIndex { .sort = Sort::Func, .index = make } },
    };
    auto const bInstance = root.instantiate(bComponent, shared);
    auto arguments = shared;
    auto const fromB = [&](std::string const& name) {
        return SortIndex { .sort = Sort::Func, .index = root.aliasExport(bInstance, name, Sort::Func) };
    };
    for (auto const& [name, function]: hostFunctions)
    {
        arguments.emplace_back("guest-" + name, fromB(name));
        arguments.emplace_back("host-" + name, SortIndex { .sort = Sort::Func, .index = function });
    }
    for (auto const* name: { "give-str", "give-handle", "give-record" })
        arguments.emplace_back(name, fromB(name));
    auto const aInstance = root.instantiate(aComponent, arguments);
    for (auto const name: LiftFailureExports)
        std::ignore = root.exportItem(
            std::string { name }, Sort::Func, root.aliasExport(aInstance, std::string { name }, Sort::Func));
    return root.build();
}

} // namespace isoline::component::runtime::testing
