// SPDX-License-Identifier: Apache-2.0
#include <isoline/api/Store.hpp>

#include <core/Assert.hpp>
#include <core/Overloaded.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/ValueCheck.hpp>

namespace isoline::api
{

namespace
{
    /// A store's executor for @p engine: a case per ExecutionEngine and no default, so that -Wswitch
    /// names this place when an engine is added.
    [[nodiscard]] std::unique_ptr<runtime::ICodeExecutor> makeExecutor(ExecutionEngine engine)
    {
        switch (engine)
        {
            case ExecutionEngine::Reference: return std::make_unique<interpreter::ReferenceInterpreter>();
        }
        // Every ExecutionEngine is a case above.
        core::unreachable(); // uncovered(unknown-engine)
    } // uncovered(unknown-engine)

    /// Why the host may not define an entity of a value of @p type, if it may not: a concrete heap names
    /// a type index only a module's own types may name, and bot is the validator's own, no module's.
    /// @return The reason, or nothing when @p type is definable.
    [[nodiscard]] std::optional<InvalidTypeReason> invalidReasonOf(ValueType const& type) noexcept
    {
        if (type.kind != ValueKind::Ref)
            return std::nullopt;
        if (type.heap.kind == binary::AbstractHeapType::Concrete)
            return InvalidTypeReason::ConcreteTypeIndex;
        if (type.heap.kind == binary::AbstractHeapType::Bot)
            return InvalidTypeReason::BottomHeap;
        return std::nullopt;
    }

    /// Whether the host may define an entity of values of @p types: the one check every define makes of
    /// the value types it is given.
    /// @return Nothing, or InvalidType of the first type it may not define.
    [[nodiscard]] std::expected<void, DefineError> checkDefinable(std::span<ValueType const> types) noexcept
    {
        for (auto const& type: types)
            if (auto const reason = invalidReasonOf(type))
                return std::unexpected { DefineError { InvalidType { .reason = *reason } } };
        return {};
    }

    /// @copydoc checkDefinable(std::span<ValueType const>)
    [[nodiscard]] std::expected<void, DefineError> checkDefinable(FuncType const& type) noexcept
    {
        return checkDefinable(type.params).and_then([&] { return checkDefinable(type.results); });
    }

    /// @copydoc checkDefinable(std::span<ValueType const>)
    [[nodiscard]] std::expected<void, DefineError> checkDefinable(ValueType const& type) noexcept
    {
        return checkDefinable(std::span { &type, 1 });
    }
} // namespace

CallFailure CallError::failure() const noexcept
{
    return std::visit(core::Overloaded {
                          [](Trap const& trap) {
                              return trap.code == TrapCode::Interrupted ? CallFailure::Interrupted
                                                                        : CallFailure::Trap;
                          },
                          [](HostFailure const&) { return CallFailure::HostFailure; },
                          [](UncaughtException const&) { return CallFailure::UncaughtException; },
                          [](ValueMismatch const&) { return CallFailure::ArgumentMismatch; },
                          [](ForeignHandle const&) { return CallFailure::ForeignHandle; },
                          [](StoreDeleted const&) { return CallFailure::StoreDeleted; },
                      },
                      detail);
}

namespace
{
    /// The next serial number of a StoreId, from a counter of the process: a counter, not a resource,
    /// so it needs no seam to be tested. Relaxed is enough, as only uniqueness matters, which every
    /// fetch_add gives.
    /// @return A number no earlier call returned.
    [[nodiscard]] uint64_t nextStoreSerial() noexcept
    {
        static auto counter = std::atomic<uint64_t> { 1 };
        return counter.fetch_add(1, std::memory_order_relaxed);
    }
} // namespace

StoreId::StoreId() noexcept: _serial { nextStoreSerial() }
{
}

Store::Store(Engine const& engine, IMemoryAllocator& allocator, StoreOptions const& options):
    _identity { engine.config().identity() },
    _id { std::make_shared<StoreId>() },
    _runtime { options.limits.value_or(engine.config().store),
               runtime::StoreServices { .allocator = allocator,
                                        // The engine's guard, never sized from an override (P8: a guard
                                        // derived from maxMemoryBytes must check the effective limits).
                                        .requiredGuardBytes = engine.requiredGuardBytes(),
                                        .pollObserver = options.observer } },
    _executor { makeExecutor(engine.config().engine) },
    _alive { std::make_shared<std::monostate>() }
{
}

Store::~Store() = default;

std::shared_ptr<Interrupter> const& Store::interrupter() const noexcept
{
    return _runtime.interrupter();
}

std::expected<Instance, InstantiateError> Store::instantiate(Module const& module,
                                                             Imports const& imports) noexcept
{
    if (module.engine() != _identity)
        return std::unexpected { InstantiateError { EngineMismatch {} } };

    // Everything that allocates before the store changes: the externs, the definitions' lookup, the
    // host functions' entries. Running out of memory here refuses the instantiation, and the entries
    // reserved are dropped again.
    auto externs = std::vector<runtime::ExternValue> {};
    auto pending = std::vector<std::pair<std::size_t, PendingHost>> {};
    try
    {
        externs.reserve(module.imports().size());
        for (auto const [index, import]: core::views::enumerate(module.imports()))
        {
            auto const importIndex = static_cast<uint32_t>(index);
            auto const* definition = imports.find(import.module, import.name);
            if (definition == nullptr)
            {
                discardReservations();
                return std::unexpected { InstantiateError {
                    LinkError { LinkErrorCode::UnknownImport, importIndex } } };
            }
            if (auto const* other = std::get_if<Imports::StoreDefinition>(definition))
            {
                if (other->store != _id)
                {
                    discardReservations();
                    return std::unexpected { InstantiateError { ForeignImport { .index = importIndex } } };
                }
                externs.push_back(other->value);
                continue;
            }
            // A host function that cannot link is refused before any is added: it costs the store no
            // function and no type. Its type is closed, so the import's must equal it.
            auto const& host = std::get<Imports::HostDefinition>(*definition);
            auto const* const* required = std::get_if<FuncType const*>(&import.type);
            if (required == nullptr || **required != host.type)
            {
                discardReservations();
                return std::unexpected { InstantiateError {
                    LinkError { LinkErrorCode::IncompatibleImportType, importIndex } } };
            }
            pending.emplace_back(externs.size(), reserveHost(host.type, host.function));
            externs.push_back(runtime::ExternValue { .kind = ExternKind::Func, .address = 0 });
        }
    }
    catch (std::bad_alloc const&)
    {
        discardReservations();
        return std::unexpected { InstantiateError { HostMemoryExhausted {} } };
    }
    catch (std::length_error const&)
    {
        discardReservations();
        return std::unexpected { InstantiateError { HostMemoryExhausted {} } };
    }
    return link(module, std::move(externs), std::move(pending));
}

std::expected<Instance, InstantiateError> Store::instantiateByPosition(
    Module const& module, std::span<Extern const> imports) noexcept
{
    if (module.engine() != _identity)
        return std::unexpected { InstantiateError { EngineMismatch {} } };
    auto const expected = module.imports().size();
    if (imports.size() != expected)
        return std::unexpected { InstantiateError {
            ImportCountMismatch { .expected = static_cast<uint32_t>(expected),
                                  // Saturated: no span of externs that many long fits a host's memory anyway.
                                  .given = static_cast<uint32_t>(std::min<std::size_t>(
                                      imports.size(), std::numeric_limits<uint32_t>::max())) } } };
    // Every extern names an entity of its store already: there is nothing to reserve, so the only
    // allocation before the store changes is the externs' own.
    auto externs = std::vector<runtime::ExternValue> {};
    try
    {
        externs.reserve(imports.size());
    }
    catch (std::bad_alloc const&)
    {
        return std::unexpected { InstantiateError { HostMemoryExhausted {} } };
    }
    catch (std::length_error const&)
    {
        return std::unexpected { InstantiateError { HostMemoryExhausted {} } };
    }
    for (auto const [index, value]: core::views::enumerate(imports))
    {
        if (!std::visit([this](auto const& handle) { return handle.isOf(*_id); }, value))
            return std::unexpected { InstantiateError {
                ForeignImport { .index = static_cast<uint32_t>(index) } } };
        externs.push_back(externValueOf(value));
    }
    return link(module, std::move(externs), {});
}

std::expected<Instance, InstantiateError> Store::link(
    Module const& module,
    std::vector<runtime::ExternValue> externs,
    std::vector<std::pair<std::size_t, PendingHost>> pending) noexcept
{
    for (auto& [index, host]: pending)
    {
        auto const address = commitHost(host);
        if (!address)
        {
            discardReservations();
            return std::unexpected { InstantiateError { address.error() } };
        }
        externs[index].address = *address;
    }
    // runtime::instantiate() throws only from its first step, which changes nothing in the store: out of
    // memory there is a refusal. The host functions committed above stay, as define()d ones do.
    auto instantiated = std::optional<runtime::InstantiationOutcome> {};
    try
    {
        instantiated.emplace(
            runtime::instantiateReporting(_runtime, *_executor, module.validated(), externs));
    }
    catch (std::bad_alloc const&)
    {
        return std::unexpected { InstantiateError { HostMemoryExhausted {} } };
    }
    catch (std::length_error const&)
    {
        return std::unexpected { InstantiateError { HostMemoryExhausted {} } };
    }
    auto const refusedGrowth = instantiated->refusedGrowth;
    return std::move(instantiated->instance)
        .transform([this](uint32_t address) { return Instance { *this, address }; })
        .transform_error([this, &refusedGrowth](runtime::InstantiationError const& error) {
            return std::visit(
                core::Overloaded {
                    [](LinkError const& link) { return InstantiateError { link }; },
                    [](LimitExceeded const& exceeded) { return InstantiateError { exceeded }; },
                    [](AllocationFailure const& failure) { return InstantiateError { failure }; },
                    [this, &refusedGrowth](auto const& abrupt) {
                        return InstantiateError { callError(Abrupt { abrupt }, refusedGrowth) };
                    },
                },
                error);
        });
}

std::expected<Function, DefineError> Store::define(FuncType const& type,
                                                   std::shared_ptr<IHostFunction> const& function) noexcept
{
    Require(function != nullptr);
    if (auto const definable = checkDefinable(type); !definable)
        return std::unexpected { definable.error() };
    auto pending = std::optional<PendingHost> {};
    try
    {
        pending = reserveHost(type, function);
    }
    catch (std::bad_alloc const&)
    {
        discardReservations();
        return std::unexpected { DefineError { HostMemoryExhausted {} } };
    }
    catch (std::length_error const&)
    {
        discardReservations();
        return std::unexpected { DefineError { HostMemoryExhausted {} } };
    }
    auto const address = commitHost(*pending);
    if (!address)
    {
        discardReservations();
        return std::unexpected { DefineError { address.error() } };
    }
    return Function { handleLink(), *address, _runtime.functionType(*address) };
}

namespace
{
    /// Why the runtime store did not add a memory or table, as define's caller is told.
    [[nodiscard]] DefineError defineErrorOf(runtime::AllocationError const& error) noexcept
    {
        return std::visit([](auto const& alternative) { return DefineError { alternative }; }, error);
    }

    /// Calls @p add, which adds one entity to the store in a step that leaves it unchanged when the host
    /// runs out of memory, and reports running out as HostMemoryExhausted.
    /// @param add Adds the entity; may throw std::bad_alloc or std::length_error, and nothing else.
    /// @return What @p add returned, or HostMemoryExhausted.
    template <typename Add>
    [[nodiscard]] std::invoke_result_t<Add&> refusingHostMemory(Add& add) noexcept
    {
        try
        {
            return add();
        }
        catch (std::bad_alloc const&)
        {
            return std::unexpected { DefineError { HostMemoryExhausted {} } };
        }
        catch (std::length_error const&)
        {
            return std::unexpected { DefineError { HostMemoryExhausted {} } };
        }
    }

    /// Whether @p type is one custom-page-sizes admits for a memory, as the validator checks a module's.
    [[nodiscard]] bool hasValidPageSize(MemoryType const& type) noexcept
    {
        return std::ranges::find(binary::ValidPageSizesLog2, type.pageSizeLog2)
               != binary::ValidPageSizesLog2.end();
    }
} // namespace

std::expected<Memory, DefineError> Store::defineMemory(MemoryType const& type) noexcept
{
    if (!hasValidPageSize(type))
        return std::unexpected { DefineError { InvalidType { .reason = InvalidTypeReason::PageSize } } };
    // A module's own memory's path: the same charges and refusals, each before anything is allocated.
    auto add = [&] {
        return _runtime.addMemory(type)
            .transform([this](MemAddr address) { return Memory { handleLink(), address }; })
            .transform_error(defineErrorOf);
    };
    return refusingHostMemory(add);
}

std::expected<Table, DefineError> Store::defineTable(TableType const& type, Ref init) noexcept
{
    if (type.element.kind != ValueKind::Ref)
        return std::unexpected { DefineError {
            InvalidType { .reason = InvalidTypeReason::NonReferenceElement } } };
    if (auto const definable = checkDefinable(type.element); !definable)
        return std::unexpected { definable.error() };
    if (auto const checked = checkValue(init, type.element); !checked)
        return std::unexpected { DefineError { checked.error() } };
    // A module's own table's path: the same charges and refusals, each before anything is allocated.
    auto add = [&] {
        return _runtime.addTable(type, init)
            .transform([this](TableAddr address) { return Table { handleLink(), address }; })
            .transform_error(defineErrorOf);
    };
    return refusingHostMemory(add);
}

std::expected<Global, DefineError> Store::defineGlobal(GlobalType const& type, Value value) noexcept
{
    if (auto const definable = checkDefinable(type.type); !definable)
        return std::unexpected { definable.error() };
    if (auto const checked = checkValue(value, type.type); !checked)
        return std::unexpected { DefineError { checked.error() } };
    auto add = [&] -> std::expected<Global, DefineError> {
        return Global { handleLink(), _runtime.addGlobal(type, value) };
    };
    return refusingHostMemory(add);
}

std::expected<MemorySize, MemoryAccessError> Store::memorySize(Memory const& memory) const noexcept
{
    if (!memory.isOf(*_id))
        return std::unexpected { MemoryAccessError::ForeignHandle };
    auto const& instance = _runtime.memory(memory.address());
    auto const bytes = static_cast<uint64_t>(instance.bytes().size());
    return MemorySize { .pages = bytes / binary::pageSize(instance.type), .bytes = bytes };
}

std::expected<MemoryType, MemoryAccessError> Store::memoryType(Memory const& memory) const noexcept
{
    if (!memory.isOf(*_id))
        return std::unexpected { MemoryAccessError::ForeignHandle };
    auto const& instance = _runtime.memory(memory.address());
    auto type = instance.type;
    type.limits.min = static_cast<uint64_t>(instance.bytes().size()) / binary::pageSize(instance.type);
    return type;
}

std::expected<std::span<std::byte>, MemoryAccessError> Store::memoryRange(Memory const& memory,
                                                                          uint64_t offset,
                                                                          uint64_t length) noexcept
{
    if (!memory.isOf(*_id))
        return std::unexpected { MemoryAccessError::ForeignHandle };
    auto const bytes = _runtime.memory(memory.address()).bytes();
    if (length == 0)
        return std::span<std::byte> {};
    if (offset > bytes.size() || length > bytes.size() - offset)
        return std::unexpected { MemoryAccessError::OutOfBounds };
    return bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
}

std::expected<uint64_t, MemoryGrowthError> Store::growMemory(Memory const& memory,
                                                             uint64_t deltaPages) noexcept
{
    if (!memory.isOf(*_id))
        return std::unexpected { MemoryGrowthError { ForeignHandle {} } };
    // The guest's own path: charged against the store's limits, refused and recorded as memory.grow is.
    return _runtime.growMemory(memory.address(), deltaPages).transform_error([](Refusal refusal) {
        return MemoryGrowthError { refusal };
    });
}

std::expected<Value, GlobalAccessError> Store::globalValue(Global const& global) noexcept
{
    if (!global.isOf(*_id))
        return std::unexpected { GlobalAccessError::ForeignHandle };
    auto const& value = _runtime.global(global.address()).value;
    // The embedder holds it now: an exnref must survive later sweeps.
    _runtime.markEscaped(value);
    return value;
}

std::expected<void, GlobalAccessError> Store::setGlobal(Global const& global, Value value) noexcept
{
    if (!global.isOf(*_id))
        return std::unexpected { GlobalAccessError::ForeignHandle };
    auto& instance = _runtime.global(global.address());
    if (instance.type.mutability != binary::Mutability::Var)
        return std::unexpected { GlobalAccessError::Immutable };
    if (!checkValue(value, instance.type.type))
        return std::unexpected { GlobalAccessError::ValueMismatch };
    instance.value = value;
    return {};
}

std::expected<GlobalType, GlobalAccessError> Store::globalType(Global const& global) const noexcept
{
    if (!global.isOf(*_id))
        return std::unexpected { GlobalAccessError::ForeignHandle };
    return _runtime.global(global.address()).type;
}

std::expected<uint64_t, TableAccessError> Store::tableSize(Table const& table) const noexcept
{
    if (!table.isOf(*_id))
        return std::unexpected { TableAccessError::ForeignHandle };
    return uint64_t { _runtime.table(table.address()).elements.size() };
}

std::expected<Ref, TableAccessError> Store::tableGet(Table const& table, uint64_t index) noexcept
{
    if (!table.isOf(*_id))
        return std::unexpected { TableAccessError::ForeignHandle };
    auto const& elements = _runtime.table(table.address()).elements;
    if (index >= elements.size())
        return std::unexpected { TableAccessError::OutOfBounds };
    auto const& element = elements[static_cast<std::size_t>(index)];
    // The embedder holds it now: an exnref must survive later sweeps.
    _runtime.markEscaped(element);
    return element;
}

std::expected<void, TableAccessError> Store::tableSet(Table const& table, uint64_t index, Ref value) noexcept
{
    if (!table.isOf(*_id))
        return std::unexpected { TableAccessError::ForeignHandle };
    auto& instance = _runtime.table(table.address());
    if (index >= instance.elements.size())
        return std::unexpected { TableAccessError::OutOfBounds };
    if (!checkValue(value, instance.type.element))
        return std::unexpected { TableAccessError::ValueMismatch };
    instance.elements[static_cast<std::size_t>(index)] = value;
    return {};
}

std::expected<uint64_t, TableGrowthError> Store::growTable(Table const& table,
                                                           uint64_t delta,
                                                           Ref init) noexcept
{
    if (!table.isOf(*_id))
        return std::unexpected { TableGrowthError { ForeignHandle {} } };
    if (!checkValue(init, _runtime.table(table.address()).type.element))
        return std::unexpected { TableGrowthError { InitValueMismatch {} } };
    // The guest's own path: charged against the store's limits, refused as table.grow is. It throws
    // nothing but what an allocation inside it throws, which it reports as HostMemory.
    return _runtime.growTable(table.address(), delta, init).transform_error([](Refusal refusal) {
        return TableGrowthError { refusal };
    });
}

std::expected<TableType, TableAccessError> Store::tableType(Table const& table) const noexcept
{
    if (!table.isOf(*_id))
        return std::unexpected { TableAccessError::ForeignHandle };
    auto const& instance = _runtime.table(table.address());
    auto type = instance.type;
    type.limits.min = instance.elements.size();
    return type;
}

std::expected<Function, FunctionRefError> Store::function(Ref const& reference) const noexcept
{
    if (reference.kind == RefKind::Null)
        return std::unexpected { FunctionRefError::Null };
    // A host object's, an exception's, or any kind added later: only a funcref names a function.
    if (reference.kind != RefKind::Func)
        return std::unexpected { FunctionRefError::NotFunction };
    if (reference.address >= _runtime.functionCount())
        return std::unexpected { FunctionRefError::UnknownAddress };
    return Function { handleLink(), reference.address, _runtime.functionType(reference.address) };
}

std::expected<void, CallError> Store::call(Function const& function,
                                           std::span<Value const> args,
                                           std::span<Value> results) noexcept
{
    if (!function.isOf(*_id))
        return std::unexpected { CallError { .detail = ForeignHandle {}, .refusedGrowth = {} } };
    auto const invoked = runtime::invokeChecked(_runtime, *_executor, function.address(), args, results);
    if (invoked.outcome)
    {
        // The embedder holds them now: an exnref among them must survive later sweeps.
        for (auto const& value: results)
            _runtime.markEscaped(value);
        return {};
    }
    // A refused call ran nothing: what is wrong with it is the mismatch its checks found. A Function
    // names a function its store holds, so the refusal always names one.
    if (invoked.entry == runtime::Entry::Refused)
    {
        auto const& failure = std::get<HostFailure>(invoked.outcome.error());
        Require(failure.mismatch.has_value());
        return std::unexpected { CallError { .detail = *failure.mismatch,
                                             .refusedGrowth = invoked.refusedGrowth } };
    }
    return std::unexpected { callError(invoked.outcome.error(), invoked.refusedGrowth) };
}

Store const& Store::ownerOf(Extern const& value) noexcept
{
    return std::visit([](auto const& handle) -> Store const& { return handle._link.owner(); }, value);
}

runtime::ExternValue Store::externValueOf(Extern const& value) noexcept
{
    // Extern's alternatives follow ExternKind's order.
    return runtime::ExternValue {
        .kind = static_cast<ExternKind>(value.index()),
        .address = std::visit([](auto const& handle) -> uint32_t { return handle.address(); }, value)
    };
}

Imports::EntityType Store::entityTypeOf(runtime::ExternValue value) const
{
    switch (value.kind)
    {
        case ExternKind::Func: return _runtime.functionType(value.address);
        case ExternKind::Table: return _runtime.table(value.address).type;
        case ExternKind::Memory: return _runtime.memory(value.address).type;
        case ExternKind::Global: return _runtime.global(value.address).type;
        case ExternKind::Tag: break; // below the switch, so that every kind is a case
    }
    return _runtime.type(_runtime.tag(value.address).typeId);
}

std::expected<void, DefineError> Store::checkHostFunctionType(FuncType const& type) noexcept
{
    return checkDefinable(type);
}

Extern Store::externOf(runtime::ExternValue value) const noexcept
{
    switch (value.kind)
    {
        case ExternKind::Func:
            return Function { handleLink(), value.address, _runtime.functionType(value.address) };
        case ExternKind::Table: return Table { handleLink(), value.address };
        case ExternKind::Memory: return Memory { handleLink(), value.address };
        case ExternKind::Global: return Global { handleLink(), value.address };
        case ExternKind::Tag: break; // below the switch, so that every kind is a case
    }
    return Tag { handleLink(), value.address, _runtime.type(_runtime.tag(value.address).typeId) };
}

std::expected<void, ValueMismatch> Store::checkValue(Value const& value, ValueType const& type) const noexcept
{
    return runtime::checkValues(_runtime, std::span { &value, 1 }, std::span { &type, 1 });
}

Store::PendingHost Store::reserveHost(FuncType const& type, std::shared_ptr<IHostFunction> const& function)
{
    auto const entry =
        _hostFunctions.try_emplace(std::pair { static_cast<IHostFunction const*>(function.get()), type })
            .first;
    return PendingHost { .entry = entry, .type = type, .function = function };
}

std::expected<FuncAddr, LimitExceeded> Store::commitHost(PendingHost& pending) noexcept
{
    if (pending.entry->second)
        return *pending.entry->second;
    return _runtime.addHostFunction(std::move(pending.type), std::move(pending.function))
        .transform([&](FuncAddr address) {
            pending.entry->second = address;
            return address;
        });
}

void Store::discardReservations() noexcept
{
    std::erase_if(_hostFunctions, [](auto const& entry) { return !entry.second.has_value(); });
}

CallError Store::callError(Abrupt const& abrupt, GrowthRecord const& refusedGrowth) noexcept
{
    auto detail = std::visit(core::Overloaded {
                                 [](Trap const& trap) { return CallError::Detail { trap }; },
                                 [](HostFailure const& failure) { return CallError::Detail { failure }; },
                                 [this](Thrown const& thrown) {
                                     // A copy, so the slot stays reclaimable; what it refers to escapes.
                                     auto const& exception = _runtime.exception(thrown.exception);
                                     for (auto const& value: exception.payload)
                                         _runtime.markEscaped(value);
                                     return CallError::Detail { UncaughtException {
                                         .tag = exception.tag, .payload = exception.payload } };
                                 },
                             },
                             abrupt);
    return CallError { .detail = std::move(detail), .refusedGrowth = refusedGrowth };
}

Instance::Instance(Store& store, uint32_t address) noexcept:
    _store { &store }, _address { address }, _alive { store._alive }
{
}

std::optional<runtime::ExternValue> Instance::exported(std::string_view name, ExternKind kind) const noexcept
{
    // A deleted store names no export.
    if (!isAlive())
        return std::nullopt;
    for (auto const& [exportName, value]: _store->_runtime.moduleInstance(_address).exports)
        if (exportName == name)
            return value.kind == kind ? std::optional { value } : std::nullopt;
    return std::nullopt;
}

std::optional<Function> Instance::function(std::string_view name) const noexcept
{
    return exported(name, ExternKind::Func).transform([this](runtime::ExternValue value) {
        return std::get<Function>(_store->externOf(value));
    });
}

std::optional<Memory> Instance::memory(std::string_view name) const noexcept
{
    return exported(name, ExternKind::Memory).transform([this](runtime::ExternValue value) {
        return std::get<Memory>(_store->externOf(value));
    });
}

std::optional<Global> Instance::global(std::string_view name) const noexcept
{
    return exported(name, ExternKind::Global).transform([this](runtime::ExternValue value) {
        return std::get<Global>(_store->externOf(value));
    });
}

std::optional<Table> Instance::table(std::string_view name) const noexcept
{
    return exported(name, ExternKind::Table).transform([this](runtime::ExternValue value) {
        return std::get<Table>(_store->externOf(value));
    });
}

std::optional<Tag> Instance::tag(std::string_view name) const noexcept
{
    return exported(name, ExternKind::Tag).transform([this](runtime::ExternValue value) {
        return std::get<Tag>(_store->externOf(value));
    });
}

std::expected<Extern, ExportAccessError> Instance::exportAt(uint32_t index) const noexcept
{
    if (!isAlive())
        return std::unexpected { ExportAccessError::StoreDeleted };
    auto const& exports = _store->_runtime.moduleInstance(_address).exports;
    if (index >= exports.size())
        return std::unexpected { ExportAccessError::OutOfRange };
    return _store->externOf(exports[index].second);
}

std::expected<void, CallError> Instance::call(Function const& function,
                                              std::span<Value const> args,
                                              std::span<Value> results) const noexcept
{
    if (!isAlive())
        return std::unexpected { CallError { .detail = StoreDeleted {}, .refusedGrowth = {} } };
    return _store->call(function, args, results);
}

std::expected<std::span<std::byte>, MemoryAccessError> Instance::memoryRange(Memory const& memory,
                                                                             uint64_t address,
                                                                             uint64_t size) const noexcept
{
    if (!isAlive())
        return std::unexpected { MemoryAccessError::StoreDeleted };
    return _store->memoryRange(memory, address, size);
}

std::expected<MemorySize, MemoryAccessError> Instance::memorySize(Memory const& memory) const noexcept
{
    if (!isAlive())
        return std::unexpected { MemoryAccessError::StoreDeleted };
    return _store->memorySize(memory);
}

} // namespace isoline::api
