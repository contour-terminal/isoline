// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the Linker and api ComponentStore tests share: an engine with its loader and validator, components
// loaded on it, host functions written as lambdas, and the function types a linker defines. Uses Catch2's
// assertions outside every callback (a failing REQUIRE throws, and host functions are noexcept).

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <isoline/api/Engine.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/component/api/Component.hpp>
#include <isoline/component/api/ComponentStore.hpp>
#include <isoline/component/api/Linker.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/Validator.hpp>

namespace isoline::component::api::testing
{

/// An engine, its core module loader and validator, and an allocator for its stores.
class ApiHarness
{
  public:
    /// @param engine The execution engine.
    explicit ApiHarness(isoline::api::ExecutionEngine engine = isoline::api::ExecutionEngine::Reference):
        engine { isoline::api::EngineConfig { .engine = engine } },
        loader { this->engine },
        validator { this->engine }
    {
    }

    /// Loads @p bytes, REQUIREd to load.
    /// @param bytes The component.
    /// @return It.
    [[nodiscard]] Component load(std::span<std::byte const> bytes)
    {
        auto loaded = Component::load(engine, bytes, ComponentConfig {}, loader, validator);
        if (!loaded)
        {
            if (auto const* invalid = std::get_if<validate::ValidationError>(&loaded.error()))
                FAIL("invalid: " << validate::specMessage(*invalid));
            FAIL("refused at load");
        }
        return *loaded;
    }

    /// An empty store of the engine.
    /// @param limits Its caps.
    /// @return It.
    [[nodiscard]] ComponentStore store(ComponentLimits const& limits = {})
    {
        return ComponentStore { engine, allocator, ComponentConfig { .limits = limits } };
    }

    isoline::api::Engine engine;
    EngineCoreModuleLoader loader;
    EngineCoreModuleValidator validator;
    isoline::api::HeapMemoryAllocator allocator {};
};

/// A host function from a callable of IHostComponentFunction::call()'s signature.
template <typename F>
class LambdaFunction final: public IHostComponentFunction
{
  public:
    explicit LambdaFunction(F body): _body { std::move(body) } {}

    [[nodiscard]] std::expected<void, isoline::api::HostFailure> call(
        HostComponentContext& context,
        canonical::Values const& args,
        canonical::ValueBuilder& results) noexcept override
    {
        ++calls;
        return _body(context, args, results);
    }

    uint32_t calls = 0; ///< How often it ran.

  private:
    F _body;
};

/// A host function running @p body.
template <typename F>
[[nodiscard]] std::shared_ptr<LambdaFunction<F>> hostFunction(F body)
{
    return std::make_shared<LambdaFunction<F>>(std::move(body));
}

/// A host function of no parameters returning the u32 @p value.
[[nodiscard]] inline auto constantFunction(uint32_t value)
{
    return hostFunction(
        [value](HostComponentContext&,
                canonical::Values const&,
                canonical::ValueBuilder& results) -> std::expected<void, isoline::api::HostFailure> {
            results.scalar(value);
            return {};
        });
}

/// The primitive @p kind in @p linker's arena.
[[nodiscard]] inline types::TypeId primitive(Linker& linker, types::ValueKind kind)
{
    return linker.types().primitive(kind);
}

/// The type `func(p0, p1, …) -> result?` in @p linker's arena, its parameters labelled as
/// runtime::testing::functionType() labels them.
/// @param params The parameters' types.
/// @param result The result's type, if any.
/// @return It.
[[nodiscard]] inline FunctionTypeSpec functionSpec(std::vector<types::TypeId> const& params,
                                                   std::optional<types::TypeId> result)
{
    auto spec = FunctionTypeSpec { .params = {}, .result = result };
    for (auto const param: params)
        spec.params.emplace_back("p" + std::to_string(spec.params.size()), param);
    return spec;
}

/// The value of a defining call that must succeed (REQUIREd).
template <typename T>
void require(std::expected<T, LinkerError> const& defined)
{
    if (!defined)
        FAIL(defined.error().message());
}

/// The u32 result of a call (REQUIREd to have succeeded with one).
[[nodiscard]] inline uint32_t u32Of(std::expected<canonical::Values, ComponentCallError> const& called)
{
    if (!called)
        FAIL(called.error().message());
    REQUIRE(called->rootCount() == 1);
    return called->root(0).scalar<uint32_t>();
}

/// Calls @p name of @p instance with the u32 arguments @p args.
[[nodiscard]] inline std::expected<canonical::Values, ComponentCallError> callWith(
    ComponentInstance& instance, std::string_view name, std::vector<uint32_t> const& args = {})
{
    auto const function = instance.function(name);
    REQUIRE(function.has_value());
    auto builder = instance.argumentsFor(*function);
    for (auto const arg: args)
        builder.scalar(arg);
    auto built = builder.finish();
    REQUIRE(built.has_value());
    return instance.call(*function, *built);
}

/// The refusal of an instantiation, of alternative @p T (REQUIREd).
template <typename T>
[[nodiscard]] T refusalAs(std::expected<ComponentInstance, InstantiateComponentError> const& instantiated)
{
    REQUIRE_FALSE(instantiated.has_value());
    auto const* refusal = std::get_if<T>(&instantiated.error());
    REQUIRE(refusal != nullptr);
    return *refusal;
}

/// The instance an instantiation made (REQUIREd).
[[nodiscard]] inline ComponentInstance instanceOf(
    std::expected<ComponentInstance, InstantiateComponentError> instantiated)
{
    REQUIRE(instantiated.has_value());
    return std::move(*instantiated);
}

} // namespace isoline::component::api::testing
