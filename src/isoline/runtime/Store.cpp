// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/Store.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/FloatEnvironment.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/ValueCheck.hpp>

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

    /// What a memory's or a table's size is charged against: one of the store's running totals, the
    /// limit it may reach, and what one unit (page or slot) charges.
    struct Budget
    {
        uint64_t& total;   ///< The running total, never above limit.
        uint64_t limit;    ///< The StoreLimits value it is charged against.
        uint64_t unitCost; ///< What one unit charges: a page's bytes, or one element.
    };

    /// What growing from @p oldUnits by @p delta units charges @p budget, if it may.
    /// @param oldUnits The current size, at most @p limit.
    /// @param delta The units to add.
    /// @return The charge; or TypeMaximum past @p limit, Limit past what is left of the budget.
    [[nodiscard]] std::expected<uint64_t, Refusal> charge(uint64_t oldUnits,
                                                          uint64_t delta,
                                                          uint64_t limit,
                                                          Budget const& budget) noexcept
    {
        if (delta > limit - oldUnits)
            return std::unexpected { Refusal::TypeMaximum };
        // delta <= limit <= 2^64 / unitCost, so nothing overflows.
        auto const added = delta * budget.unitCost;
        if (added > budget.limit - budget.total)
            return std::unexpected { Refusal::Limit };
        return added;
    }

    /// Grows @p elements by @p delta slots holding @p fill, and charges them to @p budget; leaves
    /// both unchanged on a refusal.
    /// @return Nothing, or why it was refused: TypeMaximum past @p limit, Limit past the budget,
    ///         HostMemory when allocation fails.
    [[nodiscard]] std::expected<void, Refusal> growElements(
        std::vector<Ref>& elements, uint64_t delta, uint64_t limit, Ref fill, Budget budget)
    {
        return charge(elements.size(), delta, limit, budget)
            .and_then([&](uint64_t added) -> std::expected<void, Refusal> {
                // Within limit, so the sum does not overflow.
                auto const units = elements.size() + delta;
                if (units > elements.max_size())
                    return std::unexpected { Refusal::HostMemory };
                try
                {
                    elements.resize(static_cast<std::size_t>(units), fill);
                }
                catch (std::bad_alloc const&)
                {
                    // uncovered-begin(bad-alloc)
                    return std::unexpected { Refusal::HostMemory };
                }
                // uncovered-end(bad-alloc)
                budget.total += added;
                return {};
            });
    }

    /// The refusals IMemoryAllocator::allocate() may report.
    constexpr auto AllocationRefusals =
        std::to_array({ Refusal::Embedder, Refusal::HostMemory, Refusal::Guard });

    /// The refusals ILinearMemory::grow() may report.
    constexpr auto GrowRefusals = std::to_array({ Refusal::Embedder, Refusal::HostMemory });

    /// @p refusal as the embedder's allocator or memory reported it, attributed to whom it belongs:
    /// one of @p documented stays as it is; any other (TypeMaximum, Limit) is the store's alone to
    /// decide, so a storage claiming it is refusing on its own account, which is Embedder.
    /// @param refusal What the allocator or the memory returned.
    /// @param documented The refusals it is documented to return.
    /// @return The refusal to report.
    [[nodiscard]] constexpr Refusal embedderRefusal(Refusal refusal,
                                                    std::span<Refusal const> documented) noexcept
    {
        return std::ranges::find(documented, refusal) != documented.end() ? refusal : Refusal::Embedder;
    }

    /// Records @p refusal in @p slot unless the scope recorded one of its kind already.
    void recordFirst(std::optional<Refusal>& slot, Refusal refusal) noexcept
    {
        if (!slot)
            slot = refusal;
    }

    /// The refusal to add a memory or a table of @p kind: a store limit as LimitExceeded of
    /// @p limit, every other refusal as an AllocationFailure.
    [[nodiscard]] AllocationError allocationError(Refusal refusal, ExternKind kind, Limit limit) noexcept
    {
        if (refusal == Refusal::Limit)
            return LimitExceeded { .limit = limit };
        return AllocationFailure { .kind = kind, .reason = refusal };
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

MemoryInstance::MemoryInstance(binary::MemoryType type, std::unique_ptr<ILinearMemory> memory):
    type { type }, _memory { std::move(memory) }
{
    Require(_memory != nullptr);
}

Store::Store(StoreLimits limits, StoreServices services):
    _allocator { &services.allocator },
    _requiredGuardBytes { services.requiredGuardBytes },
    _pollObserver { services.pollObserver },
    _interrupter { std::make_shared<Interrupter>() },
    _limits { limits },
    _growthRecord {}
{
}

TypeId Store::internType(binary::FuncType type)
{
    auto const named = [this](binary::ValueType const& value) {
        return !binary::isConcreteRef(value) || value.heap.typeIndex < _types.size();
    };
    Require(std::ranges::all_of(type.params, named) && std::ranges::all_of(type.results, named));
    if (auto const found = findType(type))
        return *found;
    auto const next = typeCount();
    Require(next < std::numeric_limits<TypeId>::max());
    auto const units = typeUnits(type);
    Require(units <= _limits.maxTypes - _typeUnits);
    auto const entry = _typeIds.emplace(std::move(type), next).first;
    _types.push_back(&entry->first);
    _typeUnits += units;
    return next;
}

uint64_t Store::typeUnits(binary::FuncType const& type) noexcept
{
    return 1 + uint64_t { type.params.size() } + uint64_t { type.results.size() };
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

std::expected<FuncAddr, LimitExceeded> Store::addHostFunction(binary::FuncType type,
                                                              std::shared_ptr<IHostFunction> function)
{
    Require(function != nullptr);
    if (!findType(type) && typeUnits(type) > _limits.maxTypes - used(Limit::Types))
        return std::unexpected { LimitExceeded { .limit = Limit::Types } };
    auto const typeId = internType(std::move(type));
    return append(_functions,
                  FunctionInstance {
                      .typeId = typeId, .moduleInstance = 0, .codeIndex = 0, .host = std::move(function) });
}

FuncAddr Store::addModuleFunction(TypeId typeId, uint32_t moduleInstance, uint32_t codeIndex)
{
    Require(typeId < _types.size());
    return append(
        _functions,
        FunctionInstance {
            .typeId = typeId, .moduleInstance = moduleInstance, .codeIndex = codeIndex, .host = {} });
}

std::expected<TableAddr, AllocationError> Store::addTable(binary::TableType type, Ref init)
{
    if (_tables.size() >= _limits.maxTables)
        return std::unexpected { LimitExceeded { .limit = Limit::Tables } };
    auto elements = std::vector<Ref> {};
    return growElements(elements,
                        type.limits.min,
                        slotLimit(type),
                        init,
                        Budget { .total = _tableElements, .limit = _limits.maxTableElements, .unitCost = 1 })
        .transform_error(
            [](Refusal refusal) { return allocationError(refusal, ExternKind::Table, Limit::TableElements); })
        .transform(
            [&] { return append(_tables, TableInstance { .type = type, .elements = std::move(elements) }); });
}

std::expected<MemAddr, AllocationError> Store::addMemory(binary::MemoryType type)
{
    Require(type.pageSizeLog2 < binary::PageSizeLog2Limit);
    if (_memories.size() >= _limits.maxMemories)
        return std::unexpected { LimitExceeded { .limit = Limit::Memories } };
    auto const pageBytes = binary::pageSize(type);
    auto const pages = pageLimit(type);
    auto const initialBytes =
        charge(0,
               type.limits.min,
               pages,
               Budget { .total = _memoryBytes, .limit = _limits.maxMemoryBytes, .unitCost = pageBytes });
    if (!initialBytes)
        return std::unexpected { allocationError(
            initialBytes.error(), ExternKind::Memory, Limit::MemoryBytes) };
    // pages * pageBytes fits a u64 (pageLimit), and the store's limit caps every memory together.
    auto const request = MemoryRequest {
        .type = type,
        .initialBytes = *initialBytes,
        .maximumBytes = std::min(pages, _limits.maxMemoryBytes / pageBytes) * pageBytes,
        .requiredGuardBytes = _requiredGuardBytes,
    };
    return _allocator->allocate(request)
        .transform_error([](Refusal refusal) { return embedderRefusal(refusal, AllocationRefusals); })
        .and_then([&](std::unique_ptr<ILinearMemory> memory)
                      -> std::expected<std::unique_ptr<ILinearMemory>, Refusal> {
            // The allocator's contract, not the guest's: a memory, of the size asked for.
            Require(memory != nullptr);
            Require(std::as_const(*memory).bytes().size() == request.initialBytes);
            if (memory->guardBytes() < request.requiredGuardBytes)
                return std::unexpected { Refusal::Guard };
            return memory;
        })
        .transform_error(
            [](Refusal refusal) { return allocationError(refusal, ExternKind::Memory, Limit::MemoryBytes); })
        .transform([&](std::unique_ptr<ILinearMemory> memory) {
            _memoryBytes += request.initialBytes;
            return append(_memories, MemoryInstance { type, std::move(memory) });
        });
}

GlobalAddr Store::addGlobal(binary::GlobalType type, Value value)
{
    return append(_globals, GlobalInstance { .type = type, .value = value });
}

std::expected<TagAddr, LimitExceeded> Store::addTag(TypeId typeId)
{
    Require(typeId < _types.size());
    if (_tags.size() >= _limits.maxTags)
        return std::unexpected { LimitExceeded { .limit = Limit::Tags } };
    return append(_tags, TagInstance { .typeId = typeId });
}

std::expected<ExnAddr, Trap> Store::addException(TagAddr tag, std::vector<Value> payload)
{
    Require(tag < _tags.size());
    // A host may allocate one, so its payload is checked as host results are, references included:
    // what a handler pushes is read by the tag's params, and no forged funcref or exnref may enter.
    Require(checkValues(*this, payload, type(_tags[tag].typeId).params).has_value());
    if (_exceptions.size() >= _limits.maxExceptions
        || payload.size() > _limits.maxExceptionValues - _exceptionValues)
        return trap(TrapCode::ExceptionLimitExceeded);
    _exceptionValues += payload.size();
    return append(_exceptions, ExceptionInstance { .tag = tag, .payload = std::move(payload) });
}

uint32_t Store::addModuleInstance(ModuleInstance instance)
{
    Require(_moduleInstances.size() < _limits.maxInstances);
    // A function's frame cost comes from its validated module's frame shapes, so only an instance of
    // no code may lack one.
    Require(instance.validated != nullptr || instance.module == nullptr || instance.module->code.empty());
    return append(_moduleInstances, std::move(instance));
}

std::expected<void, LimitExceeded> Store::checkInstanceDemand(InstanceDemand const& demand) const
{
    struct Claim
    {
        Limit limit;     ///< The limit it is checked against.
        uint64_t demand; ///< What the instance will add.
    };
    // In Limit order, so that the first limit a demand exceeds is the one reported.
    auto const claims = std::to_array<Claim>({
        { .limit = Limit::MemoryBytes, .demand = demand.memoryBytes },
        { .limit = Limit::TableElements, .demand = demand.tableElements },
        { .limit = Limit::Memories, .demand = demand.memories },
        { .limit = Limit::Tables, .demand = demand.tables },
        { .limit = Limit::Instances, .demand = 1 },
        { .limit = Limit::Types, .demand = demand.typeUnits },
        { .limit = Limit::Tags, .demand = demand.tags },
    });
    for (auto const& claim: claims)
    {
        // Every running total is charged within its limit, so the difference does not wrap.
        if (claim.demand > limitValue(_limits, claim.limit) - used(claim.limit))
            return std::unexpected { LimitExceeded { .limit = claim.limit } };
    }
    return {};
}

StoreLimits const& Store::limits() const noexcept
{
    return _limits;
}

uint64_t Store::used(Limit limit) const noexcept
{
    struct Usage
    {
        Limit limit;                                   ///< The limit; equal to the row's index.
        uint64_t (*held)(Store const& store) noexcept; ///< What the store holds of it.
    };
    // One row per Limit; the call depth is counted by the executor, not held here.
    static constexpr auto Usages = std::to_array<Usage>({
        { .limit = Limit::CallDepth, .held = [](Store const&) noexcept -> uint64_t { return 0; } },
        { .limit = Limit::StackValues,
          .held = [](Store const& store) noexcept -> uint64_t { return store._chargedStackValues; } },
        { .limit = Limit::NestedInvocations,
          .held = [](Store const& store) noexcept -> uint64_t { return store._invocationDepth; } },
        { .limit = Limit::MemoryBytes,
          .held = [](Store const& store) noexcept -> uint64_t { return store._memoryBytes; } },
        { .limit = Limit::TableElements,
          .held = [](Store const& store) noexcept -> uint64_t { return store._tableElements; } },
        { .limit = Limit::Memories,
          .held = [](Store const& store) noexcept -> uint64_t { return store._memories.size(); } },
        { .limit = Limit::Tables,
          .held = [](Store const& store) noexcept -> uint64_t { return store._tables.size(); } },
        { .limit = Limit::Instances,
          .held = [](Store const& store) noexcept -> uint64_t { return store._moduleInstances.size(); } },
        { .limit = Limit::Exceptions,
          .held = [](Store const& store) noexcept -> uint64_t { return store._exceptions.size(); } },
        { .limit = Limit::ExceptionValues,
          .held = [](Store const& store) noexcept -> uint64_t { return store._exceptionValues; } },
        { .limit = Limit::Types,
          .held = [](Store const& store) noexcept -> uint64_t { return store._typeUnits; } },
        { .limit = Limit::Tags,
          .held = [](Store const& store) noexcept -> uint64_t { return store._tags.size(); } },
        { .limit = Limit::Steps,
          .held = [](Store const& store) noexcept -> uint64_t { return store._steps; } },
    });
    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: Usages)
                if (static_cast<std::size_t>(row.limit) != index++)
                    return false;
            return index == static_cast<std::size_t>(LastLimit) + 1;
        }(),
        "Usages must hold one row per Limit, at the index of its enumerator");
    return Usages[static_cast<std::size_t>(limit)].held(*this);
}

std::expected<void, Trap> Store::checkInvocationNesting() const noexcept
{
    if (_invocationDepth >= _limits.maxNestedInvocations)
        return trap(TrapCode::CallStackExhausted);
    return {};
}

uint32_t Store::hostCallsInProgress() const noexcept
{
    return _hostCalls;
}

std::size_t Store::moduleInstanceCount() const noexcept
{
    return _moduleInstances.size();
}

std::size_t Store::functionCount() const noexcept
{
    return _functions.size();
}

bool Store::isLiveException(ExnAddr address) const noexcept
{
    return address < _exceptions.size();
}

std::size_t Store::memoryCount() const noexcept
{
    return _memories.size();
}

std::size_t Store::tableCount() const noexcept
{
    return _tables.size();
}

std::size_t Store::tagCount() const noexcept
{
    return _tags.size();
}

void Store::releaseTrailing(TableAddr firstTable, MemAddr firstMemory) noexcept
{
    Require(firstTable <= _tables.size() && firstMemory <= _memories.size());
    auto const tables = std::ranges::subrange { _tables.begin() + firstTable, _tables.end() };
    for (auto const& table: tables)
        _tableElements -= table.elements.size();
    _tables.erase(tables.begin(), tables.end());
    auto const memories = std::ranges::subrange { _memories.begin() + firstMemory, _memories.end() };
    for (auto const& memory: memories)
        _memoryBytes -= memory.bytes().size();
    _memories.erase(memories.begin(), memories.end());
}

GrowthRecord const& Store::growthRecord() const noexcept
{
    return _growthRecord;
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

TagInstance const& Store::tag(TagAddr address) const
{
    return at(_tags, address);
}

ExceptionInstance const& Store::exception(ExnAddr address) const
{
    return at(_exceptions, address);
}

ModuleInstance const& Store::moduleInstance(uint32_t address) const
{
    return at(_moduleInstances, address);
}

ModuleInstance& Store::moduleInstance(uint32_t address)
{
    return at(_moduleInstances, address);
}

std::expected<uint64_t, Refusal> Store::growMemory(MemAddr address, uint64_t deltaPages)
{
    auto& instance = memory(address);
    auto const pageBytes = binary::pageSize(instance.type);
    auto const oldPages = uint64_t { instance.bytes().size() / pageBytes };
    // Growing by nothing always succeeds and changes nothing, so neither the storage nor the embedder
    // is asked, and nothing is recorded.
    if (deltaPages == 0)
        return oldPages;
    auto const grown =
        charge(oldPages,
               deltaPages,
               pageLimit(instance.type),
               Budget { .total = _memoryBytes, .limit = _limits.maxMemoryBytes, .unitCost = pageBytes })
            .and_then([&](uint64_t added) {
                return instance.grow((oldPages + deltaPages) * pageBytes)
                    .transform_error([](Refusal refusal) { return embedderRefusal(refusal, GrowRefusals); })
                    .transform([&] {
                        _memoryBytes += added;
                        return oldPages;
                    });
            });
    if (!grown)
        recordFirst(_growthRecord.memory, grown.error());
    return grown;
}

std::expected<uint64_t, Refusal> Store::growTable(TableAddr address, uint64_t delta, Ref init)
{
    auto& instance = table(address);
    auto const oldSize = uint64_t { instance.elements.size() };
    auto const grown =
        growElements(instance.elements,
                     delta,
                     slotLimit(instance.type),
                     init,
                     Budget { .total = _tableElements, .limit = _limits.maxTableElements, .unitCost = 1 })
            .transform([oldSize] { return oldSize; });
    if (!grown)
        recordFirst(_growthRecord.table, grown.error());
    return grown;
}

RefusalScope::RefusalScope(Store& store) noexcept:
    _store { &store }, _outer { std::exchange(store._growthRecord, GrowthRecord {}) }
{
}

RefusalScope::~RefusalScope()
{
    _store->_growthRecord = _outer;
}

GrowthRecord const& RefusalScope::record() const noexcept
{
    return _store->_growthRecord;
}

InvocationScope::InvocationScope(Store& store) noexcept: _store { &store }
{
    Require(_store->checkInvocationNesting().has_value());
    // The outermost invocation's budget is fresh; nested ones share it.
    if (_store->_invocationDepth == 0)
        _store->_steps = 0;
    ++_store->_invocationDepth;
}

InvocationScope::~InvocationScope()
{
    --_store->_invocationDepth;
}

HostCallScope::HostCallScope(Store& store) noexcept: _store { &store }
{
    ++_store->_hostCalls;
}

HostCallScope::~HostCallScope()
{
    --_store->_hostCalls;
    establishFloatDefaults();
}

} // namespace isoline::runtime
