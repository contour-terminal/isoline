// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/Store.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::runtime
{

namespace
{
    using binary::ExternKind;
    using binary::Limits;

    /// The most units (pages or slots) a memory or table may reach: its maximum, capped by what its
    /// address type can index and by what a byte count can express.
    /// @param limits Its limits.
    /// @param addressReach The most units its address type can index.
    /// @param unitBytes The bytes one unit takes.
    /// @return The largest size it may grow to.
    [[nodiscard]] constexpr uint64_t unitLimit(Limits const& limits,
                                               uint64_t addressReach,
                                               uint64_t unitBytes) noexcept
    {
        auto const reach = std::min(addressReach, std::numeric_limits<uint64_t>::max() / unitBytes);
        return std::min(limits.max.value_or(reach), reach);
    }

    /// The most pages a memory may reach.
    [[nodiscard]] constexpr uint64_t pageLimit(binary::MemoryType const& type) noexcept
    {
        return unitLimit(type.limits, binary::maxPages(type), binary::pageSize(type));
    }

    /// The most slots a table may reach.
    [[nodiscard]] constexpr uint64_t slotLimit(binary::TableType const& type) noexcept
    {
        return unitLimit(type.limits, binary::maxTableSize(type), sizeof(Ref));
    }

    /// Resizes @p items to @p units units of @p unitBytes each, new items being @p fill, if the
    /// limit and the allocator admit it and the allocation succeeds; otherwise leaves it unchanged.
    template <typename T>
    [[nodiscard]] std::expected<void, AllocationFailure> resize(std::vector<T>& items,
                                                                uint64_t units,
                                                                uint64_t limit,
                                                                uint64_t unitBytes,
                                                                IMemoryAllocator const& allocator,
                                                                T const& fill,
                                                                ExternKind kind)
    {
        auto const failure = std::unexpected { AllocationFailure { .kind = kind } };
        if (units > limit || !allocator.mayHold(units * unitBytes))
            return failure;
        auto const count = units * unitBytes / sizeof(T);
        if (count > items.max_size())
            return failure;
        try
        {
            items.resize(static_cast<std::size_t>(count), fill);
        }
        catch (std::bad_alloc const&)
        {
            // uncovered-begin(bad-alloc)
            return failure;
        }
        // uncovered-end(bad-alloc)
        return {};
    }

    /// The instance at @p address of @p instances. Precondition (checked): it exists.
    template <typename Instances>
    [[nodiscard]] auto& at(Instances& instances, uint32_t address)
    {
        Require(address < instances.size());
        return instances[address];
    }

    /// Appends @p instance to @p instances.
    /// @return Its address.
    template <typename Instance>
    [[nodiscard]] uint32_t append(std::deque<Instance>& instances, Instance instance)
    {
        Require(instances.size() < std::numeric_limits<uint32_t>::max());
        instances.push_back(std::move(instance));
        return static_cast<uint32_t>(instances.size() - 1);
    }
} // namespace

Store::Store(IMemoryAllocator const& allocator, uint32_t maxCallDepth):
    _allocator { &allocator }, _maxCallDepth { maxCallDepth }
{
}

TypeId Store::internType(binary::FuncType type)
{
    auto const named = [this](binary::ValueType const& value) {
        return !binary::isConcreteRef(value) || value.heap.typeIndex < _types.size();
    };
    Require(std::ranges::all_of(type.params, named) && std::ranges::all_of(type.results, named));
    auto const next = typeCount();
    Require(next < std::numeric_limits<TypeId>::max());
    auto const [entry, added] = _typeIds.try_emplace(std::move(type), next);
    if (added)
        _types.push_back(&entry->first);
    return entry->second;
}

std::optional<TypeId> Store::findType(binary::FuncType const& type) const
{
    auto const entry = _typeIds.find(type);
    if (entry == _typeIds.end())
        return std::nullopt;
    return entry->second;
}

TypeId Store::typeCount() const noexcept
{
    return static_cast<TypeId>(_types.size());
}

binary::FuncType const& Store::type(TypeId id) const
{
    Require(id < _types.size());
    return *_types[id];
}

FuncAddr Store::addHostFunction(binary::FuncType type, HostCallback callback)
{
    Require(static_cast<bool>(callback));
    auto const typeId = internType(std::move(type));
    return append(_functions,
                  FunctionInstance {
                      .typeId = typeId, .moduleInstance = 0, .codeIndex = 0, .host = std::move(callback) });
}

FuncAddr Store::addModuleFunction(TypeId typeId, uint32_t moduleInstance, uint32_t codeIndex)
{
    Require(typeId < _types.size());
    return append(
        _functions,
        FunctionInstance {
            .typeId = typeId, .moduleInstance = moduleInstance, .codeIndex = codeIndex, .host = {} });
}

std::expected<TableAddr, AllocationFailure> Store::addTable(binary::TableType type, Ref init)
{
    auto elements = std::vector<Ref> {};
    return resize(
               elements, type.limits.min, slotLimit(type), sizeof(Ref), *_allocator, init, ExternKind::Table)
        .transform(
            [&] { return append(_tables, TableInstance { .type = type, .elements = std::move(elements) }); });
}

std::expected<MemAddr, AllocationFailure> Store::addMemory(binary::MemoryType type)
{
    Require(type.pageSizeLog2 < binary::PageSizeLog2Limit);
    auto bytes = std::vector<std::byte> {};
    return resize(bytes,
                  type.limits.min,
                  pageLimit(type),
                  binary::pageSize(type),
                  *_allocator,
                  std::byte {},
                  ExternKind::Memory)
        .transform(
            [&] { return append(_memories, MemoryInstance { .type = type, .bytes = std::move(bytes) }); });
}

GlobalAddr Store::addGlobal(binary::GlobalType type, Value value)
{
    return append(_globals, GlobalInstance { .type = type, .value = value });
}

uint32_t Store::addModuleInstance(ModuleInstance instance)
{
    return append(_moduleInstances, std::move(instance));
}

FunctionInstance const& Store::function(FuncAddr address) const
{
    return at(_functions, address);
}

FunctionInstance& Store::function(FuncAddr address)
{
    return at(_functions, address);
}

binary::FuncType const& Store::functionType(FuncAddr address) const
{
    return type(function(address).typeId);
}

TableInstance const& Store::table(TableAddr address) const
{
    return at(_tables, address);
}

TableInstance& Store::table(TableAddr address)
{
    return at(_tables, address);
}

MemoryInstance const& Store::memory(MemAddr address) const
{
    return at(_memories, address);
}

MemoryInstance& Store::memory(MemAddr address)
{
    return at(_memories, address);
}

GlobalInstance const& Store::global(GlobalAddr address) const
{
    return at(_globals, address);
}

GlobalInstance& Store::global(GlobalAddr address)
{
    return at(_globals, address);
}

ModuleInstance const& Store::moduleInstance(uint32_t address) const
{
    return at(_moduleInstances, address);
}

ModuleInstance& Store::moduleInstance(uint32_t address)
{
    return at(_moduleInstances, address);
}

int64_t Store::growMemory(MemAddr address, uint64_t deltaPages)
{
    auto& instance = memory(address);
    auto const pageBytes = binary::pageSize(instance.type);
    auto const oldPages = instance.bytes.size() / pageBytes;
    auto const limit = pageLimit(instance.type);
    if (deltaPages > limit - oldPages)
        return -1;
    return resize(instance.bytes,
                  oldPages + deltaPages,
                  limit,
                  pageBytes,
                  *_allocator,
                  std::byte {},
                  ExternKind::Memory)
        .transform([oldPages] { return static_cast<int64_t>(oldPages); })
        .value_or(-1);
}

int64_t Store::growTable(TableAddr address, uint64_t delta, Ref init)
{
    auto& instance = table(address);
    auto const oldSize = uint64_t { instance.elements.size() };
    auto const limit = slotLimit(instance.type);
    if (delta > limit - oldSize)
        return -1;
    return resize(
               instance.elements, oldSize + delta, limit, sizeof(Ref), *_allocator, init, ExternKind::Table)
        .transform([oldSize] { return static_cast<int64_t>(oldSize); })
        .value_or(-1);
}

uint32_t Store::maxCallDepth() const noexcept
{
    return _maxCallDepth;
}

} // namespace isoline::runtime
