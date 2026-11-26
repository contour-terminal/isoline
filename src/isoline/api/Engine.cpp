// SPDX-License-Identifier: Apache-2.0
#include <isoline/api/Engine.hpp>

#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>

#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::api
{

EngineIdentity EngineConfig::identity() const noexcept
{
    return EngineIdentity { .engine = engine, .mode = mode, .features = features };
}

Engine::Engine(EngineConfig config): _config { config }
{
    Require(_config.features.checkPrerequisites().has_value());
}

ThreadPreparation::ThreadPreparation(std::unique_ptr<IThreadResource> resource):
    _resource { std::move(resource) }
{
    Require(_resource != nullptr);
}

std::expected<Module, LoadError> Engine::load(std::span<std::byte const> bytes) const noexcept
{
    if (bytes.size() > _config.load.maxModuleBytes)
        return std::unexpected { LoadError {
            ModuleTooLarge { .bytes = bytes.size(), .limit = _config.load.maxModuleBytes } } };

    // The API boundary: decoding, validating and describing allocate, and the host running out of
    // memory is a refusal, never an exception in the embedder's lap.
    try
    {
        return validate::ValidatedModule::load(bytes, _config.features, _config.load.decode)
            .transform([this](std::shared_ptr<validate::ValidatedModule const> validated) {
                return Module { Module::Key {}, std::move(validated), _config.identity() };
            })
            .transform_error([](validate::ModuleError error) {
                return std::visit([](auto alternative) { return LoadError { std::move(alternative) }; },
                                  std::move(error));
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
