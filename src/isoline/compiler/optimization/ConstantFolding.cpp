// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/optimization/FoldingTable.hpp>
#include <isoline/compiler/optimization/PassTable.hpp>
#include <isoline/compiler/optimization/Rewrite.hpp>
#include <isoline/runtime/Trap.hpp>

namespace isoline::compiler::optimization
{

using intermediate::BlockId;
using intermediate::Builder;
using intermediate::Function;
using intermediate::ImmediateRange;
using intermediate::Instruction;
using intermediate::OperandRange;
using intermediate::Operation;
using intermediate::Type;
using intermediate::ValueId;

namespace
{
    /// The constant operation of each type a folded value may have, by Type.
    [[nodiscard]] constexpr Operation constantOperationOf(Type type) noexcept
    {
        switch (type)
        {
            case Type::I32: return Operation::I32Const;
            case Type::I64: return Operation::I64Const;
            case Type::F32: return Operation::F32Const;
            case Type::F64: return Operation::F64Const;
            case Type::Ref:            // uncovered(constant-of-a-non-numeric-type)
            case Type::Pointer: break; // uncovered(constant-of-a-non-numeric-type)
        }
        Require(false);            // isFoldable() admits numeric results only
        return Operation::Removed; // uncovered(constant-of-a-non-numeric-type)
    }

    /// Whether a fold ended its block.
    enum class BlockEnd : uint8_t
    {
        Continues, ///< The block goes on after it.
        Ended,     ///< It ended the block with a trap terminator.
    };

    /// Whether the pass took an edge away, which may leave blocks unreachable.
    enum class EdgesRemoved : uint8_t
    {
        None, ///< Every edge stays.
        Some, ///< A branch became a jump, or a block a trap.
    };

    /// The pass over one function: its definitions and whether it removed an edge.
    class Folding
    {
      public:
        Folding(Function& function, PassContext& context):
            _function { function },
            _context { context },
            _definitions { definitionsOf(function, context.memory) },
            _operands { &context.memory },
            _poll { context }
        {
        }

        /// Folds every reachable block, in reverse postorder so that a value is folded before its uses.
        [[nodiscard]] PassOutcome run()
        {
            auto const order = reversePostorderOf(_function, _context.memory);
            for (auto const block: order)
                if (auto folded = foldBlock(block); !folded)
                    return folded;
            if (_edgesRemoved == EdgesRemoved::Some)
                std::ignore = removeUnreachableBlocks(_function, _context.memory);
            return {};
        }

      private:
        /// Folds the instructions of @p block in order, ending it at the first that always traps, polling the
        /// cancellation per instruction.
        /// @return Nothing; or Cancelled.
        [[nodiscard]] PassOutcome foldBlock(BlockId block)
        {
            auto const count = _function.block(block).instructions.size();
            for (auto const position: std::views::iota(std::size_t { 0 }, count))
            {
                if (auto polled = _poll.step(1); !polled)
                    return polled;
                auto const index = _function.block(block).instructions[position];
                auto const& instruction = _function.instruction(index);
                switch (instruction.operation)
                {
                    case Operation::Removed: break;
                    case Operation::TrapIf:
                    case Operation::TrapIfSubject:
                        if (foldTrapIf(block, position, index) == BlockEnd::Ended)
                            return {};
                        break;
                    case Operation::Branch: foldBranch(index); break;
                    case Operation::Switch: foldSwitch(index); break;
                    default:
                        if (foldOperation(block, position, index) == BlockEnd::Ended)
                            return {};
                        break;
                }
            }
            return {};
        }

        /// The constant @p value is, if it is one.
        [[nodiscard]] std::optional<uint64_t> constant(ValueId value) const noexcept
        {
            return constantOf(_function, _definitions, value);
        }

        /// Folds trapIf or trapIfSubject at @p position of @p block on a constant condition: on zero it never
        /// traps and goes; trapIf on another constant always traps and ends the block. trapIfSubject on a
        /// non-zero constant stays: a trap terminator names no subject.
        /// @return Whether it ended the block.
        BlockEnd foldTrapIf(BlockId block, std::size_t position, uint32_t index)
        {
            auto const& instruction = _function.instruction(index);
            auto const condition = constant(_function.operands(instruction.operands)[0]);
            if (!condition)
                return BlockEnd::Continues;
            if (static_cast<uint32_t>(*condition) == 0)
            {
                _function.remove(index);
                return BlockEnd::Continues;
            }
            if (instruction.operation == Operation::TrapIfSubject)
                return BlockEnd::Continues;
            auto const code = static_cast<runtime::TrapCode>(_function.immediates(instruction.immediates)[0]);
            endWithTrap(block, position, code);
            return BlockEnd::Ended;
        }

        /// Turns a branch on a constant condition into a jump to the target it takes, in place: that target's
        /// immediates and arguments are already a run of the branch's.
        void foldBranch(uint32_t index)
        {
            auto& instruction = _function.instruction(index);
            auto const condition = constant(_function.operands(instruction.operands)[0]);
            if (!condition)
                return;
            auto const targets = _function.targets(instruction);
            Require(targets.size() == 2);
            auto const taken = static_cast<uint32_t>(*condition) != 0 ? std::size_t { 0 } : std::size_t { 1 };
            toJump(instruction, taken, targets[taken].arguments);
        }

        /// Turns a switch on a constant index into a jump to the target it takes: the index's target when it
        /// is below their number, else the fallback, which is the first.
        void foldSwitch(uint32_t index)
        {
            auto& instruction = _function.instruction(index);
            auto const selector = constant(_function.operands(instruction.operands)[0]);
            if (!selector)
                return;
            auto const targets = _function.targets(instruction);
            auto const chosen = static_cast<uint32_t>(*selector);
            auto const taken = uint64_t { chosen } + 1 < targets.size() ? std::size_t { chosen } + 1 : 0;
            toJump(instruction, taken, targets[taken].arguments);
        }

        /// Rewrites @p instruction into a jump to its target @p taken, which passes @p arguments.
        void toJump(Instruction& instruction, std::size_t taken, OperandRange arguments)
        {
            instruction.operation = Operation::Jump;
            instruction.operands = arguments;
            instruction.immediates =
                ImmediateRange { .begin = instruction.immediates.begin + static_cast<uint32_t>(2 * taken),
                                 .count = 2 };
            _edgesRemoved = EdgesRemoved::Some;
        }

        /// Folds the FoldingTable operation at @p position of @p block when all its operands are constants.
        /// @return Whether it trapped, ending the block.
        BlockEnd foldOperation(BlockId block, std::size_t position, uint32_t index)
        {
            auto const& instruction = _function.instruction(index);
            auto const row = foldingRowOf(instruction.operation);
            if (!row)
                return BlockEnd::Continues;
            _operands.clear();
            for (auto const operand: _function.operands(instruction.operands))
            {
                auto const value = constant(operand);
                if (!value)
                    return BlockEnd::Continues;
                _operands.push_back(*value);
            }
            auto const type = instruction.type;
            auto const folded = row->kernel(_operands, type);
            if (!folded)
            {
                endWithTrap(block, position, folded.error());
                return BlockEnd::Ended;
            }
            auto builder = Builder { _function };
            builder.replaceImmediates(index, std::array { *folded });
            auto& rewritten = _function.instruction(index);
            rewritten.operation = constantOperationOf(type);
            rewritten.operands = OperandRange { .begin = rewritten.operands.begin, .count = 0 };
            return BlockEnd::Continues;
        }

        /// Removes the instructions of @p block from @p position on, and ends it with a trap of @p code: what
        /// followed never runs, and the blocks only it reached no longer do.
        void endWithTrap(BlockId block, std::size_t position, runtime::TrapCode code)
        {
            auto const& listed = _function.block(block).instructions;
            for (auto const index: std::span { listed }.subspan(position))
                _function.remove(index);
            auto builder = Builder { _function };
            builder.switchTo(block);
            builder.trap(code);
            _edgesRemoved = EdgesRemoved::Some;
        }

        Function& _function;
        PassContext& _context;
        std::pmr::vector<uint32_t> _definitions;
        std::pmr::vector<uint64_t> _operands;            ///< The operands of the operation being folded.
        CancellationPoll _poll;                          ///< Polled per instruction.
        EdgesRemoved _edgesRemoved = EdgesRemoved::None; ///< Then unreachable blocks are removed.
    };
} // namespace

PassOutcome foldConstants(Function& function, PassContext& context)
{
    return Folding { function, context }.run();
}

} // namespace isoline::compiler::optimization
