// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/intermediate/MemoryStretches.hpp>

#include <cstdint>
#include <memory_resource>
#include <utility>
#include <vector>

#include <isoline/compiler/intermediate/Guards.hpp>

namespace isoline::compiler::intermediate
{

Stretch MemoryStretches::after(Function const& function, uint32_t index, Stretch current) noexcept
{
    if (memoryMotionOf(function.instruction(index).operation) == MemoryMotion::MayMove)
        return Stretch { 1 } + index;
    return current;
}

MemoryStretches::MemoryStretches(Function const& function,
                                 ControlFlowGraph const& graph,
                                 std::pmr::memory_resource& memory):
    _entries(function.blockCount(), UnreachedStretch, &memory)
{
    if (function.blockCount() == 0)
        return;
    auto worklist = std::pmr::vector<BlockId> { &memory };
    _entries[0] = 0;
    worklist.push_back(BlockId { 0 });
    while (!worklist.empty())
    {
        auto const block = worklist.back();
        worklist.pop_back();
        auto exit = _entries[std::to_underlying(block)];
        for (auto const index: function.block(block).instructions)
            exit = after(function, index, exit);
        for (auto const successor: graph.successors(block))
        {
            auto& entry = _entries[std::to_underlying(successor)];
            auto const merged = Stretch { 1 } + function.instructionCount() + std::to_underlying(successor);
            if (entry == exit || entry == merged)
                continue;
            entry = entry == UnreachedStretch ? exit : merged;
            worklist.push_back(successor);
        }
    }
}

} // namespace isoline::compiler::intermediate
