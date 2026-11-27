// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/optimization/PassTable.hpp>
#include <isoline/compiler/optimization/Rewrite.hpp>

namespace isoline::compiler::optimization
{

using intermediate::BlockId;
using intermediate::Effect;
using intermediate::Function;
using intermediate::Operation;
using intermediate::Unnumbered;
using intermediate::ValueId;

namespace
{
    /// Whether a value or an instruction is needed.
    enum class Liveness : uint8_t
    {
        Dead, ///< Nothing kept needs it (yet).
        Live, ///< Something kept needs it.
    };

    /// Whether DCE may remove an instruction of @p operation that nothing needs: the Decisions' column
    /// (Effect), Pure and ReadsContext only.
    [[nodiscard]] constexpr bool isRemovableWhenUnused(Operation operation) noexcept
    {
        auto const effect = intermediate::effectOf(operation);
        return effect == Effect::Pure || effect == Effect::ReadsContext;
    }

    /// The pass over one function: marks what the kept instructions need, then removes the rest.
    class DeadCode
    {
      public:
        DeadCode(Function& function, PassContext& context):
            _function { function },
            _context { context },
            _definitions { definitionsOf(function, context.memory) },
            _places { parameterPlacesOf(function, context.memory) },
            _incoming { incomingEdgesOf(function, context.memory) },
            _values(function.valueCount(), Liveness::Dead, &context.memory),
            _instructions(function.instructionCount(), Liveness::Dead, &context.memory),
            _worklist { &context.memory }
        {
        }

        [[nodiscard]] PassOutcome run()
        {
            auto poll = CancellationPoll { _context };
            for (auto const blockIndex: std::views::iota(uint32_t { 0 }, _function.blockCount()))
            {
                auto const& block = _function.block(BlockId { blockIndex });
                for (auto const index: block.instructions)
                {
                    auto const& instruction = _function.instruction(index);
                    if (instruction.operation != Operation::Removed
                        && !isRemovableWhenUnused(instruction.operation))
                        keep(index);
                }
                if (auto polled = poll.step(block.instructions.size() + 1); !polled)
                    return polled;
            }
            while (!_worklist.empty())
            {
                auto const value = _worklist.back();
                _worklist.pop_back();
                needDefinitionOf(value);
                if (auto polled = poll.step(1); !polled)
                    return polled;
            }
            sweep();
            return {};
        }

      private:
        /// Marks @p value needed, once.
        void need(ValueId value)
        {
            auto& liveness = _values[std::to_underlying(value)];
            if (liveness == Liveness::Live)
                return;
            liveness = Liveness::Live;
            _worklist.push_back(value);
        }

        /// Keeps the instruction at @p index, and needs what it reads: every operand but the arguments of its
        /// targets, which only a needed parameter needs.
        void keep(uint32_t index)
        {
            auto& liveness = _instructions[index];
            if (liveness == Liveness::Live)
                return;
            liveness = Liveness::Live;
            auto const& instruction = _function.instruction(index);
            auto arguments = std::size_t { 0 };
            _function.forEachTarget(instruction, [&arguments](intermediate::BranchTarget const& target) {
                arguments += target.arguments.count;
            });
            auto const operands = _function.operands(instruction.operands);
            for (auto const operand: operands.first(operands.size() - arguments))
                need(operand);
        }

        /// What needing @p value needs: the instruction that defines it, or, for a parameter, the argument
        /// every edge passes it.
        void needDefinitionOf(ValueId value)
        {
            auto const definition = _definitions[std::to_underlying(value)];
            if (definition != Unnumbered)
            {
                keep(definition);
                return;
            }
            auto const block = _places.block[std::to_underlying(value)];
            if (block == Unnumbered)
                return; // uncovered(value-neither-defined-nor-a-parameter)
            auto const position = _places.position[std::to_underlying(value)];
            for (auto const& edge: _incoming.into(BlockId { block }))
                need(_function.operands(
                    intermediate::OperandRange { .begin = edge.arguments + position, .count = 1 })[0]);
        }

        /// Removes every removable instruction not kept, and every parameter not needed but the entry's.
        void sweep()
        {
            auto fates = std::pmr::vector<ParameterFate>(
                _function.valueCount(), ParameterFate::Kept, &_context.memory);
            auto dropped = std::size_t { 0 };
            for (auto const blockIndex: std::views::iota(uint32_t { 0 }, _function.blockCount()))
            {
                auto const& block = _function.block(BlockId { blockIndex });
                for (auto const index: block.instructions)
                    if (_function.instruction(index).operation != Operation::Removed
                        && _instructions[index] == Liveness::Dead)
                        _function.remove(index);
                if (blockIndex == 0)
                    continue;
                for (auto const parameter: _function.operands(block.parameters))
                    if (_values[std::to_underlying(parameter)] == Liveness::Dead)
                    {
                        fates[std::to_underlying(parameter)] = ParameterFate::Dropped;
                        ++dropped;
                    }
            }
            if (dropped != 0)
                dropParameters(_function, fates);
        }

        Function& _function;
        PassContext& _context;
        std::pmr::vector<uint32_t> _definitions;
        ParameterPlaces _places;
        IncomingEdges _incoming;
        std::pmr::vector<Liveness> _values;       ///< Per value, whether it is needed.
        std::pmr::vector<Liveness> _instructions; ///< Per instruction, whether it is kept.
        std::pmr::vector<ValueId> _worklist;      ///< Needed values whose definitions are not yet kept.
    };
} // namespace

PassOutcome eliminateDeadCode(Function& function, PassContext& context)
{
    return DeadCode { function, context }.run();
}

} // namespace isoline::compiler::optimization
