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

/// How far a provided memory's inaccessible region reaches: bytes().size() + guardBytes() fixed at
/// this, whatever the memory's size, as one mapping of reach bytes (Contour's, a reserved memory's) has.
struct ProvidedReach
{
    uint64_t bytes = 0; ///< The reach, from the memory's first byte.
};

/// An embedder's allocator, as Contour provides its own mapping: it hands out one memory, held in a
/// fixed buffer of objectBytes, behind which it declares a guard, and refuses every further request with
/// Embedder, as Contour's takeProvision() does. The memory never moves, so bytes().data() is stable
/// across grows, and every grow is the embedder's call (decide), within the buffer.
///
/// The guard is declared, never held: nothing reads past bytes(), and the store only checks what the
/// memory declares (memoryRequirementsFor()), so a test may declare a guard of gibibytes. It is either a
/// guard of fixed size behind the bytes at every size, or what a fixed reach leaves behind them
/// (ProvidedReach).
///
/// One memory is all a store on it can hold: a second instance with a memory, a module of several
/// memories, or a component whose core instances each define one fails to instantiate with
/// AllocationFailure{Memory, Embedder}. An embedder like it gives each store its own allocator.
class ProvidedMemoryAllocator final: public IMemoryAllocator
{
  public:
    /// @param objectBytes The most bytes the memory may hold: its buffer.
    /// @param guardBytes The guard the memory declares after its bytes, at every size.
    /// @param decide Decides each grow, given the new size in bytes; asked before the buffer's bound is
    ///               checked, so that it sees every grow the store passes on.
    ProvidedMemoryAllocator(uint64_t objectBytes,
                            uint64_t guardBytes,
                            std::function<GrowDecision(uint64_t newBytes)> decide):
        _objectBytes { objectBytes },
        _guard { .shape = GuardShape::FollowsTheBytes, .bytes = guardBytes },
        _decide { std::move(decide) }
    {
    }

    /// @param objectBytes The most bytes the memory may hold: its buffer.
    /// @param reach How far its inaccessible region reaches: the guard it declares is reach.bytes minus its
    ///              size (0 once its size is past it).
    /// @param decide Decides each grow, as above.
    ProvidedMemoryAllocator(uint64_t objectBytes,
                            ProvidedReach reach,
                            std::function<GrowDecision(uint64_t newBytes)> decide):
        _objectBytes { objectBytes },
        _guard { .shape = GuardShape::EndsAtTheReach, .bytes = reach.bytes },
        _decide { std::move(decide) }
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
    /// How the declared guard follows the memory's size.
    enum class GuardShape : uint8_t
    {
        FollowsTheBytes, ///< The same guard behind the bytes at every size.
        EndsAtTheReach,  ///< Whatever a fixed reach leaves behind the bytes.
    };

    /// The declared guard: its shape, and the guard (FollowsTheBytes) or the reach (EndsAtTheReach).
    struct DeclaredGuard
    {
        GuardShape shape = GuardShape::FollowsTheBytes; ///< How it follows the size.
        uint64_t bytes = 0;                             ///< The guard or the reach.

        /// The guard behind @p size bytes.
        [[nodiscard]] uint64_t behind(uint64_t size) const noexcept
        {
            return shape == GuardShape::FollowsTheBytes ? bytes : bytes - std::min(bytes, size);
        }
    };

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
            _buffer(static_cast<std::size_t>(allocator._objectBytes)),
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

        [[nodiscard]] uint64_t guardBytes() const noexcept override
        {
            return _allocator->_guard.behind(_size);
        }

        [[nodiscard]] std::expected<void, Refusal> grow(uint64_t newBytes) noexcept override
        {
            if (_allocator->_decide(newBytes) == GrowDecision::Refuse || newBytes > _allocator->_objectBytes)
                return std::unexpected { Refusal::Embedder };
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
    DeclaredGuard _guard;
    std::function<GrowDecision(uint64_t newBytes)> _decide;
    Provision _provision = Provision::Available; ///< Whether the one memory was handed out.
};

} // namespace isoline::runtime::testing
