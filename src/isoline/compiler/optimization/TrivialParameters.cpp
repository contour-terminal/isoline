// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/optimization/PassTable.hpp>
#include <isoline/compiler/optimization/Rewrite.hpp>

namespace isoline::compiler::optimization
{

using intermediate::BlockId;
using intermediate::Function;
using intermediate::Unnumbered;
using intermediate::ValueId;

namespace
{
    /// How many outside values a group of parameters is passed.
    enum class Outside : uint8_t
    {
        None, ///< None yet.
        One,  ///< One, the group's replacement.
        Many, ///< Two or more: the group stays.
    };

    /// The pass over one function. The parameters of blocks other than the entry form a graph, each pointing
    /// at the values its edges pass it; a group of parameters strongly connected through it, passed one value
    /// from outside the group and otherwise only each other, all hold that value. Tarjan's walk finds the
    /// groups operands first, so each group is decided once, with its outside values already resolved.
    class TrivialParameters
    {
      public:
        TrivialParameters(Function& function, PassContext& context):
            _function { function },
            _places { parameterPlacesOf(function, context.memory) },
            _incoming { incomingEdgesOf(function, context.memory) },
            _substitution { function.valueCount(), context.memory },
            _order(function.valueCount(), Unnumbered, &context.memory),
            _lowest(function.valueCount(), 0, &context.memory),
            _group(function.valueCount(), Unnumbered, &context.memory),
            _fates(function.valueCount(), ParameterFate::Kept, &context.memory),
            _stack { &context.memory },
            _frames { &context.memory },
            _members { &context.memory },
            _poll { context }
        {
        }

        [[nodiscard]] PassOutcome run()
        {
            for (auto const blockIndex: std::views::iota(uint32_t { 1 }, _function.blockCount()))
            {
                auto const block = BlockId { blockIndex };
                for (auto const parameter: _function.operands(_function.block(block).parameters))
                    if (_order[std::to_underlying(parameter)] == Unnumbered)
                        if (auto walked = walkFrom(parameter); !walked)
                            return walked;
                if (auto polled = _poll.step(1); !polled)
                    return polled;
            }
            if (_substitution.isEmpty())
                return {};
            _substitution.applyTo(_function);
            dropParameters(_function, _fates);
            return {};
        }

      private:
        /// One frame of the walk: a parameter and how many of its edges it has followed.
        struct Frame
        {
            ValueId parameter;
            std::size_t edge;
        };

        /// Whether @p value is a parameter of a block other than the entry: a node of the graph.
        [[nodiscard]] bool isNode(ValueId value) const noexcept
        {
            auto const block = _places.block[std::to_underlying(value)];
            return block != Unnumbered && block != 0;
        }

        /// The edges into the block of parameter @p parameter.
        [[nodiscard]] std::span<IncomingEdge const> edgesOf(ValueId parameter) const noexcept
        {
            return _incoming.into(BlockId { _places.block[std::to_underlying(parameter)] });
        }

        /// The value edge @p edge passes parameter @p parameter.
        [[nodiscard]] ValueId argumentOf(ValueId parameter, IncomingEdge const& edge) const noexcept
        {
            auto const position = _places.position[std::to_underlying(parameter)];
            return _function.operands(
                intermediate::OperandRange { .begin = edge.arguments + position, .count = 1 })[0];
        }

        /// Enters @p parameter into the walk.
        void enter(ValueId parameter)
        {
            auto const index = std::to_underlying(parameter);
            _order[index] = _next;
            _lowest[index] = _next;
            ++_next;
            _stack.push_back(parameter);
            _frames.push_back(Frame { .parameter = parameter, .edge = 0 });
        }

        /// Tarjan's walk from @p root, on an explicit stack, polling the cancellation per edge followed and
        /// per parameter left.
        /// @return Nothing; or Cancelled.
        [[nodiscard]] PassOutcome walkFrom(ValueId root)
        {
            enter(root);
            while (!_frames.empty())
            {
                if (auto polled = _poll.step(1); !polled)
                    return polled;
                auto& frame = _frames.back();
                auto const parameter = frame.parameter;
                auto const index = std::to_underlying(parameter);
                auto const edges = edgesOf(parameter);
                if (frame.edge < edges.size())
                {
                    auto const argument = argumentOf(parameter, edges[frame.edge++]);
                    if (!isNode(argument))
                        continue;
                    auto const target = std::to_underlying(argument);
                    if (_order[target] == Unnumbered)
                        enter(argument);
                    else if (_group[target] == Unnumbered) // on the stack: in a group still open
                        _lowest[index] = std::min(_lowest[index], _order[target]);
                    continue;
                }
                _frames.pop_back();
                if (!_frames.empty())
                {
                    auto const parent = std::to_underlying(_frames.back().parameter);
                    _lowest[parent] = std::min(_lowest[parent], _lowest[index]);
                }
                if (_lowest[index] == _order[index])
                    closeGroup(parameter);
            }
            return {};
        }

        /// Pops the group @p root heads off the stack and decides it.
        void closeGroup(ValueId root)
        {
            _members.clear();
            auto const group = _groups++;
            while (true)
            {
                auto const member = _stack.back();
                _stack.pop_back();
                _group[std::to_underlying(member)] = group;
                _members.push_back(member);
                if (member == root)
                    break;
            }
            auto outside = Outside::None;
            auto replacement = ValueId {};
            for (auto const member: _members)
            {
                for (auto const& edge: edgesOf(member))
                {
                    auto const value = _substitution.resolve(argumentOf(member, edge));
                    if (isNode(value) && _group[std::to_underlying(value)] == group)
                        continue;
                    if (outside == Outside::None)
                    {
                        outside = Outside::One;
                        replacement = value;
                    }
                    else if (value != replacement)
                    {
                        outside = Outside::Many;
                        break;
                    }
                }
                if (outside == Outside::Many)
                    return;
            }
            if (outside != Outside::One)
                return; // passed nothing from outside: no edge reaches the group
            for (auto const member: _members)
            {
                _substitution.replace(member, replacement);
                _fates[std::to_underlying(member)] = ParameterFate::Dropped;
            }
        }

        Function& _function;
        ParameterPlaces _places;
        IncomingEdges _incoming;
        Substitution _substitution;
        std::pmr::vector<uint32_t> _order;  ///< Per value, when the walk entered it; Unnumbered before.
        std::pmr::vector<uint32_t> _lowest; ///< Per value, the lowest order it reaches within its open group.
        std::pmr::vector<uint32_t> _group;  ///< Per value, its group once closed; Unnumbered while open.
        std::pmr::vector<ParameterFate> _fates;
        std::pmr::vector<ValueId> _stack;   ///< The entered parameters whose group is still open.
        std::pmr::vector<Frame> _frames;    ///< The walk's own stack.
        std::pmr::vector<ValueId> _members; ///< The group being decided.
        uint32_t _next = 0;                 ///< The next order to give.
        uint32_t _groups = 0;               ///< How many groups are closed.
        CancellationPoll _poll;             ///< Polled per edge followed and per parameter left.
    };
} // namespace

PassOutcome removeTrivialParameters(Function& function, PassContext& context)
{
    return TrivialParameters { function, context }.run();
}

} // namespace isoline::compiler::optimization
