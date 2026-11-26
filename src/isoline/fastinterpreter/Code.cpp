// SPDX-License-Identifier: Apache-2.0
#include <isoline/fastinterpreter/Code.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <span>
#include <utility>

namespace isoline::fastinterpreter
{

namespace
{
    /// Whether every container of @p function allocates from @p arena.
    [[nodiscard]] bool allocatesFrom(LoweredFunction const& function,
                                     std::pmr::memory_resource const* arena) noexcept
    {
        auto const resources = std::array {
            function.code.get_allocator().resource(),     function.localInit.get_allocator().resource(),
            function.operands.get_allocator().resource(), function.sites.get_allocator().resource(),
            function.handlers.get_allocator().resource(), function.catches.get_allocator().resource(),
        };
        return std::ranges::count(resources, arena) == std::ssize(resources);
    }

    /// Whether @p functions and every container of each allocate from @p arena.
    [[nodiscard]] bool allocateFrom(std::pmr::memory_resource const* arena,
                                    std::pmr::vector<LoweredFunction> const& functions) noexcept
    {
        return functions.get_allocator().resource() == arena
               && std::ranges::all_of(functions, [arena](LoweredFunction const& function) {
                      return allocatesFrom(function, arena);
                  });
    }
} // namespace

LoweredModule::LoweredModule(std::shared_ptr<std::pmr::memory_resource> arena,
                             std::pmr::vector<LoweredFunction> functions,
                             Dispatch dispatch,
                             runtime::ExecutionMode mode,
                             uint64_t loweredBytes,
                             LoweringStats stats):
    _arena { std::move(arena) },
    _functions { std::move(functions) },
    _dispatch { dispatch },
    _mode { mode },
    _loweredBytes { loweredBytes },
    _stats { stats }
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

LoweringStats const& LoweredModule::stats() const noexcept
{
    return _stats;
}

} // namespace isoline::fastinterpreter
