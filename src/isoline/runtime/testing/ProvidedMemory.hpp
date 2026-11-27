// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>

namespace isoline::runtime::testing
{

/// What an embedder decides about a memory's growth.
enum class GrowDecision : uint8_t
{
    Refuse, ///< The memory stays as it is; memory.grow returns -1.
    Grant,  ///< The memory grows.
};

/// An embedder's allocator, as Contour provides its own mapping: it hands out one memory, held in a
/// fixed buffer of objectBytes followed by a guard it declares, and refuses every further request with
/// Embedder, as Contour's takeProvision() does. The memory never moves, so bytes().data() is stable
/// across grows, and every grow is the embedder's call (decide), within the buffer.
///
/// One memory is all a store on it can hold: a second instance with a memory, a module of several
/// memories, or a component whose core instances each define one fails to instantiate with
/// AllocationFailure{Memory, Embedder}. An embedder like it gives each store its own allocator.
class ProvidedMemoryAllocator final: public IMemoryAllocator
{
  public:
    /// @param objectBytes The most bytes the memory may hold: its buffer's accessible part.
    /// @param guardBytes The guard the memory declares after its bytes, also held in the buffer.
    /// @param decide Decides each grow, given the new size in bytes; asked before the buffer's bound is
    ///               checked, so that it sees every grow the store passes on.
    ProvidedMemoryAllocator(uint64_t objectBytes,
                            uint64_t guardBytes,
                            std::function<GrowDecision(uint64_t newBytes)> decide):
        _objectBytes { objectBytes }, _guardBytes { guardBytes }, _decide { std::move(decide) }
    {
    }

    /// Hands out the one memory, zeroed, unless it was handed out already or @p request's initial size
    /// does not fit the buffer.
    /// @param request What the memory is.
    /// @return The memory, or Embedder.
    [[nodiscard]] std::expected<std::unique_ptr<ILinearMemory>, Refusal> allocate(
        MemoryRequest const& request) noexcept override
    {
        if (_provision == Provision::Taken || request.initialBytes > _objectBytes)
            return std::unexpected { Refusal::Embedder };
        _provision = Provision::Taken;
        return std::make_unique<ProvidedMemory>(*this, request.initialBytes);
    }

  private:
    /// Whether the allocator's one memory is still to be handed out.
    enum class Provision : uint8_t
    {
        Available, ///< Not handed out yet.
        Taken,     ///< Handed out; every later request is refused.
    };

    /// The memory handed out: a fixed buffer, of which its first size bytes are accessible.
    class ProvidedMemory final: public ILinearMemory
    {
      public:
        ProvidedMemory(ProvidedMemoryAllocator& allocator, uint64_t size):
            _allocator { &allocator },
            _buffer(static_cast<std::size_t>(allocator._objectBytes + allocator._guardBytes)),
            _size { size }
        {
        }

        [[nodiscard]] std::span<std::byte> bytes() noexcept override
        {
            return std::span { _buffer }.first(static_cast<std::size_t>(_size));
        }

        [[nodiscard]] std::span<std::byte const> bytes() const noexcept override
        {
            return std::span { _buffer }.first(static_cast<std::size_t>(_size));
        }

        [[nodiscard]] uint64_t guardBytes() const noexcept override { return _allocator->_guardBytes; }

        /// The buffer's bytes past the accessible ones, before the guard: it is sized once, for the largest.
        [[nodiscard]] uint64_t tailBytes() const noexcept override
        {
            return _allocator->_objectBytes - _size;
        }

        /// Grows within the buffer, the embedder deciding; refuses with Guard a size whose tail the buffer's
        /// rest and the guard cannot hold together.
        [[nodiscard]] std::expected<void, Refusal> grow(GrowRequest const& request) noexcept override
        {
            auto const newBytes = request.newBytes;
            if (_allocator->_decide(newBytes) == GrowDecision::Refuse || newBytes > _allocator->_objectBytes)
                return std::unexpected { Refusal::Embedder };
            if (_allocator->_objectBytes - newBytes + _allocator->_guardBytes < request.requiredTailBytes)
                return std::unexpected { Refusal::Guard };
            // The buffer was zeroed whole, and nothing past _size was written since.
            _size = newBytes;
            return {};
        }

      private:
        ProvidedMemoryAllocator* _allocator;
        std::vector<std::byte> _buffer; ///< Sized once, zeroed, never resized: its data never moves.
        uint64_t _size;
    };

    uint64_t _objectBytes;
    uint64_t _guardBytes;
    std::function<GrowDecision(uint64_t newBytes)> _decide;
    Provision _provision = Provision::Available; ///< Whether the one memory was handed out.
};

/// An embedder's allocator whose memories each live in a buffer of exactly their size plus a slack: the slack
/// it is given at allocation, then a grow's own (growSlack), as an embedder that maps a memory's pages anew
/// at every grow places them. A memory keeps its slack readable past its end, with no guard, so a grow to a
/// size whose tail (GrowRequest::requiredTailBytes) is more than growSlack is refused with Guard, the memory
/// unchanged.
class ReallocatingMemoryAllocator final: public IMemoryAllocator
{
  public:
    /// @param allocationSlack The bytes kept past the end of a memory as it is allocated.
    /// @param growSlack The bytes kept past the end of a memory after each grow.
    ReallocatingMemoryAllocator(uint64_t allocationSlack, uint64_t growSlack):
        _allocationSlack { allocationSlack }, _growSlack { growSlack }
    {
    }

    /// Allocates @p request's initial bytes, zeroed, and the allocation slack after them; the store decides
    /// whether that tail is enough (MemoryRequest::requiredTailBytes).
    /// @param request What the memory is.
    /// @return The memory.
    [[nodiscard]] std::expected<std::unique_ptr<ILinearMemory>, Refusal> allocate(
        MemoryRequest const& request) noexcept override
    {
        return std::make_unique<ReallocatingMemory>(request.initialBytes, _allocationSlack, _growSlack);
    }

  private:
    /// The memory handed out: its bytes, then its slack, in a buffer replaced by every grow.
    class ReallocatingMemory final: public ILinearMemory
    {
      public:
        ReallocatingMemory(uint64_t size, uint64_t slack, uint64_t growSlack):
            _buffer(static_cast<std::size_t>(size + slack)), _size { size }, _growSlack { growSlack }
        {
        }

        [[nodiscard]] std::span<std::byte> bytes() noexcept override
        {
            return std::span { _buffer }.first(static_cast<std::size_t>(_size));
        }

        [[nodiscard]] std::span<std::byte const> bytes() const noexcept override
        {
            return std::span { _buffer }.first(static_cast<std::size_t>(_size));
        }

        [[nodiscard]] uint64_t guardBytes() const noexcept override { return 0; }

        [[nodiscard]] uint64_t tailBytes() const noexcept override { return _buffer.size() - _size; }

        /// Moves to a buffer of the new size and the grow slack; refuses with Guard, unchanged, when that
        /// slack is less than the tail the new size needs.
        [[nodiscard]] std::expected<void, Refusal> grow(GrowRequest const& request) noexcept override
        {
            if (_growSlack < request.requiredTailBytes)
                return std::unexpected { Refusal::Guard };
            auto moved = std::vector<std::byte>(static_cast<std::size_t>(request.newBytes + _growSlack));
            std::ranges::copy(bytes(), moved.begin());
            _buffer = std::move(moved);
            _size = request.newBytes;
            return {};
        }

      private:
        std::vector<std::byte> _buffer; ///< The bytes, then the slack.
        uint64_t _size;                 ///< The accessible bytes.
        uint64_t _growSlack;            ///< The slack a grow keeps.
    };

    uint64_t _allocationSlack;
    uint64_t _growSlack;
};

} // namespace isoline::runtime::testing
