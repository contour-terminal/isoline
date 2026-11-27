// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <isoline/component/canonical/GuestMemory.hpp>
#include <isoline/component/canonical/LiftLower.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::canonical::testing
{

/// One read or write a FakeGuestMemory saw.
struct MemoryAccess
{
    uint32_t address = 0; ///< Its first byte.
    uint64_t length = 0;  ///< Its bytes.

    /// Two accesses are equal when both members are.
    bool operator==(MemoryAccess const&) const = default;
};

/// One reallocate() a FakeGuestMemory saw.
struct Reallocation
{
    uint32_t old = 0;       ///< The block resized.
    uint32_t oldSize = 0;   ///< Its size.
    uint32_t alignment = 0; ///< The alignment asked.
    uint32_t newSize = 0;   ///< The size asked.
    uint32_t result = 0;    ///< The address returned.

    /// Two reallocations are equal when every member is.
    bool operator==(Reallocation const&) const = default;
};

/// How a FakeGuestMemory behaves.
struct FakeGuestMemoryOptions
{
    std::size_t size = 4096; ///< The memory's bytes at first.
    /// Where reallocate() starts handing out blocks: each is the next free address aligned as asked.
    uint32_t heapStart = 1024;
    /// The poll() that returns Interrupted, counting from 1; never when empty.
    std::optional<uint32_t> interruptAtPoll {};
};

/// An IGuestMemoryAccess over a std::vector<std::byte>, recording every access. Its reallocate() is a
/// bump allocator that grows the memory when a block passes its end, by moving it into a new, larger
/// vector: a view of the old memory kept past a reallocate() reads freed storage, which ASan reports.
class FakeGuestMemory final: public IGuestMemoryAccess
{
  public:
    /// A memory as @p options describe it, zero-filled.
    explicit FakeGuestMemory(FakeGuestMemoryOptions options = {}):
        _bytes(options.size), _next { options.heapStart }, _interruptAtPoll { options.interruptAtPoll }
    {
    }

    [[nodiscard]] std::expected<void, Trap> read(uint32_t address, std::span<std::byte> out) override
    {
        if (auto inRange = checkRange(address, out.size(), 1, _bytes.size()); !inRange)
            return inRange;
        _reads.push_back(MemoryAccess { .address = address, .length = out.size() });
        std::ranges::copy(std::span { _bytes }.subspan(address, out.size()), out.begin());
        return {};
    }

    [[nodiscard]] std::expected<void, Trap> write(uint32_t address, std::span<std::byte const> bytes) override
    {
        if (auto inRange = checkRange(address, bytes.size(), 1, _bytes.size()); !inRange)
            return inRange;
        _writes.push_back(MemoryAccess { .address = address, .length = bytes.size() });
        std::ranges::copy(bytes, std::span { _bytes }.subspan(address).begin());
        return {};
    }

    [[nodiscard]] std::expected<uint32_t, Trap> reallocate(uint32_t old,
                                                           uint32_t oldSize,
                                                           uint32_t alignment,
                                                           uint32_t newSize) override
    {
        auto const result = (uint64_t { _next } + alignment - 1) / alignment * alignment;
        auto const end = result + newSize;
        if (end > _bytes.size())
        {
            // A new vector, so the old storage is freed: what memory.grow may do to a real memory.
            auto grown = std::vector<std::byte>(end);
            std::ranges::copy(_bytes, grown.begin());
            _bytes = std::move(grown);
        }
        _next = static_cast<uint32_t>(end);
        _reallocations.push_back(Reallocation { .old = old,
                                                .oldSize = oldSize,
                                                .alignment = alignment,
                                                .newSize = newSize,
                                                .result = static_cast<uint32_t>(result) });
        return static_cast<uint32_t>(result);
    }

    [[nodiscard]] std::expected<void, Trap> poll() override
    {
        ++_polls;
        if (_interruptAtPoll && _polls == *_interruptAtPoll)
            return std::unexpected { trapOf(TrapCode::Interrupted) };
        return {};
    }

    [[nodiscard]] uint64_t size() override { return _bytes.size(); }

    /// The memory's bytes.
    [[nodiscard]] std::span<std::byte> bytes() noexcept { return _bytes; }

    /// Copies @p bytes to @p address, recording nothing: a test's setup.
    void put(uint32_t address, std::span<std::byte const> bytes)
    {
        std::ranges::copy(bytes, std::span { _bytes }.subspan(address).begin());
    }

    /// The reads so far.
    [[nodiscard]] std::vector<MemoryAccess> const& reads() const noexcept { return _reads; }

    /// The writes so far.
    [[nodiscard]] std::vector<MemoryAccess> const& writes() const noexcept { return _writes; }

    /// The reallocations so far.
    [[nodiscard]] std::vector<Reallocation> const& reallocations() const noexcept { return _reallocations; }

    /// The polls so far.
    [[nodiscard]] uint32_t polls() const noexcept { return _polls; }

  private:
    std::vector<std::byte> _bytes;
    uint32_t _next;
    std::optional<uint32_t> _interruptAtPoll;
    std::vector<MemoryAccess> _reads {};
    std::vector<MemoryAccess> _writes {};
    std::vector<Reallocation> _reallocations {};
    uint32_t _polls = 0;
};

/// A HandleOps over one table of handles, as definitions.py's Table holds them (slot 0 unused, every
/// new handle appended), each resource type mapped to the ResourceTypeIndex of the same number.
class FakeHandleOps final: public HandleOps
{
  public:
    [[nodiscard]] std::expected<ResourceValue, Trap> liftOwn(types::ResourceTypeId type,
                                                             uint32_t index) override
    {
        auto entry = find(type, index);
        if (entry)
            _handles[index].reset();
        return entry;
    }

    [[nodiscard]] std::expected<ResourceValue, Trap> liftBorrow(types::ResourceTypeId type,
                                                                uint32_t index) override
    {
        return find(type, index).transform([](ResourceValue value) {
            value.ownership = Ownership::Borrow;
            return value;
        });
    }

    [[nodiscard]] std::expected<uint32_t, Trap> lowerOwn(types::ResourceTypeId type,
                                                         ResourceValue value) override
    {
        return add(type, value, Ownership::Own);
    }

    [[nodiscard]] std::expected<uint32_t, Trap> lowerBorrow(types::ResourceTypeId type,
                                                            ResourceValue value) override
    {
        return add(type, value, Ownership::Borrow);
    }

    /// Adds a handle: a test's setup.
    /// @return Its index.
    uint32_t put(ResourceValue value)
    {
        _handles.emplace_back(value);
        return static_cast<uint32_t>(_handles.size() - 1);
    }

    /// The handle at @p index, if any.
    [[nodiscard]] std::optional<ResourceValue> at(uint32_t index) const
    {
        return index < _handles.size() ? _handles[index] : std::nullopt;
    }

  private:
    [[nodiscard]] std::expected<ResourceValue, Trap> find(types::ResourceTypeId type, uint32_t index) const
    {
        if (index >= _handles.size() || !_handles[index])
            return std::unexpected { trapOf(TrapCode::UnknownHandle, index) };
        if (_handles[index]->type.value != type.value)
            return std::unexpected { trapOf(
                TrapCode::WrongHandleType, index, type.value, _handles[index]->type.value) };
        return *_handles[index];
    }

    [[nodiscard]] std::expected<uint32_t, Trap> add(types::ResourceTypeId type,
                                                    ResourceValue value,
                                                    Ownership ownership)
    {
        _handles.emplace_back(ResourceValue {
            .type = ResourceTypeIndex { type.value }, .rep = value.rep, .ownership = ownership });
        return static_cast<uint32_t>(_handles.size() - 1);
    }

    /// The handles by index; slot 0 is never one.
    std::vector<std::optional<ResourceValue>> _handles = std::vector<std::optional<ResourceValue>>(1);
};

} // namespace isoline::component::canonical::testing
