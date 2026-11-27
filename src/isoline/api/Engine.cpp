// SPDX-License-Identifier: Apache-2.0
#include <isoline/api/Engine.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
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

#include <isoline/api/ExecutionEngines.hpp>
#include <isoline/fastinterpreter/Lowering.hpp>
#include <isoline/runtime/CountingArena.hpp>
#include <isoline/runtime/ModuleCode.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::api
{

namespace
{
    static_assert(ModuleLevel == fastinterpreter::NoFunction, "the API names the same module-level refusal");

    /// Which compile limit each lowering failure but the host's memory is.
    struct FailureLimit
    {
        fastinterpreter::LoweringFailure failure; ///< Why lowering refused.
        CompileLimit limit;                       ///< The limit the API reports.
    };

    constexpr auto FailureLimits = std::to_array<FailureLimit>({
        { .failure = fastinterpreter::LoweringFailure::Budget, .limit = CompileLimit::Budget },
        { .failure = fastinterpreter::LoweringFailure::FrameSlots, .limit = CompileLimit::FrameSlots },
        { .failure = fastinterpreter::LoweringFailure::CodeWords, .limit = CompileLimit::CodeWords },
    });

    /// How Engine::load() reports a module the fast engine refused to lower: the host out of memory as
    /// such, which is not the module's to answer for, and every other refusal as the limit it met.
    [[nodiscard]] LoadError loadErrorOf(fastinterpreter::LoweringError const& error) noexcept
    {
        auto const limit = std::ranges::find(FailureLimits, error.reason, &FailureLimit::failure);
        if (limit == FailureLimits.end())
            return LoadError { HostMemoryExhausted {} };
        return LoadError { CompileLimitExceeded { .codeIndex = error.codeIndex,
                                                  .requested = error.requested,
                                                  .limit = error.limit,
                                                  .kind = limit->limit } };
    }
} // namespace

EngineIdentity EngineConfig::identity() const noexcept
{
    auto const* const row = engines::rowOf(engine);
    auto const lowered = row != nullptr && row->dispatch == engines::DispatchUse::Lowered;
    return EngineIdentity { .engine = engine,
                            .mode = mode,
                            .features = features,
                            .dispatch = lowered ? std::optional { dispatch } : std::nullopt };
}

Engine::Engine(EngineConfig config) noexcept: _config { std::move(config) }
{
    Require(_config.features.checkPrerequisites().has_value());
    Require(engines::rowOf(_config.engine) != nullptr);
    // An engine that lowers for its dispatch runs with it too, which this build must have compiled.
    Require(engines::rowOf(_config.engine)->dispatch == engines::DispatchUse::None
            || fastinterpreter::isAvailable(_config.dispatch));
}

ThreadPreparation::ThreadPreparation(std::unique_ptr<IThreadResource> resource) noexcept:
    _resource { std::move(resource) }
{
    Require(_resource != nullptr);
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
                auto const request = engines::CodeRequest { .mode = _config.mode,
                                                            .dispatch = _config.dispatch,
                                                            .budget = _config.load.maxCompileBytes,
                                                            .memory = _config.loweringMemory };
                return engines::rowOf(_config.engine)
                    ->code(*validated, request)
                    .transform_error(&loadErrorOf)
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
