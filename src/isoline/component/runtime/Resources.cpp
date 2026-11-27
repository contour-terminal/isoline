// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/runtime/Resources.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/HandleTable.hpp>

namespace isoline::component::runtime
{

using canonical::TrapCode;
using canonical::trapOf;

namespace
{

    /// How a trap names a resource type by its implementor, by ResourceImplementor alternative: the
    /// component-model suite's words for a guest's.
    constexpr auto ImplementorWords = std::to_array<std::string_view>({
        "guest-defined resource",
        "host-defined resource",
    });

    static_assert(ImplementorWords.size() == std::variant_size_v<ResourceImplementor>);

} // namespace

std::expected<ResourceTypeIndex, Trap> ResourceTypeTable::create(ResourceImplementor implementor)
{
    if (_types.size() >= _cap)
        return std::unexpected { trapOf(TrapCode::ResourceTypes, _cap) };
    _types.push_back(std::move(implementor));
    return ResourceTypeIndex { static_cast<uint32_t>(_types.size()) };
}

ResourceImplementor const& ResourceTypeTable::operator[](ResourceTypeIndex type) const noexcept
{
    Require(contains(type));
    return _types[type.value - 1];
}

uint32_t InstanceTables::add()
{
    _instances.emplace_back(*_budget, _maxIndex);
    return static_cast<uint32_t>(_instances.size() - 1);
}

ComponentInstanceState& InstanceTables::operator[](uint32_t instance) noexcept
{
    Require(instance < _instances.size());
    return _instances[instance];
}

TaskId TaskStack::begin()
{
    auto const id = TaskId { _next };
    _tasks.emplace_back(id, Task {});
    ++_next;
    return id;
}

Task* TaskStack::find(TaskId id) noexcept
{
    auto const found = std::ranges::lower_bound(_tasks, id, {}, &std::pair<TaskId, Task>::first);
    if (found == _tasks.end() || found->first != id)
        return nullptr;
    return &found->second;
}

std::expected<void, Trap> TaskStack::resolve(TaskId id)
{
    Require(!_tasks.empty() && _tasks.back().first == id);
    auto& task = _tasks.back().second;
    Require(task.state == Task::State::Initial);
    if (task.borrowCount > 0)
        return std::unexpected { trapOf(TrapCode::BorrowOutstanding) };
    task.state = Task::State::Resolved;
    return {};
}

void TaskStack::end(TaskId id) noexcept
{
    Require(!_tasks.empty() && _tasks.back().first == id);
    _tasks.pop_back();
}

std::expected<void, Trap> Subtask::addLender(InstanceTables& instances, Lender lender)
{
    auto handle = instances[lender.componentInstance].handles.get(lender.handleIndex);
    Require(handle.has_value() && std::holds_alternative<ResourceHandle>(handle->get()));
    lenders.push_back(lender);
    if (auto charged = instances.budget().charge(); !charged)
    {
        lenders.pop_back();
        return std::unexpected { std::move(charged.error()) };
    }
    ++std::get<ResourceHandle>(handle->get()).lendCount;
    return {};
}

void Subtask::deliverResolve(InstanceTables& instances) noexcept
{
    for (auto const& lender: lenders)
    {
        // A lent handle cannot be removed, so its place still holds it, however its table grew.
        auto handle = instances[lender.componentInstance].handles.get(lender.handleIndex);
        Require(handle.has_value());
        auto& lent = std::get<ResourceHandle>(handle->get());
        Require(lent.lendCount > 0);
        --lent.lendCount;
    }
    instances.budget().release(static_cast<uint32_t>(lenders.size()));
    lenders.clear();
    state = State::Returned;
}

ResourceTypeIndex ResourceBindings::operator[](types::ResourceTypeId resource) const noexcept
{
    auto const found = _types.find(resource);
    Require(found != _types.end());
    return found->second;
}

ResourceStore::~ResourceStore()
{
    destroyHostResources();
}

void ResourceStore::destroyHostResources() noexcept
{
    for (auto const instance: std::views::iota(std::size_t { 0 }, _instances.size()))
    {
        _instances[static_cast<uint32_t>(instance)].handles.forEachSlot([&](uint32_t, HandleEntry& entry) {
            auto* const handle = std::get_if<ResourceHandle>(&entry);
            if (handle == nullptr || handle->ownership != Ownership::Own)
                return;
            auto const* const host = std::get_if<HostImplementor>(&_types[handle->type]);
            if (host == nullptr)
                return;
            auto const rep = handle->rep;
            // The slot is freed in place (no free-list entry: the table is being torn down), so a
            // second teardown finds nothing.
            entry = FreeSlot {};
            if (host->destructor)
                host->destructor->destroy(rep);
        });
    }
}

Trap wrongHandleType(ResourceTypeTable const& types,
                     uint32_t index,
                     ResourceTypeIndex expected,
                     ResourceTypeIndex found)
{
    auto const expectedKind = types[expected].index();
    auto const foundKind = types[found].index();
    auto foundWords = std::string { ImplementorWords[foundKind] };
    if (foundKind == expectedKind)
        foundWords = "a different " + foundWords;
    return trapOf(TrapCode::WrongHandleType, index, ImplementorWords[expectedKind], foundWords);
}

std::expected<std::reference_wrapper<ResourceHandle>, Trap> resourceHandleAt(ResourceStore& resources,
                                                                             uint32_t instance,
                                                                             ResourceTypeIndex type,
                                                                             uint32_t index)
{
    auto entry = resources.instances()[instance].handles.get(index);
    if (!entry)
        return std::unexpected { std::move(entry.error()) };
    // Every entry get() returns is a resource handle until P12 adds the other kinds, which trap here.
    auto& handle = std::get<ResourceHandle>(entry->get());
    if (handle.type != type)
        return std::unexpected { wrongHandleType(resources.types(), index, type, handle.type) };
    return std::ref(handle);
}

HandleTable& ResourceOperations::table() noexcept
{
    return _resources->instances()[_instance].handles;
}

std::expected<ResourceValue, Trap> ResourceOperations::liftOwn(types::ResourceTypeId type, uint32_t index)
{
    auto const expected = (*_bindings)[type];
    auto found = resourceHandleAt(*_resources, _instance, expected, index);
    if (!found)
        return std::unexpected { std::move(found.error()) };
    auto const& handle = found->get();
    if (handle.lendCount != 0)
        return std::unexpected { trapOf(TrapCode::LendOutstanding) };
    if (handle.ownership != Ownership::Own)
        return std::unexpected { trapOf(TrapCode::OwnExpected, index) };
    auto const value = ResourceValue { .type = expected, .rep = handle.rep, .ownership = Ownership::Own };
    auto removed = table().remove(index);
    Require(removed.has_value());
    return value;
}

std::expected<ResourceValue, Trap> ResourceOperations::liftBorrow(types::ResourceTypeId type, uint32_t index)
{
    auto* const subtask = std::get_if<Subtask*>(&_scope);
    Require(subtask != nullptr && *subtask != nullptr);
    auto const expected = (*_bindings)[type];
    auto found = resourceHandleAt(*_resources, _instance, expected, index);
    if (!found)
        return std::unexpected { std::move(found.error()) };
    auto const rep = found->get().rep;
    if (auto lent = (*subtask)->addLender(_resources->instances(),
                                          Lender { .componentInstance = _instance, .handleIndex = index });
        !lent)
        return std::unexpected { std::move(lent.error()) };
    return ResourceValue { .type = expected, .rep = rep, .ownership = Ownership::Borrow };
}

std::expected<ResourceTypeIndex, Trap> ResourceOperations::checkLowered(types::ResourceTypeId type,
                                                                        ResourceValue value) const
{
    auto const expected = (*_bindings)[type];
    if (!_resources->types().contains(value.type) || value.type != expected)
        return std::unexpected { trapOf(TrapCode::WrongResource, value.rep) };
    return expected;
}

std::expected<uint32_t, Trap> ResourceOperations::lowerOwn(types::ResourceTypeId type, ResourceValue value)
{
    auto const expected = checkLowered(type, value);
    if (!expected)
        return std::unexpected { expected.error() };
    return table().add(ResourceHandle { .type = *expected, .rep = value.rep, .ownership = Ownership::Own });
}

std::expected<uint32_t, Trap> ResourceOperations::lowerBorrow(types::ResourceTypeId type, ResourceValue value)
{
    auto const* const taskId = std::get_if<TaskId>(&_scope);
    Require(taskId != nullptr);
    auto* const task = _resources->tasks().find(*taskId);
    Require(task != nullptr);
    auto const expected = checkLowered(type, value);
    if (!expected)
        return std::unexpected { expected.error() };
    // Into the instance that implements the resource, a borrow is its representation: no handle.
    if (auto const* guest = std::get_if<GuestImplementor>(&_resources->types()[*expected]);
        guest != nullptr && guest->componentInstance == _instance)
        return value.rep;
    auto const index = table().add(ResourceHandle {
        .type = *expected, .rep = value.rep, .ownership = Ownership::Borrow, .borrowScope = *taskId });
    if (index)
        ++task->borrowCount;
    return index;
}

} // namespace isoline::component::runtime
