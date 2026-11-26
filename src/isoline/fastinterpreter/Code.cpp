// SPDX-License-Identifier: Apache-2.0
#include <isoline/fastinterpreter/Code.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <utility>

namespace isoline::fastinterpreter
{

namespace
{
    /// Whether @p functions and every container of each allocate from @p arena.
    [[nodiscard]] bool allocateFrom(std::pmr::memory_resource const* arena,
                                    std::pmr::vector<LoweredFunction> const& functions) noexcept
    {
        auto const fromArena = [arena](auto const& container) {
            return container.get_allocator().resource() == arena;
        };
        return fromArena(functions)
               && std::ranges::all_of(functions, [&fromArena](LoweredFunction const& function) {
                      return fromArena(function.code) && fromArena(function.localInit)
                             && fromArena(function.operands) && fromArena(function.sites)
                             && fromArena(function.handlers) && fromArena(function.catches);
                  });
    }
} // namespace

LoweredModule::LoweredModule(std::shared_ptr<std::pmr::memory_resource> arena,
                             std::pmr::vector<LoweredFunction> functions,
                             Dispatch dispatch,
                             runtime::ExecutionMode mode,
                             uint64_t loweredBytes):
    _arena { std::move(arena) },
    _functions { std::move(functions) },
    _dispatch { dispatch },
    _mode { mode },
    _loweredBytes { loweredBytes }
{
    Require(allocateFrom(_arena.get(), _functions));
}

runtime::ExecutionEngine LoweredModule::engine() const noexcept
{
    return runtime::ExecutionEngine::Fast;
}

std::span<LoweredFunction const> LoweredModule::functions() const noexcept
{
    return _functions;
}

Dispatch LoweredModule::dispatch() const noexcept
{
    return _dispatch;
}

runtime::ExecutionMode LoweredModule::mode() const noexcept
{
    return _mode;
}

uint64_t LoweredModule::loweredBytes() const noexcept
{
    return _loweredBytes;
}

} // namespace isoline::fastinterpreter
