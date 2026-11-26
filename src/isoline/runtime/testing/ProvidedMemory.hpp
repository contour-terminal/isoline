// SPDX-License-Identifier: Apache-2.0
#pragma once

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
    uint64_t _guardBytes;
    std::function<GrowDecision(uint64_t newBytes)> _decide;
    Provision _provision = Provision::Available; ///< Whether the one memory was handed out.
};

} // namespace isoline::runtime::testing
