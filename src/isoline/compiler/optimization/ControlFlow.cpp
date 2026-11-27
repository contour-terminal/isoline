// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
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
using intermediate::Function;
using intermediate::ImmediateField;
using intermediate::Operation;
using intermediate::ValueId;

namespace
{
    /// The listed instructions of @p block that are not removed, up to two: whether it holds only one.
    [[nodiscard]] std::optional<uint32_t> onlyInstructionOf(Function const& function, BlockId block) noexcept
    {
        auto only = std::optional<uint32_t> {};
        for (auto const index: function.block(block).instructions)
        {
            if (function.instruction(index).operation == Operation::Removed)
                continue;
            if (only)
                return std::nullopt;
            only = index;
        }
        return only;
    }

    /// Where a forwarding block's chain ends, as the block reached and the values passed to it.
    struct Forward
    {
        BlockId block {};   ///< The first block of the chain that does not forward.
        uint32_t begin = 0; ///< Where its arguments start in Threading's pool.
        uint32_t count = 0; ///< How many there are: the block's parameters.
    };

    /// How far the resolution of a block's chain is.
    enum class ChainState : uint8_t
    {
        NotForwarding, ///< The block does not forward: a chain ends at it.
        Unresolved,    ///< It forwards; its chain's end is not known yet.
        InProgress,    ///< Its chain is being followed: meeting it again is a cycle.
        Resolved,      ///< Its chain's end is known (forwards[]).
    };

    /// Redirects every edge into a forwarding block to the end of its chain.
    ///
    /// A block forwards when it holds only a jump to another block, passes no more values than it takes,
    /// and nothing but that jump uses its parameters: then an edge into it may go where its jump goes,
    /// passing what the jump would, with the block's parameters replaced by the edge's arguments. Since the
    /// jump passes no more than the block takes, no edge grows, and each terminator is rewritten in place.
    /// A value the jump passes that is not the block's own is defined in a block that dominates it, and so
    /// every block with an edge into it. A block holding anything else, a poll included, never forwards.
    class Threading
    {
      public:
        Threading(Function& function, PassContext& context):
            _function { function },
            _context { context },
            _places { parameterPlacesOf(function, context.memory) },
            _states(function.blockCount(), ChainState::NotForwarding, &context.memory),
            _forwards(function.blockCount(), Forward {}, &context.memory),
            _pool { &context.memory },
            _path { &context.memory },
            _scratch { &context.memory }
        {
        }

        /// Redirects the edges.
        /// @return How many it redirected; or Cancelled.
        [[nodiscard]] std::expected<uint32_t, CompileError> run()
        {
            findForwardingBlocks();
            auto poll = CancellationPoll { _context };
            auto redirected = uint32_t { 0 };
            for (auto const blockIndex: std::views::iota(uint32_t { 0 }, _function.blockCount()))
            {
                auto const block = BlockId { blockIndex };
                if (auto const index = _function.terminator(block))
                    redirected += redirect(*index);
                if (auto polled = poll.step(_function.block(block).instructions.size() + 1); !polled)
                    return std::unexpected { polled.error() };
            }
            return redirected;
        }

      private:
        /// The jump a forwarding block holds.
        [[nodiscard]] intermediate::Instruction const& jumpOf(BlockId block) const noexcept
        {
            return _function.instruction(*_function.terminator(block));
        }

        /// The block a jump goes to.
        [[nodiscard]] BlockId targetOf(intermediate::Instruction const& jump) const noexcept
        {
            return BlockId { static_cast<uint32_t>(_function.immediates(jump.immediates)[0]) };
        }

        /// Marks every block that forwards Unresolved.
        void findForwardingBlocks()
        {
            auto uses = std::pmr::vector<uint32_t>(_function.valueCount(), 0, &_context.memory);
            for (auto const blockIndex: std::views::iota(uint32_t { 0 }, _function.blockCount()))
                for (auto const index: _function.block(BlockId { blockIndex }).instructions)
                    if (auto const& instruction = _function.instruction(index);
                        instruction.operation != Operation::Removed)
                        for (auto const operand: _function.operands(instruction.operands))
                            ++uses[std::to_underlying(operand)];
            for (auto const blockIndex: std::views::iota(uint32_t { 1 }, _function.blockCount()))
            {
                auto const block = BlockId { blockIndex };
                auto const only = onlyInstructionOf(_function, block);
                if (!only || _function.instruction(*only).operation != Operation::Jump)
                    continue;
                auto const& jump = _function.instruction(*only);
                auto const parameters = _function.operands(_function.block(block).parameters);
                auto const arguments = _function.operands(jump.operands);
                if (targetOf(jump) == block || arguments.size() > parameters.size())
                    continue;
                // Nothing but the jump may use a parameter: a block it dominates would lose the definition.
                for (auto const argument: arguments)
                    if (_places.block[std::to_underlying(argument)] == blockIndex)
                        --uses[std::to_underlying(argument)];
                auto const usedElsewhere = std::ranges::any_of(parameters, [&uses](ValueId parameter) {
                    return uses[std::to_underlying(parameter)] != 0;
                });
                if (!usedElsewhere)
                    _states[blockIndex] = ChainState::Unresolved;
            }
        }

        /// The end of @p block's chain, resolved once for every block on it, on an explicit stack.
        [[nodiscard]] Forward const& chainOf(BlockId block)
        {
            _path.clear();
            _path.push_back(block);
            while (!_path.empty())
            {
                auto const current = _path.back();
                auto& state = _states[std::to_underlying(current)];
                if (state == ChainState::Resolved)
                {
                    _path.pop_back();
                    continue;
                }
                auto const& jump = jumpOf(current);
                auto const next = targetOf(jump);
                auto const nextState = _states[std::to_underlying(next)];
                if (state == ChainState::Unresolved && nextState == ChainState::Unresolved)
                {
                    state = ChainState::InProgress;
                    _path.push_back(next);
                    continue;
                }
                resolve(current, jump, next);
                _path.pop_back();
            }
            return _forwards[std::to_underlying(block)];
        }

        /// Resolves @p block's chain from its jump to @p next: through next's chain when next forwards and is
        /// resolved, to next itself otherwise (a block that does not forward, or a cycle of forwarding
        /// blocks).
        void resolve(BlockId block, intermediate::Instruction const& jump, BlockId next)
        {
            auto const arguments = _function.operands(jump.operands);
            auto const begin = static_cast<uint32_t>(_pool.size());
            auto end =
                Forward { .block = next, .begin = begin, .count = static_cast<uint32_t>(arguments.size()) };
            if (_states[std::to_underlying(next)] == ChainState::Resolved
                && _forwards[std::to_underlying(next)].block != block)
            {
                // next's chain passes its own parameters and outer values: its parameters become this jump's
                // arguments.
                auto const through = _forwards[std::to_underlying(next)];
                end = Forward { .block = through.block, .begin = begin, .count = through.count };
                for (auto const position: std::views::iota(through.begin, through.begin + through.count))
                {
                    auto const value = _pool[position];
                    auto const place = std::to_underlying(value);
                    _pool.push_back(_places.block[place] == std::to_underlying(next)
                                        ? arguments[_places.position[place]]
                                        : value);
                }
            }
            else
                _pool.insert(_pool.end(), arguments.begin(), arguments.end());
            _forwards[std::to_underlying(block)] = end;
            _states[std::to_underlying(block)] = ChainState::Resolved;
        }

        /// Redirects the edges of the terminator at @p index into forwarding blocks, in place.
        /// @return How many it redirected.
        uint32_t redirect(uint32_t index)
        {
            auto& terminator = _function.instruction(index);
            auto const& row = intermediate::operationRow(terminator.operation);
            auto const& schema = intermediate::immediateSchemaRow(row.immediates);
            auto const immediates = _function.immediates(terminator.immediates);
            auto const operands = _function.operands(terminator.operands);
            auto read = std::size_t { intermediate::operandSignatureRow(row.operands).count };
            auto write = read;
            auto redirected = uint32_t { 0 };
            for (auto const position: std::views::iota(std::size_t { 0 }, immediates.size()))
            {
                if (intermediate::immediateFieldAt(schema, position) != ImmediateField::Target)
                    continue;
                auto const target = BlockId { static_cast<uint32_t>(immediates[position]) };
                auto const count = static_cast<std::size_t>(immediates[position + 1]);
                if (_states[std::to_underlying(target)] == ChainState::NotForwarding
                    || chainOf(target).block == target)
                {
                    // Kept as it is; write <= read, so a forward copy never overwrites what it still reads.
                    for (auto const argument: std::views::iota(read, read + count))
                        operands[write++] = operands[argument];
                    read += count;
                    continue;
                }
                auto const& end = _forwards[std::to_underlying(target)];
                _scratch.assign(operands.begin() + static_cast<std::ptrdiff_t>(read),
                                operands.begin() + static_cast<std::ptrdiff_t>(read + count));
                for (auto const value: std::span { _pool }.subspan(end.begin, end.count))
                {
                    auto const place = std::to_underlying(value);
                    operands[write++] = _places.block[place] == std::to_underlying(target)
                                            ? _scratch[_places.position[place]]
                                            : value;
                }
                read += count;
                Require(write <= read);
                immediates[position] = std::to_underlying(end.block);
                immediates[position + 1] = end.count;
                ++redirected;
            }
            // Operands past the targets' (a return's values) are no arguments: they stay as they are.
            for (auto const operand: std::views::iota(read, operands.size()))
                operands[write++] = operands[operand];
            terminator.operands.count = static_cast<uint32_t>(write);
            return redirected;
        }

        Function& _function;
        PassContext& _context;
        ParameterPlaces _places;
        std::pmr::vector<ChainState> _states; ///< Per block, its chain's resolution.
        std::pmr::vector<Forward> _forwards;  ///< Per resolved block, its chain's end.
        std::pmr::vector<ValueId> _pool;      ///< The resolved chains' arguments.
        std::pmr::vector<BlockId> _path;      ///< The chain being resolved.
        std::pmr::vector<ValueId> _scratch;   ///< The arguments of the edge being redirected.
    };

    /// Merges each block that ends with a jump into a block with no other predecessor with that block, in
    /// reverse postorder, so that a chain's head takes in the whole chain and every instruction moves once.
    /// The merged block's parameters become the jump's arguments; the head keeps its own hotness.
    [[nodiscard]] PassOutcome mergeChains(Function& function, PassContext& context)
    {
        auto predecessors = std::pmr::vector<uint32_t>(function.blockCount(), 0, &context.memory);
        for (auto const blockIndex: std::views::iota(uint32_t { 0 }, function.blockCount()))
            if (auto const index = function.terminator(BlockId { blockIndex }))
                function.forEachTarget(function.instruction(*index),
                                       [&predecessors](intermediate::BranchTarget const& target) {
                                           ++predecessors[std::to_underlying(target.block)];
                                       });
        auto merged = std::pmr::vector<uint8_t>(function.blockCount(), 0, &context.memory);
        auto substitution = Substitution { function.valueCount(), context.memory };
        auto poll = CancellationPoll { context };
        auto removed = std::pmr::vector<BlockId> { &context.memory };
        for (auto const head: reversePostorderOf(function, context.memory))
        {
            if (merged[std::to_underlying(head)] != 0)
                continue;
            while (true)
            {
                auto const index = function.terminator(head);
                if (!index || function.instruction(*index).operation != Operation::Jump)
                    break;
                auto const& jump = function.instruction(*index);
                auto const next = BlockId { static_cast<uint32_t>(function.immediates(jump.immediates)[0]) };
                if (next == head || std::to_underlying(next) == 0
                    || predecessors[std::to_underlying(next)] != 1)
                    break;
                auto const parameters = function.operands(function.block(next).parameters);
                auto const arguments = function.operands(jump.operands);
                for (auto const position: std::views::iota(std::size_t { 0 }, parameters.size()))
                    substitution.replace(parameters[position], arguments[position]);
                function.remove(*index);
                auto& taken = function.block(next).instructions;
                auto& into = function.block(head).instructions;
                into.insert(into.end(), taken.begin(), taken.end());
                taken.clear();
                merged[std::to_underlying(next)] = 1;
                if (auto polled = poll.step(into.size()); !polled)
                    return polled;
            }
        }
        for (auto const blockIndex: std::views::iota(uint32_t { 0 }, function.blockCount()))
            if (merged[blockIndex] != 0)
                removed.push_back(BlockId { blockIndex });
        substitution.applyTo(function);
        function.removeBlocks(removed);
        return {};
    }
} // namespace

PassOutcome simplifyControlFlow(Function& function, PassContext& context)
{
    std::ignore = removeUnreachableBlocks(function, context.memory);
    auto threading = Threading { function, context };
    auto const redirected = threading.run();
    if (!redirected)
        return std::unexpected { redirected.error() };
    if (*redirected != 0)
        std::ignore = removeUnreachableBlocks(function, context.memory);
    return mergeChains(function, context);
}

} // namespace isoline::compiler::optimization
