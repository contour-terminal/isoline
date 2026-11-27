// SPDX-License-Identifier: Apache-2.0
//
// Bounds-check elimination (basics): the two rules of intermediate/Guards.hpp, and nothing else. A check is
// removed only where a proof shows it can never trap and its clamp is kept or not needed:
//  (a) its index is a constant and its access ends within the memory's declared minimum, which every size the
//      memory can have reaches: it becomes inBoundsAddress, held to that minimum by the verifier;
//  (b) a check of the same memory and SSA index dominates it, covering its bytes (guardCovers()), and no
//      operation that may grow or move a memory (MemoryMotionTable) lies on any path between: it takes that
//      check's address, through guardAddress when it lies further on. The dominating check's select is the
//      clamp, and guardAddress keeps its 0.
// No range analysis: an index is proved only by a constant or by the very SSA value an earlier check took.
//
// Rule (b)'s "no motion between" is intermediate/MemoryStretches.hpp's numbering, which the verifier holds
// every use of a guarded pointer to as well: the dominator-tree walk keeps, per index value, the widest
// checks of the current stretch in scope.

#include <core/Assert.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Guards.hpp>
#include <isoline/compiler/intermediate/MemoryStretches.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/optimization/PassTable.hpp>
#include <isoline/compiler/optimization/Rewrite.hpp>

namespace isoline::compiler::optimization
{

namespace
{
    using intermediate::BlockId;
    using intermediate::Operation;
    using intermediate::Unnumbered;
    using intermediate::ValueId;

    using intermediate::Stretch;

    /// How many facts of one index a lookup reads, newest first: a bound on its work, so that the walk stays
    /// linear however many memories or widths one index is checked at. A fact past it is not found, and its
    /// check is kept.
    constexpr auto FactsReadPerIndex = 4U;

    /// A check that dominates the rest of its subtree within its stretch: what rule (b) proves by.
    struct Fact
    {
        uint64_t memory = 0;        ///< Its memory.
        Stretch stretch = 0;        ///< The stretch it was made in.
        ValueId address {};         ///< The address it returned, its select applied.
        uint64_t offset = 0;        ///< Its static offset.
        uint64_t size = 0;          ///< Its width.
        uint32_t next = Unnumbered; ///< The fact of the same index it shadows; Unnumbered if none.
    };

    /// What a block's walk changed, to undo when its subtree is done.
    struct Undo
    {
        ValueId index {};           ///< The index whose newest fact changed.
        uint32_t head = Unnumbered; ///< Its newest fact before.
    };

    /// One block of the explicit-stack walk of the dominator tree.
    struct Frame
    {
        BlockId block {};         ///< The block.
        uint32_t nextChild = 0;   ///< Its next child to walk, among its children.
        std::size_t undoMark = 0; ///< The undo log's size when the block was entered.
    };

    /// One run of the pass over one function.
    class Elimination
    {
      public:
        Elimination(intermediate::Function& function, PassContext& context):
            _function { function },
            _context { context },
            _memory { context.memory },
            _builder { function },
            _graph { intermediate::controlFlowGraphOf(function, context.memory) },
            _tree { _graph, context.memory },
            _definitions { definitionsOf(function, context.memory) },
            _stretches { function, _graph, context.memory },
            _heads(function.valueCount(), Unnumbered, &context.memory),
            _facts(&context.memory),
            _undo(&context.memory),
            _substitution { function.valueCount(), context.memory },
            _cancellation { context }
        {
        }

        [[nodiscard]] PassOutcome run()
        {
            if (auto walked = walk(); !walked)
                return walked;
            if (!_substitution.isEmpty())
                _substitution.applyTo(_function);
            return {};
        }

      private:
        /// The children of every reachable block in the dominator tree, block after block:
        /// children[begins[b], begins[b + 1]).
        struct Children
        {
            std::pmr::vector<uint32_t> begins;
            std::pmr::vector<BlockId> blocks;
        };

        [[nodiscard]] Children childrenOf() const
        {
            auto const count = _function.blockCount();
            auto children = Children { .begins = std::pmr::vector<uint32_t>(count + 1, 0, &_memory),
                                       .blocks = std::pmr::vector<BlockId>(&_memory) };
            for (auto const block: _tree.reversePostorder())
                if (auto const parent = _tree.immediateDominator(block))
                    ++children.begins[std::to_underlying(*parent) + 1];
            for (auto const index: std::views::iota(uint32_t { 0 }, count))
                children.begins[index + 1] += children.begins[index];
            children.blocks.resize(children.begins[count]);
            auto cursors =
                std::pmr::vector<uint32_t>(children.begins.begin(), children.begins.end() - 1, &_memory);
            for (auto const block: _tree.reversePostorder())
                if (auto const parent = _tree.immediateDominator(block))
                    children.blocks[cursors[std::to_underlying(*parent)]++] = block;
            return children;
        }

        /// The walk: each block in dominator-tree preorder, its facts undone when its subtree is done.
        [[nodiscard]] PassOutcome walk()
        {
            auto const children = childrenOf();
            auto stack = std::pmr::vector<Frame> { &_memory };
            stack.push_back(Frame { .block = BlockId { 0 }, .undoMark = _undo.size() });
            if (auto visited = visit(BlockId { 0 }); !visited)
                return visited;
            while (!stack.empty())
            {
                auto& frame = stack.back();
                auto const parent = std::to_underlying(frame.block);
                if (children.begins[parent] + frame.nextChild < children.begins[parent + 1])
                {
                    auto const child = children.blocks[children.begins[parent] + frame.nextChild++];
                    stack.push_back(Frame { .block = child, .undoMark = _undo.size() });
                    if (auto visited = visit(child); !visited)
                        return visited;
                    continue;
                }
                while (_undo.size() > frame.undoMark)
                {
                    _heads[std::to_underlying(_undo.back().index)] = _undo.back().head;
                    _undo.pop_back();
                }
                stack.pop_back();
            }
            return {};
        }

        /// Applies both rules to the checks of @p block, in order.
        [[nodiscard]] PassOutcome visit(BlockId block)
        {
            auto stretch = _stretches.entryOf(block);
            for (auto const index: _function.block(block).instructions)
            {
                if (auto stepped = _cancellation.step(1); !stepped)
                    return stepped;
                if (_function.instruction(index).operation == Operation::CheckMemory)
                    eliminate(index, stretch);
                stretch = intermediate::MemoryStretches::after(_function, index, stretch);
            }
            return {};
        }

        /// Applies rule (a), else rule (b), to the checkMemory at @p index, made in @p stretch; records it as
        /// a fact when it stays.
        void eliminate(uint32_t index, Stretch stretch)
        {
            auto const& instruction = _function.instruction(index);
            auto const operands = _function.operands(instruction.operands);
            auto const immediates = _function.immediates(instruction.immediates);
            auto const checked = operands[0];
            auto const base = operands[1];
            auto const memory = immediates[0];
            auto const offset = immediates[1];
            auto const size = immediates[2];
            auto const address = _function.operands(instruction.results)[0];
            if (auto const effective = provedByMinimum(checked, memory, offset, size))
            {
                rewrite(index,
                        Operation::InBoundsAddress,
                        std::array { base },
                        std::array { memory, *effective, size });
                _context.statistics.add(PassStatistic::ChecksProvedByMinimum);
                return;
            }
            auto const fact = factOf(checked, memory, stretch);
            if (fact)
                if (auto const delta = intermediate::guardCovers(fact->offset, fact->size, offset, size))
                {
                    if (*delta == 0)
                    {
                        _substitution.replace(address, fact->address);
                        _function.remove(index);
                        _context.statistics.add(PassStatistic::ChecksSharingDominatingAddress);
                        return;
                    }
                    rewrite(
                        index, Operation::GuardAddress, std::array { fact->address }, std::array { *delta });
                    _context.statistics.add(PassStatistic::ChecksMovedFromDominatingAddress);
                    return;
                }
            // Kept: a fact for what it dominates when it reaches further than the one in scope, or starts
            // earlier at the same end. Translation never emits an end past 2^64 - 1 (staticEndOverflows()).
            if (intermediate::staticEndOverflows(offset, size))
                return;
            auto const end = offset + size;
            if (fact
                && (end < fact->offset + fact->size
                    || (end == fact->offset + fact->size && offset >= fact->offset)))
                return;
            record(checked,
                   Fact { .memory = memory,
                          .stretch = stretch,
                          .address = address,
                          .offset = offset,
                          .size = size });
        }

        /// Rule (a): the offset of the access when @p checked is a constant and the access ends within the
        /// memory's declared minimum (intermediate::guardMemoryAccess() against it); none otherwise.
        [[nodiscard]] std::optional<uint64_t> provedByMinimum(ValueId checked,
                                                              uint64_t memory,
                                                              uint64_t offset,
                                                              uint64_t size) const noexcept
        {
            auto const minimums = _context.memoryMinimumBytes;
            if (memory >= minimums.size())
                return std::nullopt;
            auto const bits = constantOf(_function, _definitions, checked);
            if (!bits)
                return std::nullopt;
            return intermediate::guardMemoryAccess(
                _function.typeOf(checked), *bits, offset, size, minimums[static_cast<std::size_t>(memory)]);
        }

        /// The newest fact of @p checked for @p memory in @p stretch, among the first FactsReadPerIndex; the
        /// facts of a stretch are always the newest, so the first of another stretch ends the search.
        [[nodiscard]] std::optional<Fact> factOf(ValueId checked,
                                                 uint64_t memory,
                                                 Stretch stretch) const noexcept
        {
            auto position = _heads[std::to_underlying(checked)];
            for ([[maybe_unused]] auto const read: std::views::iota(0U, FactsReadPerIndex))
            {
                if (position == Unnumbered || _facts[position].stretch != stretch)
                    return std::nullopt;
                if (_facts[position].memory == memory)
                    return _facts[position];
                position = _facts[position].next;
            }
            return std::nullopt;
        }

        /// Makes @p fact the newest of @p checked, until the walk leaves the current block's subtree.
        void record(ValueId checked, Fact fact)
        {
            auto& head = _heads[std::to_underlying(checked)];
            _undo.push_back(Undo { .index = checked, .head = head });
            fact.next = head;
            head = static_cast<uint32_t>(_facts.size());
            _facts.push_back(fact);
        }

        /// Turns the instruction at @p index into @p operation over @p operands and @p immediates, keeping
        /// its result: a Pointer either way.
        void rewrite(uint32_t index,
                     Operation operation,
                     std::span<ValueId const> operands,
                     std::span<uint64_t const> immediates)
        {
            _builder.replaceOperands(index, operands);
            _builder.replaceImmediates(index, immediates);
            _function.instruction(index).operation = operation;
        }

        intermediate::Function& _function;
        PassContext& _context;
        std::pmr::memory_resource& _memory;
        intermediate::Builder _builder;
        intermediate::ControlFlowGraph _graph;
        intermediate::DominatorTree _tree;
        std::pmr::vector<uint32_t> _definitions;
        intermediate::MemoryStretches _stretches; ///< The stretches no memory motion interrupts.
        std::pmr::vector<uint32_t> _heads; ///< Per value, its newest fact as an index; Unnumbered if none.
        std::pmr::vector<Fact> _facts;     ///< Every fact recorded, in order; at most one per check.
        std::pmr::vector<Undo> _undo;      ///< The changes to _heads not yet undone, oldest first.
        Substitution _substitution;        ///< The checks that share a dominating address.
        CancellationPoll _cancellation;
    };

    /// Whether @p function has a checkMemory: none, and the pass has nothing to do.
    [[nodiscard]] bool hasMemoryCheck(intermediate::Function const& function) noexcept
    {
        for (auto const index: std::views::iota(uint32_t { 0 }, function.instructionCount()))
            if (function.instruction(index).operation == Operation::CheckMemory)
                return true;
        return false;
    }
} // namespace

PassOutcome eliminateBoundsChecks(intermediate::Function& function, PassContext& context)
{
    if (!hasMemoryCheck(function))
        return {};
    return Elimination { function, context }.run();
}

} // namespace isoline::compiler::optimization
