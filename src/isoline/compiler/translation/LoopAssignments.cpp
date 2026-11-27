// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/translation/LoopAssignments.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <utility>

#include <isoline/binary/Opcode.hpp>

namespace isoline::compiler::translation
{

namespace
{
    /// A link of a local's list of the open constructs whose sets hold it, deepest first.
    struct Membership
    {
        uint32_t depth = 0; ///< The open construct's depth, 1 for the outermost.
        uint32_t next = 0;  ///< The next link, or NoLink.
    };

    /// A link that names nothing.
    constexpr auto NoLink = std::numeric_limits<uint32_t>::max();

    /// @p size as a 32-bit index, which every list here stays below (Require).
    [[nodiscard]] uint32_t narrow(std::size_t size) noexcept
    {
        Require(size < NoLink);
        return static_cast<uint32_t>(size);
    }

    /// One open construct: its ordinal and the locals its set holds so far.
    struct Open
    {
        uint32_t ordinal = 0;              ///< Its ordinal.
        std::pmr::vector<uint32_t> locals; ///< Its set, unsorted while it is open.
    };

    /// The pass: open constructs as a stack, and per local the open constructs whose sets hold it.
    ///
    /// Locals join only the innermost open construct, directly or when a child closes, so each local's
    /// memberships are a stack too, deepest first, and "is it in the innermost set?" is one comparison.
    class Assignment
    {
      public:
        Assignment(uint64_t cap, std::pmr::memory_resource& memory, std::pmr::vector<uint32_t>& bodyIndices):
            _cap { cap },
            _memory { memory },
            _bodyIndices { bodyIndices },
            _head(&memory),
            _links(&memory),
            _freeLinks(&memory),
            _open(&memory)
        {
        }

        /// Opens the construct the instruction at @p bodyIndex starts.
        void open(uint32_t bodyIndex)
        {
            auto const ordinal = narrow(_bodyIndices.size());
            _bodyIndices.push_back(bodyIndex);
            ++_depth;
            if (_open.size() < _depth)
                _open.push_back(Open { .ordinal = 0, .locals = std::pmr::vector<uint32_t> { &_memory } });
            _open[_depth - 1].ordinal = ordinal;
        }

        /// Records an assignment of @p local in the innermost open construct, if any.
        [[nodiscard]] std::expected<void, CompileError> assign(uint32_t local)
        {
            if (_depth == 0)
                return {};
            return join(local);
        }

        /// Closes the innermost construct: sorts its set into the result and merges it into its parent.
        /// @return Nothing; or the refusal when the merge passes the cap.
        [[nodiscard]] std::expected<void, CompileError> close(std::pmr::vector<LoopAssignments::Run>& runs,
                                                              std::pmr::vector<uint32_t>& pool);

        /// How many constructs are open.
        [[nodiscard]] uint32_t depth() const noexcept { return _depth; }

      private:
        /// Adds @p local to the innermost open set unless it holds it.
        [[nodiscard]] std::expected<void, CompileError> join(uint32_t local)
        {
            if (local >= _head.size())
                _head.resize(std::size_t { local } + 1, NoLink);
            auto const head = _head[local];
            if (head != NoLink && _links[head].depth == _depth)
                return {};
            if (++_joinings > _cap)
                return std::unexpected { CompileError { .code = CompileErrorCode::TooManyBlockParameters,
                                                        .requested = _joinings,
                                                        .limit = _cap } };
            auto link = NoLink;
            if (_freeLinks.empty())
            {
                link = narrow(_links.size());
                _links.push_back({});
            }
            else
            {
                link = _freeLinks.back();
                _freeLinks.pop_back();
            }
            _links[link] = Membership { .depth = _depth, .next = head };
            _head[local] = link;
            _open[_depth - 1].locals.push_back(local);
            return {};
        }

        uint64_t _cap;
        std::pmr::memory_resource& _memory;
        std::pmr::vector<uint32_t>& _bodyIndices;
        std::pmr::vector<uint32_t> _head; ///< Per local, its deepest membership's link, or NoLink.
        std::pmr::vector<Membership> _links;
        std::pmr::vector<uint32_t> _freeLinks; ///< Links popped at a close, for the next joinings.
        std::pmr::vector<Open> _open;          ///< Per depth, the open construct; reused once closed.
        uint32_t _depth = 0;                   ///< How many constructs are open.
        uint64_t _joinings = 0;                ///< Every joining so far, direct or merged.
    };

    std::expected<void, CompileError> Assignment::close(std::pmr::vector<LoopAssignments::Run>& runs,
                                                        std::pmr::vector<uint32_t>& pool)
    {
        Require(_depth > 0);
        auto& closing = _open[_depth - 1];
        std::ranges::sort(closing.locals);
        runs[closing.ordinal] =
            LoopAssignments::Run { .begin = narrow(pool.size()), .count = narrow(closing.locals.size()) };
        pool.insert(pool.end(), closing.locals.begin(), closing.locals.end());
        // Each local of the closing set has it as its deepest membership: drop that, then join the
        // parent, now the innermost.
        for (auto const local: closing.locals)
        {
            auto const link = _head[local];
            Require(link != NoLink && _links[link].depth == _depth);
            _head[local] = _links[link].next;
            _freeLinks.push_back(link);
        }
        --_depth;
        // The parent's set is another vector: joining it leaves the closing one, kept for its capacity.
        if (_depth > 0)
            for (auto const local: closing.locals)
                if (auto joined = join(local); !joined)
                    return joined;
        closing.locals.clear();
        return {};
    }
} // namespace

LoopAssignments::LoopAssignments(std::pmr::memory_resource& memory):
    _bodyIndices(&memory), _runs(&memory), _locals(&memory)
{
}

std::span<uint32_t const> LoopAssignments::locals(uint32_t ordinal) const noexcept
{
    Require(ordinal < _runs.size());
    auto const run = _runs[ordinal];
    return std::span { _locals }.subspan(run.begin, run.count);
}

uint32_t LoopAssignments::bodyIndex(uint32_t ordinal) const noexcept
{
    Require(ordinal < _bodyIndices.size());
    return _bodyIndices[ordinal];
}

uint32_t LoopAssignments::firstAtOrAfter(uint32_t bodyIndex) const noexcept
{
    return static_cast<uint32_t>(std::ranges::lower_bound(_bodyIndices, bodyIndex) - _bodyIndices.begin());
}

std::expected<LoopAssignments, CompileError> assignedLocalsPerLoop(std::span<binary::Instruction const> body,
                                                                   uint64_t maxTotalBlockParameters,
                                                                   std::pmr::memory_resource& memory)
{
    auto result = LoopAssignments { memory };
    auto pass = Assignment { maxTotalBlockParameters, memory, result._bodyIndices };
    for (auto const [index, instruction]: core::views::enumerate(body))
    {
        switch (instruction.opcode)
        {
            case binary::Opcode::Block:
            case binary::Opcode::Loop:
            case binary::Opcode::If:
            case binary::Opcode::TryTable: // refused by translation, but its end closes it
                pass.open(narrow(index));
                result._runs.emplace_back();
                break;
            case binary::Opcode::LocalSet:
            case binary::Opcode::LocalTee:
                if (auto assigned = pass.assign(instruction.a); !assigned)
                    return std::unexpected { assigned.error() };
                break;
            case binary::Opcode::End:
                // The body's final end closes the function, which is no construct.
                if (pass.depth() > 0)
                    if (auto closed = pass.close(result._runs, result._locals); !closed)
                        return std::unexpected { closed.error() };
                break;
            default: break;
        }
    }
    return result;
}

} // namespace isoline::compiler::translation
