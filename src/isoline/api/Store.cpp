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
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
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

    /// Whether @p type names a concrete type index, among its params or results.
    [[nodiscard]] bool namesConcreteType(FuncType const& type) noexcept
    {
        return std::ranges::any_of(type.params, binary::isConcreteRef)
               || std::ranges::any_of(type.results, binary::isConcreteRef);
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

Store::Store(Engine const& engine, IMemoryAllocator& allocator, IPollObserver* observer):
    _identity { engine.config().identity() },
    _id { std::make_shared<StoreId>() },
    _runtime { engine.config().store,
               runtime::StoreServices { .allocator = allocator,
                                        .requiredGuardBytes = engine.requiredGuardBytes(),
                                        .pollObserver = observer } },
    _executor { makeExecutor(engine.config().engine) }
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
    Require(!namesConcreteType(type));
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
    return Function { *_id, *address, _runtime.functionType(*address) };
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

Instance::Instance(Store& store, uint32_t address) noexcept: _store { &store }, _address { address }
{
}

std::optional<runtime::ExternValue> Instance::exported(std::string_view name, ExternKind kind) const noexcept
{
    for (auto const& [exportName, value]: _store->_runtime.moduleInstance(_address).exports)
        if (exportName == name)
            return value.kind == kind ? std::optional { value } : std::nullopt;
    return std::nullopt;
}

std::optional<Function> Instance::function(std::string_view name) const noexcept
{
    return exported(name, ExternKind::Func).transform([this](runtime::ExternValue value) {
        return Function { *_store->_id, value.address, _store->_runtime.functionType(value.address) };
    });
}

std::optional<Memory> Instance::memory(std::string_view name) const noexcept
{
    return exported(name, ExternKind::Memory).transform([this](runtime::ExternValue value) {
        return Memory { *_store->_id, value.address };
    });
}

std::optional<TagAddr> Instance::tag(std::string_view name) const noexcept
{
    return exported(name, ExternKind::Tag).transform([](runtime::ExternValue value) {
        return value.address;
    });
}

std::expected<void, CallError> Instance::call(Function function,
                                              std::span<Value const> args,
                                              std::span<Value> results) const noexcept
{
    Require(function.isOf(*_store->_id));
    auto& runtime = _store->_runtime;
    auto const invoked =
        runtime::invokeChecked(runtime, *_store->_executor, function.address(), args, results);
    if (invoked.outcome)
    {
        // The embedder holds them now: an exnref among them must survive later sweeps.
        for (auto const& value: results)
            runtime.markEscaped(value);
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
    return std::unexpected { _store->callError(invoked.outcome.error(), invoked.refusedGrowth) };
}

std::expected<std::span<std::byte>, MemoryAccessError> Instance::memoryRange(Memory memory,
                                                                             uint64_t address,
                                                                             uint64_t size) const noexcept
{
    Require(memory.isOf(*_store->_id));
    auto const bytes = _store->_runtime.memory(memory.address()).bytes();
    if (size == 0)
        return std::span<std::byte> {};
    if (address > bytes.size() || size > bytes.size() - address)
        return std::unexpected { MemoryAccessError::OutOfBounds };
    return bytes.subspan(static_cast<std::size_t>(address), static_cast<std::size_t>(size));
}

MemorySize Instance::memorySize(Memory memory) const noexcept
{
    Require(memory.isOf(*_store->_id));
    auto const& instance = _store->_runtime.memory(memory.address());
    auto const bytes = static_cast<uint64_t>(instance.bytes().size());
    return MemorySize { .pages = bytes / binary::pageSize(instance.type), .bytes = bytes };
}

} // namespace isoline::api
