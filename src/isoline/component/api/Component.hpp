// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>
#include <variant>

#include <isoline/Features.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/EngineIdentity.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/runtime/ComponentPlan.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/ValidatedComponent.hpp>
#include <isoline/component/validate/Validator.hpp>

namespace isoline::component::api
{

/// What names one embedded core module for a cache (D10): the component's hash and the module's
/// ordinal, pre-order over nested components (binary::coreModulePositions()). Equal bytes give equal
/// keys, so a cache serves a module compiled once to every load of the component.
struct CoreModuleKey
{
    std::array<std::byte, 32> componentHash {}; ///< SHA-256 of the component's bytes.
    uint32_t ordinal = 0;                       ///< The module's ordinal.

    /// Two keys are equal when their hashes and ordinals are.
    bool operator==(CoreModuleKey const&) const = default;
};

/// Loads (and, for an engine that compiles, compiles) one embedded core module: the seam through which
/// a cache serves compiled modules (D10). Component::load() calls it only after the whole component
/// validated, once per module.
class ICoreModuleLoader
{
  public:
    virtual ~ICoreModuleLoader() = default;

    /// Loads one embedded core module. Contract: it throws nothing; the host running out of memory is the
    /// refusal HostMemoryExhausted, as isoline::api::Engine::load() reports it. The module must be of the
    /// engine Component::load() was given (its identity is checked), so a cache keys modules by the
    /// engine's identity as well as by @p key.
    /// @param key The component's hash and the module's ordinal.
    /// @param bytes The module's bytes, from its preamble on; already validated, decode-only.
    /// @return The module, or why it was refused.
    [[nodiscard]] virtual std::expected<isoline::api::Module, isoline::api::LoadError> load(
        CoreModuleKey key, std::span<std::byte const> bytes) noexcept = 0;

  protected:
    ICoreModuleLoader() = default;
    ICoreModuleLoader(ICoreModuleLoader const&) = default;
    ICoreModuleLoader& operator=(ICoreModuleLoader const&) = default;
    ICoreModuleLoader(ICoreModuleLoader&&) = default;
    ICoreModuleLoader& operator=(ICoreModuleLoader&&) = default;
};

/// The loader without a cache: every module through isoline::api::Engine::load(), the key unused.
class EngineCoreModuleLoader final: public ICoreModuleLoader
{
  public:
    /// @param engine The engine that loads the modules; outlives the loader.
    explicit EngineCoreModuleLoader(isoline::api::Engine const& engine) noexcept: _engine { &engine } {}

    [[nodiscard]] std::expected<isoline::api::Module, isoline::api::LoadError> load(
        CoreModuleKey key, std::span<std::byte const> bytes) noexcept override;

  private:
    isoline::api::Engine const* _engine;
};

/// The core module validator of an engine (R5): each embedded module decoded and validated with the
/// engine's features and load limits, as isoline::api::Engine::load() would, but never compiled (D10).
class EngineCoreModuleValidator final: public validate::ICoreModuleValidator
{
  public:
    /// @param engine The engine whose features and load limits apply; outlives the validator.
    explicit EngineCoreModuleValidator(isoline::api::Engine const& engine) noexcept: _engine { &engine } {}

    [[nodiscard]] std::expected<validate::CoreModuleShape, isoline::api::LoadError> validate(
        std::span<std::byte const> bytes) override;

    /// The engine's features, under which declared core types are validated too.
    [[nodiscard]] FeatureSet coreFeatures() const noexcept override;

  private:
    isoline::api::Engine const* _engine;
};

/// What a Component is loaded with: the gated features admitted and the caps enforced while decoding
/// and validating it.
struct ComponentConfig
{
    ComponentFeatureSet features = ComponentFeatureSet::wasi03(); ///< The gated features admitted.
    ComponentLimits limits {};                                    ///< The caps.
};

/// An embedded core module the ICoreModuleLoader refused, after the component validated.
struct CoreModuleLoadError
{
    uint32_t ordinal = 0;          ///< The module's ordinal.
    isoline::api::LoadError error; ///< The loader's refusal.
};

/// A core module the ICoreModuleLoader loaded for another engine than the component's: a store of the
/// component's engine could not instantiate it.
struct CoreModuleEngineMismatch
{
    uint32_t ordinal = 0; ///< The module's ordinal.
};

/// An ICoreModuleValidator whose core features are not the engine's: the component would be validated
/// under other features than its modules run with. Refused before anything is decoded.
struct CoreFeaturesMismatch
{
    FeatureSet validator; ///< ICoreModuleValidator::coreFeatures().
    FeatureSet engine;    ///< The engine's EngineConfig::features.
};

/// Why Component::load() refused a component: malformed, invalid, a core module its loader refused or
/// made for another engine, a validator of other features than the engine's, or the host out of memory
/// (caught at this boundary, as isoline::api::Engine::load() does).
using ComponentLoadError = std::variant<binary::DecodeError,
                                        validate::ValidationError,
                                        CoreModuleLoadError,
                                        CoreModuleEngineMismatch,
                                        CoreFeaturesMismatch,
                                        isoline::api::HostMemoryExhausted>;

/// An import or an export of a component: its name and its type in the component's arena (types()).
using ExternDescription = types::NamedExtern;

/// A loaded component: decoded, validated, every embedded core module loaded once, and one plan per
/// component definition (runtime::buildPlans()).
///
/// Immutable and cheap to copy: copies share one state, which any number of threads may read at once.
/// What its accessors return stays valid while any copy lives.
class Component
{
  public:
    /// A copy, sharing this component's state.
    ///
    /// There is deliberately no move constructor or move assignment, as api::Module has none: a moved-from
    /// component would hold no state, and every member reads it. Moving copies instead, one atomic
    /// increment, leaving the source whole.
    Component(Component const&) = default;

    /// Makes this component a copy of @p other, sharing its state; moving assigns the same way.
    /// @return This component.
    Component& operator=(Component const&) = default;

    ~Component() = default;

    /// Loads a component: refuses a @p validator of other core features than @p engine's; decodes it,
    /// validates it (each embedded core module decode-only, through @p validator), and only then loads
    /// every core module through @p loader, in ordinal order, keyed by the component's hash, refusing
    /// one of another engine identity; then plans it. Nothing is thrown: the host running out of memory is a
    /// refusal.
    /// @param engine The engine its core modules are for; the component records its identity.
    /// @param bytes The component's bytes, from its preamble on; copied, so the caller may free them.
    /// @param config The gated features and caps.
    /// @param loader Loads each core module once the component validated.
    /// @param validator Validates each core module without compiling it.
    /// @return The component, or the first refusal.
    [[nodiscard]] static std::expected<Component, ComponentLoadError> load(
        isoline::api::Engine const& engine,
        std::span<std::byte const> bytes,
        ComponentConfig const& config,
        ICoreModuleLoader& loader,
        validate::ICoreModuleValidator& validator) noexcept;

    /// The root component's imports, in definition order, typed in types().
    [[nodiscard]] std::span<ExternDescription const> imports() const& noexcept;
    /// Deleted: the imports of a temporary component would dangle at the end of the expression.
    [[nodiscard]] std::span<ExternDescription const> imports() const&& = delete;

    /// The root component's exports, in definition order, typed in types().
    [[nodiscard]] std::span<ExternDescription const> exports() const& noexcept;
    /// Deleted: as imports().
    [[nodiscard]] std::span<ExternDescription const> exports() const&& = delete;

    /// SHA-256 of the component's bytes: equal for equal bytes, the first half of every CoreModuleKey.
    [[nodiscard]] std::array<std::byte, 32> const& hash() const& noexcept;
    /// Deleted: as imports().
    [[nodiscard]] std::array<std::byte, 32> const& hash() const&& = delete;

    /// The identity of the engine it was loaded for, which a store instantiating it must share.
    [[nodiscard]] isoline::api::EngineIdentity const& engine() const& noexcept;
    /// Deleted: as imports().
    [[nodiscard]] isoline::api::EngineIdentity const& engine() const&& = delete;

    /// Every type of the component: what imports() and exports() are typed in.
    [[nodiscard]] types::TypeArena const& types() const& noexcept;
    /// Deleted: as imports().
    [[nodiscard]] types::TypeArena const& types() const&& = delete;

    /// The validated component its plans read.
    [[nodiscard]] validate::ValidatedComponent const& validated() const& noexcept;
    /// Deleted: as imports().
    [[nodiscard]] validate::ValidatedComponent const& validated() const&& = delete;

    /// Its embedded core modules, by ordinal, each loaded once.
    [[nodiscard]] std::span<isoline::api::Module const> coreModules() const& noexcept;
    /// Deleted: as imports().
    [[nodiscard]] std::span<isoline::api::Module const> coreModules() const&& = delete;

    /// Its plans, one per entry of the pool, shared by every copy.
    [[nodiscard]] runtime::ComponentPlans const& plans() const& noexcept;
    /// Deleted: as imports().
    [[nodiscard]] runtime::ComponentPlans const& plans() const&& = delete;

  private:
    struct State;

    explicit Component(std::shared_ptr<State const> state) noexcept: _state { std::move(state) } {}

    std::shared_ptr<State const> _state;
};

} // namespace isoline::component::api
