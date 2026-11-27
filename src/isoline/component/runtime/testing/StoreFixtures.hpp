// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the ComponentStore and Calls tests share: a store over a loaded component on any engine the
// build provides, core modules written as text, host functions written as lambdas, and a call of an
// export that hands back its lifted results. Uses Catch2's assertions outside every callback (a failing
// REQUIRE throws, and the store's calls are noexcept).

#include <core/Assert.hpp>

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
    auto bytes = isoline::text::compileText(source, isoline::text::TextLimits {});
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

} // namespace isoline::component::runtime::testing
