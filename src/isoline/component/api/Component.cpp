// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/api/Component.hpp>

#include <core/Assert.hpp>
#include <core/Sha256.hpp>
#include <core/Utils.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/meta/Check.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::component::api
{

/// What every copy of a Component shares.
struct Component::State
{
    std::shared_ptr<validate::ValidatedComponent const> validated; ///< The component, validated.
    std::vector<isoline::api::Module> coreModules;                 ///< Its core modules, by ordinal.
    runtime::ComponentPlans plans;                                 ///< One plan per pool entry.
    std::array<std::byte, 32> hash {};                             ///< SHA-256 of its bytes.
    isoline::api::EngineIdentity engine;                           ///< The engine it was loaded for.
};

std::expected<isoline::api::Module, isoline::api::LoadError> EngineCoreModuleLoader::load(
    CoreModuleKey, std::span<std::byte const> bytes) noexcept
{
    return _engine->load(bytes);
}

std::expected<validate::CoreModuleShape, isoline::api::LoadError> EngineCoreModuleValidator::validate(
    std::span<std::byte const> bytes)
{
    // What isoline::api::Engine::load() checks, short of making a Module: the size first, then decoding
    // and validation under the engine's features and decode limits.
    auto const& config = _engine->config();
    if (bytes.size() > config.load.maxModuleBytes)
        return std::unexpected { isoline::api::LoadError {
            isoline::api::ModuleTooLarge { .bytes = bytes.size(), .limit = config.load.maxModuleBytes } } };
    return validate::loadCoreModule(bytes, config.features, config.load.decode);
}

FeatureSet EngineCoreModuleValidator::coreFeatures() const noexcept
{
    return _engine->config().features;
}

namespace
{

    /// Loads every embedded core module of @p validated through @p loader, by ordinal, stopping at the
    /// first refusal.
    [[nodiscard]] std::expected<std::vector<isoline::api::Module>, ComponentLoadError> loadCoreModules(
        validate::ValidatedComponent const& validated,
        std::array<std::byte, 32> const& hash,
        isoline::api::EngineIdentity const& engine,
        ICoreModuleLoader& loader)
    {
        auto const& pool = validated.pool;
        auto const positions = binary::coreModulePositions(pool);
        Require(positions.size() == validated.coreModules.size());
        auto const bytes = std::span<std::byte const> { *pool.bytes };
        auto modules = std::vector<isoline::api::Module> {};
        modules.reserve(positions.size());
        for (auto const [ordinal, position]: core::views::enumerate(positions))
        {
            auto const& definition = std::get<binary::CoreModuleDefinition>(
                pool.components[position.scope].definitions[position.definition]);
            auto const key =
                CoreModuleKey { .componentHash = hash, .ordinal = static_cast<uint32_t>(ordinal) };
            auto loaded = loader.load(key, bytes.subspan(definition.offset, definition.size));
            if (!loaded)
                return std::unexpected { ComponentLoadError {
                    CoreModuleLoadError { .ordinal = key.ordinal, .error = std::move(loaded.error()) } } };
            // A cache may hand back a module of another engine, which no store of this one instantiates.
            if (loaded->engine() != engine)
                return std::unexpected { ComponentLoadError {
                    CoreModuleEngineMismatch { .ordinal = key.ordinal } } };
            modules.push_back(*loaded);
        }
        return modules;
    }

} // namespace

std::expected<Component, ComponentLoadError> Component::load(
    isoline::api::Engine const& engine,
    std::span<std::byte const> bytes,
    ComponentConfig const& config,
    ICoreModuleLoader& loader,
    validate::ICoreModuleValidator& validator) noexcept
{
    // The API boundary: decoding, validating, loading and planning allocate, and the host running out
    // of memory is a refusal, never an exception in the embedder's lap.
    try
    {
        // Declared core types are validated under the validator's features, embedded modules run under the
        // engine's: the two must agree, or validation would admit what the engine refuses.
        if (auto const features = engine.config().features; validator.coreFeatures() != features)
            return std::unexpected { ComponentLoadError {
                CoreFeaturesMismatch { .validator = validator.coreFeatures(), .engine = features } } };
        auto pool = binary::decodeComponent(bytes, config.features, config.limits);
        if (!pool)
            return std::unexpected { ComponentLoadError { pool.error() } };
        // Validation first, whole: no core module is loaded (compiled) for a component that is invalid
        // anywhere (D10).
        auto validated =
            validate::validateComponent(std::move(*pool), config.features, config.limits, validator);
        if (!validated)
            return std::unexpected { ComponentLoadError { std::move(validated.error()) } };
        auto state = State { .validated = std::move(*validated),
                             .coreModules = {},
                             .plans = {},
                             .hash = core::Sha256::hash(bytes),
                             .engine = engine.config().identity() };
        auto modules = loadCoreModules(*state.validated, state.hash, state.engine, loader);
        if (!modules)
            return std::unexpected { std::move(modules.error()) };
        state.coreModules = std::move(*modules);
        state.plans = runtime::buildPlans(*state.validated);
        return Component { std::make_shared<State const>(std::move(state)) };
    }
    catch (std::bad_alloc const&)
    {
        return std::unexpected { ComponentLoadError { isoline::api::HostMemoryExhausted {} } };
    }
    catch (std::length_error const&)
    {
        // A container asked for more than its max_size(): a memory refusal, as std::bad_alloc is.
        return std::unexpected { ComponentLoadError { isoline::api::HostMemoryExhausted {} } };
    }
}

std::span<ExternDescription const> Component::imports() const& noexcept
{
    return _state->validated->type.imports;
}

std::span<ExternDescription const> Component::exports() const& noexcept
{
    return _state->validated->type.exports;
}

std::array<std::byte, 32> const& Component::hash() const& noexcept
{
    return _state->hash;
}

isoline::api::EngineIdentity const& Component::engine() const& noexcept
{
    return _state->engine;
}

types::TypeArena const& Component::types() const& noexcept
{
    return _state->validated->types;
}

validate::ValidatedComponent const& Component::validated() const& noexcept
{
    return *_state->validated;
}

std::span<isoline::api::Module const> Component::coreModules() const& noexcept
{
    return _state->coreModules;
}

runtime::ComponentPlans const& Component::plans() const& noexcept
{
    return _state->plans;
}

runtime::ComponentCode Component::code() const
{
    return runtime::ComponentCode { .owner = _state,
                                    .validated = _state->validated.get(),
                                    .plans = &_state->plans,
                                    .modules = _state->coreModules,
                                    .engine = _state->engine };
}

} // namespace isoline::component::api
