// SPDX-License-Identifier: Apache-2.0
#include <isoline/api/Engine.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

#include <isoline/fastinterpreter/Lowering.hpp>
#include <isoline/meta/Check.hpp>
#include <isoline/runtime/CountingArena.hpp>
#include <isoline/runtime/ModuleCode.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::api
{

namespace
{
    /// How Engine::load() reports a module the fast engine refused to lower: the host out of memory as
    /// such, which is not the module's to answer for, and every other refusal as the limit it met.
    [[nodiscard]] LoadError loadErrorOf(fastinterpreter::LoweringError const& error) noexcept
    {
        if (error.reason == fastinterpreter::LoweringFailure::HostMemory)
            return LoadError { HostMemoryExhausted {} };
        return LoadError { CompileLimitExceeded {
            .codeIndex = error.codeIndex, .requested = error.requested, .limit = error.limit } };
    }

    /// What @p config's engine makes of @p validated to run it: a case per ExecutionEngine and no default,
    /// so that -Wswitch names this place when an engine is added.
    /// @param codeMemory What the fast engine's code is allocated from, through the compile budget.
    /// @return The fast engine's lowered module, within the compile budget; null for the reference
    ///         engine; or why the module was refused.
    [[nodiscard]] std::expected<std::shared_ptr<runtime::ModuleCode const>, LoadError> codeOf(
        EngineConfig const& config,
        validate::ValidatedModule const& validated,
        std::pmr::memory_resource& codeMemory)
    {
        switch (config.engine)
        {
            case ExecutionEngine::Reference: return nullptr;
            case ExecutionEngine::Fast: break;
        }
        auto arena = std::make_shared<runtime::CountingArena>(config.load.maxCompileBytes, codeMemory);
        return fastinterpreter::lower(
                   validated,
                   fastinterpreter::LoweringConfig { .mode = config.mode, .dispatch = config.dispatch },
                   std::move(arena))
            .transform([](std::shared_ptr<fastinterpreter::LoweredModule const> lowered) {
                return std::shared_ptr<runtime::ModuleCode const> { std::move(lowered) };
            })
            .transform_error(&loadErrorOf);
    }
} // namespace

EngineIdentity EngineConfig::identity() const noexcept
{
    auto const lowered = engine == ExecutionEngine::Fast;
    return EngineIdentity { .engine = engine,
                            .mode = mode,
                            .features = features,
                            .dispatch = lowered ? std::optional { dispatch } : std::nullopt };
}

Engine::Engine(EngineConfig config) noexcept: Engine { config, *std::pmr::new_delete_resource() }
{
}

Engine::Engine(EngineConfig config, std::pmr::memory_resource& codeMemory) noexcept:
    _config { config }, _codeMemory { &codeMemory }
{
    ISOLINE_PRECONDITION(_config.features.checkPrerequisites().has_value());
    // The fast engine lowers for its dispatch and runs with it, which this build must have compiled.
    ISOLINE_PRECONDITION(_config.engine != ExecutionEngine::Fast
                         || std::ranges::find(availableDispatches(), _config.dispatch)
                                != availableDispatches().end());
}

ThreadPreparation::ThreadPreparation(std::unique_ptr<IThreadResource> resource) noexcept:
    _resource { std::move(resource) }
{
    ISOLINE_PRECONDITION(_resource != nullptr);
}

std::expected<Module, LoadError> Engine::load(std::span<std::byte const> bytes) const noexcept
{
    if (bytes.size() > _config.load.maxModuleBytes)
        return std::unexpected { LoadError {
            ModuleTooLarge { .bytes = bytes.size(), .limit = _config.load.maxModuleBytes } } };

    // The API boundary: decoding, validating, lowering and describing allocate, and the host running out
    // of memory is a refusal, never an exception in the embedder's lap.
    try
    {
        return validate::ValidatedModule::load(bytes, _config.features, _config.load.decode)
            .transform_error([](validate::ModuleError error) {
                return std::visit([](auto alternative) { return LoadError { std::move(alternative) }; },
                                  std::move(error));
            })
            .and_then([this](std::shared_ptr<validate::ValidatedModule const> validated) {
                return codeOf(_config, *validated, *_codeMemory)
                    .transform([&](std::shared_ptr<runtime::ModuleCode const> code) {
                        return Module {
                            Module::Key {}, std::move(validated), _config.identity(), std::move(code)
                        };
                    });
            });
    }
    catch (std::bad_alloc const&)
    {
        return std::unexpected { LoadError { HostMemoryExhausted {} } };
    }
    catch (std::length_error const&)
    {
        // A container asked for more than its max_size(): a memory refusal, as std::bad_alloc is.
        return std::unexpected { LoadError { HostMemoryExhausted {} } };
    }
}

std::expected<ThreadPreparation, PrepareError> Engine::prepareThread() const noexcept
{
    return ThreadPreparation {};
}

uint64_t Engine::requiredGuardBytes() const noexcept
{
    return 0;
}

} // namespace isoline::api
