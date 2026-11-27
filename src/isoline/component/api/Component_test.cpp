// SPDX-License-Identifier: Apache-2.0
#include <core/Sha256.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <future>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Engine.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/api/Component.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/testing/Builder.hpp>
#include <isoline/component/binary/testing/ComponentFixtures.hpp>
#include <isoline/component/binary/testing/DecoderFixtures.hpp>
#include <isoline/component/runtime/ComponentPlan.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/Validator.hpp>
#include <isoline/component/validate/testing/CoreModuleValidator.hpp>

using isoline::component::ComponentLimit;
using isoline::component::api::Component;
using isoline::component::api::ComponentConfig;
using isoline::component::api::ComponentLoadError;
using isoline::component::api::CoreModuleKey;
using isoline::component::api::CoreModuleLoadError;
using isoline::component::api::EngineCoreModuleLoader;
using isoline::component::api::EngineCoreModuleValidator;
using isoline::component::api::ICoreModuleLoader;
using isoline::component::binary::DecodeError;
using isoline::component::binary::testing::Bytes;
using isoline::component::binary::testing::ComponentBuilder;
using isoline::component::binary::testing::doublingComponent;
using isoline::component::binary::testing::identityModule;
using isoline::component::binary::testing::liftedIdentity;
using isoline::component::binary::testing::modulePreamble;
using isoline::component::binary::testing::taggedModule;
using isoline::component::validate::ValidationError;
using isoline::component::validate::ValidationErrorCode;
using isoline::component::validate::testing::CoreModuleValidator;

namespace
{

/// How long a thread of a test may take before it is called stuck.
constexpr auto ThreadDeadline = std::chrono::seconds { 60 };

/// A loader that records every call, loads through an engine, and refuses (or throws) where told.
class RecordingLoader final: public ICoreModuleLoader
{
  public:
    explicit RecordingLoader(isoline::api::Engine const& engine) noexcept: _engine { &engine } {}

    [[nodiscard]] std::expected<isoline::api::Module, isoline::api::LoadError> load(
        CoreModuleKey key, std::span<std::byte const> bytes) override
    {
        keys.push_back(key);
        modules.emplace_back(bytes.begin(), bytes.end());
        if (key.ordinal == throwAt)
            throw std::bad_alloc {};
        if (key.ordinal == refuseAt)
            return std::unexpected { isoline::api::LoadError { isoline::api::HostMemoryExhausted {} } };
        return _engine->load(bytes);
    }

    std::vector<CoreModuleKey> keys {};  ///< Each call's key, in order.
    std::vector<Bytes> modules {};       ///< Each call's module bytes, in order.
    std::optional<uint32_t> refuseAt {}; ///< The ordinal refused, if any.
    std::optional<uint32_t> throwAt {};  ///< The ordinal whose load throws std::bad_alloc, if any.

  private:
    isoline::api::Engine const* _engine;
};

/// What a load needs: an engine, a recording loader and a counting validator.
struct Loading
{
    isoline::api::Engine engine { isoline::api::EngineConfig {} };
    RecordingLoader loader { engine };
    CoreModuleValidator validator {};

    /// Loads @p bytes under @p config.
    [[nodiscard]] std::expected<Component, ComponentLoadError> load(std::span<std::byte const> bytes,
                                                                    ComponentConfig const& config = {})
    {
        return Component::load(engine, bytes, config, loader, validator);
    }
};

/// The error @p loaded failed with, as @p Error (REQUIREd).
template <typename Error>
[[nodiscard]] Error errorOf(std::expected<Component, ComponentLoadError> const& loaded)
{
    REQUIRE_FALSE(loaded.has_value());
    auto const* error = std::get_if<Error>(&loaded.error());
    REQUIRE(error != nullptr);
    return *error;
}

/// @p count distinct core modules (taggedModule()), each embedded in the root component.
[[nodiscard]] ComponentBuilder modulesComponent(uint32_t count)
{
    auto builder = ComponentBuilder {};
    for (auto const index: std::views::iota(uint32_t { 0 }, count))
        std::ignore = builder.coreModule(taggedModule(std::format("m{}", index)));
    return builder;
}

} // namespace

TEST_CASE("Component.loadsBuilderComponent")
{
    auto loading = Loading {};
    auto const bytes = liftedIdentity().build();
    auto const component = loading.load(bytes);
    REQUIRE(component.has_value());
    CHECK(component->imports().empty());
    REQUIRE(component->exports().size() == 1);
    auto const& exported = component->exports()[0];
    CHECK(exported.name.str() == "id");
    auto const* function = std::get_if<isoline::component::types::FunctionTypeId>(&exported.type);
    REQUIRE(function != nullptr);
    auto const& type = component->types()[*function];
    REQUIRE(type.params.size() == 1);
    CHECK(component->types()[type.params[0].second].kind == isoline::component::types::ValueKind::U32);
    REQUIRE(type.result.has_value());
    CHECK(component->types()[*type.result].kind == isoline::component::types::ValueKind::U32);
    CHECK(component->engine() == loading.engine.config().identity());
    CHECK(component->hash() == core::Sha256::hash(bytes));
    REQUIRE(component->coreModules().size() == 1);
    CHECK(component->plans().plans.size() == 1);
}

TEST_CASE("Component.loaderSeesEveryCoreModuleOnce")
{
    // Two modules, the first instantiated three times: two loads, by ordinal, keyed by the hash.
    auto builder = ComponentBuilder {};
    auto const first = builder.coreModule(identityModule());
    std::ignore = builder.coreModule(taggedModule("second"));
    for ([[maybe_unused]] auto const time: std::views::iota(0, 3))
        std::ignore = builder.coreInstantiate(first, {});
    auto const bytes = builder.build();
    auto loading = Loading {};
    auto const component = loading.load(bytes);
    REQUIRE(component.has_value());
    auto const hash = core::Sha256::hash(bytes);
    CHECK(loading.loader.keys
          == std::vector { CoreModuleKey { .componentHash = hash, .ordinal = 0 },
                           CoreModuleKey { .componentHash = hash, .ordinal = 1 } });
    CHECK(loading.loader.modules == std::vector { identityModule(), taggedModule("second") });
    CHECK(loading.validator.calls() == 2);
    CHECK(component->coreModules().size() == 2);
}

TEST_CASE("Component.ordinalsArePreOrder")
{
    // A, a nested component holding B, then C: the loader sees A, B, C as ordinals 0, 1, 2.
    auto builder = ComponentBuilder {};
    std::ignore = builder.coreModule(taggedModule("A"));
    auto child = ComponentBuilder {};
    std::ignore = child.coreModule(taggedModule("B"));
    std::ignore = builder.nested(std::move(child));
    std::ignore = builder.coreModule(taggedModule("C"));
    auto loading = Loading {};
    REQUIRE(loading.load(builder.build()).has_value());
    CHECK(loading.loader.modules == std::vector { taggedModule("A"), taggedModule("B"), taggedModule("C") });
}

TEST_CASE("Component.noCompileBeforeValidation")
{
    // Valid modules, then a last definition that instantiates a module that does not exist: the
    // component is refused by validation, and no module was loaded.
    auto builder = modulesComponent(2);
    std::ignore = builder.coreInstantiate(7, {});
    auto loading = Loading {};
    auto const error = errorOf<ValidationError>(loading.load(builder.build()));
    CHECK(error.code == ValidationErrorCode::ModuleIndexOutOfBounds);
    CHECK(loading.validator.calls() == 2);
    CHECK(loading.loader.keys.empty());
}

TEST_CASE("Component.coreModuleErrorIndexed")
{
    auto loading = Loading {};
    loading.loader.refuseAt = 1;
    auto const error = errorOf<CoreModuleLoadError>(loading.load(modulesComponent(3).build()));
    CHECK(error.ordinal == 1);
    CHECK(std::holds_alternative<isoline::api::HostMemoryExhausted>(error.error));
    // The load stops at the refusal.
    CHECK(loading.loader.keys.size() == 2);
}

TEST_CASE("Component.loaderRefusalStopsALargeLoadPartway")
{
    // 200 modules, the loader (a cache giving up, or an embedder cancelling) refusing the 50th: the
    // modules loaded so far are released, and no later one is asked for.
    auto loading = Loading {};
    loading.loader.refuseAt = 49;
    auto const error = errorOf<CoreModuleLoadError>(loading.load(modulesComponent(200).build()));
    CHECK(error.ordinal == 49);
    CHECK(loading.loader.keys.size() == 50);
    CHECK(loading.validator.calls() == 200);
}

TEST_CASE("Component.hostMemoryExhaustedIsARefusal")
{
    // The boundary catches std::bad_alloc wherever it is thrown, the loader included.
    auto loading = Loading {};
    loading.loader.throwAt = 1;
    auto const loaded = loading.load(modulesComponent(3).build());
    std::ignore = errorOf<isoline::api::HostMemoryExhausted>(loaded);
}

TEST_CASE("Component.sameBytesSameIdentity")
{
    auto loading = Loading {};
    auto const bytes = liftedIdentity().build();
    auto const first = loading.load(bytes);
    auto const second = loading.load(bytes);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->hash() == second->hash());
    REQUIRE(loading.loader.keys.size() == 2);
    CHECK(loading.loader.keys[0] == loading.loader.keys[1]);
    // One byte more (a custom section) is another component.
    auto other = liftedIdentity();
    other.custom("note", std::span<std::byte const> {});
    auto const third = loading.load(other.build());
    REQUIRE(third.has_value());
    CHECK(third->hash() != first->hash());
}

TEST_CASE("Component.copiesShareOneState")
{
    auto loading = Loading {};
    auto const loaded = loading.load(liftedIdentity().build());
    REQUIRE(loaded.has_value());
    auto const copy = [&] {
        return *loaded;
    }();
    CHECK(&copy.plans() == &loaded->plans());
    CHECK(copy.coreModules().data() == loaded->coreModules().data());
    CHECK(&copy.validated() == &loaded->validated());
    CHECK(loading.loader.keys.size() == 1);
}

TEST_CASE("Component.limitsChargedAtLoad")
{
    SECTION("the component's bytes: refused before anything is validated")
    {
        auto loading = Loading {};
        auto const bytes = liftedIdentity().build();
        auto config = ComponentConfig {};
        config.limits.maxComponentBytes = bytes.size() - 1;
        auto const error = errorOf<DecodeError>(loading.load(bytes, config));
        CHECK(error.code == isoline::component::binary::DecodeErrorCode::LimitExceeded);
        CHECK(loading.validator.calls() == 0);
        CHECK(loading.loader.keys.empty());
    }
    SECTION("a nested component's modules: refused at validation, nothing loaded")
    {
        auto child = modulesComponent(3);
        auto builder = modulesComponent(1);
        std::ignore = builder.nested(std::move(child));
        auto loading = Loading {};
        auto config = ComponentConfig {};
        config.limits.maxModules = 2;
        auto const error = errorOf<ValidationError>(loading.load(builder.build(), config));
        CHECK(isoline::component::validate::exceededLimit(error) == ComponentLimit::Modules);
        CHECK(loading.loader.keys.empty());
    }
}

TEST_CASE("Component.malformedNestedModuleIsNamed")
{
    // A nested component's module, its preamble whole but its body cut short: refused by validation
    // with the module's ordinal and the core decoder's error, never an abort, nothing loaded.
    auto truncated = modulePreamble();
    truncated.push_back(std::byte { 0x01 }); // a type section, its size missing
    auto child = ComponentBuilder {};
    std::ignore = child.coreModule(std::move(truncated));
    auto builder = modulesComponent(1);
    std::ignore = builder.nested(std::move(child));
    auto loading = Loading {};
    auto const error = errorOf<ValidationError>(loading.load(builder.build()));
    CHECK(error.code == ValidationErrorCode::CoreModuleRefused);
    REQUIRE(error.coreModule.has_value());
    CHECK(error.coreModule->ordinal == 1);
    CHECK(std::holds_alternative<isoline::api::DecodeError>(error.coreModule->error));
    CHECK(loading.loader.keys.empty());
}

TEST_CASE("Component.engineLoaderAndValidator")
{
    // The production pair: the engine's limits refuse an oversized module at validation, by ordinal;
    // otherwise every module loads through the engine, for the component's engine.
    auto config = isoline::api::EngineConfig {};
    config.load.maxModuleBytes = taggedModule("m0").size();
    auto const engine = isoline::api::Engine { config };
    auto loader = EngineCoreModuleLoader { engine };
    auto validator = EngineCoreModuleValidator { engine };

    auto const loaded = Component::load(engine, modulesComponent(2).build(), {}, loader, validator);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->coreModules().size() == 2);
    for (auto const& module: loaded->coreModules())
        CHECK(module.engine() == loaded->engine());

    auto builder = modulesComponent(2);
    std::ignore = builder.coreModule(taggedModule("a-longer-tag"));
    auto const refused = Component::load(engine, builder.build(), {}, loader, validator);
    REQUIRE_FALSE(refused.has_value());
    auto const* error = std::get_if<ValidationError>(&refused.error());
    REQUIRE(error != nullptr);
    CHECK(error->code == ValidationErrorCode::CoreModuleRefused);
    REQUIRE(error->coreModule.has_value());
    CHECK(error->coreModule->ordinal == 2);
    CHECK(std::holds_alternative<isoline::api::ModuleTooLarge>(error->coreModule->error));
}

TEST_CASE("ComponentPlan.sharedAcrossThreads", "[threading]")
{
    // One Component, eight threads each copying it and reading its plans (the stores that run them come
    // with Task 15): every thread sees the one state, unchanged.
    auto loading = Loading {};
    auto const loaded = loading.load(doublingComponent(20).build());
    REQUIRE(loaded.has_value());
    auto const& component = *loaded;
    auto const* const shared = &component.plans();
    auto tasks = std::vector<std::future<std::pair<void const*, std::size_t>>> {};
    for ([[maybe_unused]] auto const thread: std::views::iota(0, 8))
        tasks.push_back(std::async(std::launch::async, [component] {
            auto steps = std::size_t { 0 };
            for (auto const& plan: component.plans().plans)
                steps += plan.steps.size();
            return std::pair<void const*, std::size_t> { &component.plans(), steps };
        }));
    for (auto& task: tasks)
    {
        REQUIRE(task.wait_for(ThreadDeadline) == std::future_status::ready);
        auto const [plans, steps] = task.get();
        CHECK(plans == shared);
        CHECK(steps == std::size_t { 3 } * 19);
    }
}
