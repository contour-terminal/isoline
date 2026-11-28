// SPDX-License-Identifier: Apache-2.0
#include <isoline/execution/CompiledExecutor.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/HostTarget.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>
#include <isoline/runtime/HostCall.hpp>
#include <isoline/runtime/RawValue.hpp>
#include <isoline/runtime/ValueCheck.hpp>

namespace isoline::execution
{

namespace
{
    /// The frame rule of a compiled backend, by the architecture it generates for.
    struct HostFrameRuleRow
    {
        compiler::machine::Architecture architecture; ///< The architecture.
        compiler::machine::FrameRule rule;            ///< Its backend's frame rule.
    };

    /// Every compiled backend's frame rule: x86-64's (P5); P6 adds arm64's.
    constexpr auto HostFrameRuleTable = std::to_array<HostFrameRuleRow>({
        { .architecture = compiler::machine::Architecture::X64, .rule = compiler::x86_64::X64FrameRule },
    });

    /// Whether @p stackAddress leaves at least @p bytes above @p floor.
    [[nodiscard]] constexpr bool hasRoom(std::uintptr_t stackAddress,
                                         std::uintptr_t floor,
                                         uint64_t bytes) noexcept
    {
        return stackAddress >= floor && stackAddress - floor >= bytes;
    }

    /// What generated code left pending, as the invocation's Abrupt.
    [[nodiscard]] runtime::Abrupt abruptOf(compiler::PendingAbrupt const& pending, ExecutorRecord& record)
    {
        if (pending.kind == compiler::PendingKind::Trap)
        {
            auto const subject = pending.subject == compiler::SubjectPresence::Present
                                     ? std::optional { static_cast<int64_t>(pending.subjectBits) }
                                     : std::nullopt;
            return runtime::Abrupt { runtime::Trap { pending.code, subject } };
        }
        Require(pending.kind == compiler::PendingKind::StubAbrupt && record.abrupt.has_value());
        auto const abrupt = *record.abrupt;
        record.abrupt.reset();
        return abrupt;
    }

    /// A refusal before anything ran: no poll, no charge.
    [[nodiscard]] std::unexpected<runtime::Abrupt> exhausted() noexcept
    {
        return std::unexpected { runtime::Abrupt {
            runtime::Trap { runtime::TrapCode::CallStackExhausted } } };
    }
} // namespace

std::optional<compiler::machine::FrameRule> hostFrameRule() noexcept
{
    auto const row = std::ranges::find(HostFrameRuleTable, HostArchitecture, &HostFrameRuleRow::architecture);
    if (row == HostFrameRuleTable.end())
        return std::nullopt;
    return row->rule;
}

CompiledExecutor::StoreBinding::StoreBinding(runtime::Store& store, CompiledExecutor& executor):
    store { &store },
    gate { store },
    record { .store = &store, .executor = &executor, .gate = &gate },
    state { .interruptFlag = store.interrupter()->flagForGeneratedCode(),
            .stubs = stubEntries().data(),
            .dispatch = &store.dispatchView(),
            .executorState = &record }
{
    coverHostFunctions(state, record);
}

std::expected<std::unique_ptr<CompiledExecutor>, ExecutorError> CompiledExecutor::create(
    CompiledExecutorConfig config, runtime::StoreLimits const& limits, IStackBounds const& stackBounds)
{
    auto const rule = hostFrameRule();
    if (!rule)
        return std::unexpected { ExecutorError::NoBackendForHost };
    if (config.mode == runtime::ExecutionMode::Deterministic
        && config.nativeStackBytes < requiredNativeStack(*rule, limits))
        return std::unexpected { ExecutorError::GrantBelowRequired };
    if (!stackBounds.lowestUsableAddress())
        return std::unexpected { ExecutorError::StackBoundsUnavailable };
    return std::unique_ptr<CompiledExecutor> { new CompiledExecutor { config, *rule, stackBounds } };
}

CompiledExecutor::CompiledExecutor(CompiledExecutorConfig config,
                                   compiler::machine::FrameRule rule,
                                   IStackBounds const& stackBounds) noexcept:
    _config { config }, _rule { rule }, _stackBounds { &stackBounds }
{
}

CompiledExecutor::~CompiledExecutor() = default;

runtime::ExecutionEngine CompiledExecutor::engine() const noexcept
{
    return runtime::ExecutionEngine::Compiled;
}

CompiledExecutor::StoreBinding& CompiledExecutor::bindingOf(runtime::Store& store)
{
    if (_binding == nullptr)
        _binding = std::make_unique<StoreBinding>(store, *this);
    // One store's state: the depth count, the lease and the scratch are that store's.
    Require(_binding->store == &store);
    return *_binding;
}

void CompiledExecutor::prepareInstance(runtime::Store& store, uint32_t moduleInstance) noexcept
{
    auto& binding = bindingOf(store);
    auto const& instance = store.moduleInstance(moduleInstance);
    // runtime::instantiate() checked the code's engine; the mode is the executor's to check.
    auto code = std::static_pointer_cast<CompiledModuleCode const>(instance.code);
    Require(code != nullptr && code->mode() == _config.mode);
    auto prepared = std::make_unique<InstanceBinding>();
    for (auto const address: instance.memories)
        prepared->memories.push_back(&store.memory(address).view());
    for (auto const address: instance.tables)
        prepared->tables.push_back(&store.table(address).view());
    for (auto const address: instance.globals)
        prepared->globals.push_back(&store.global(address).cell);
    prepared->context =
        compiler::InstanceContext { .memory0 =
                                        prepared->memories.empty() ? nullptr : prepared->memories.front(),
                                    .memories = prepared->memories.data(),
                                    .tables = prepared->tables.data(),
                                    .globals = prepared->globals.data(),
                                    .typeIds = instance.types.data(),
                                    .functions = instance.functions.data(),
                                    .moduleInstance = moduleInstance };
    // The instance's own functions, past its imports, run its code.
    auto const& offsets = code->compiled().functionOffsets;
    auto const imported = instance.functions.size() - offsets.size();
    for (auto const codeIndex: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(offsets.size())))
        store.bindDispatch(instance.functions[imported + codeIndex],
                           moduleInstance,
                           code->function(codeIndex),
                           &prepared->context);
    prepared->code = std::move(code);
    if (binding.instances.size() <= moduleInstance)
        binding.instances.resize(std::size_t { moduleInstance } + 1);
    binding.instances[moduleInstance] = std::move(prepared);
}

std::expected<void, runtime::Abrupt> CompiledExecutor::invoke(runtime::Store& store,
                                                              runtime::FuncAddr function,
                                                              std::span<runtime::Value const> args,
                                                              std::span<runtime::Value> results)
{
    return enter(store, function, args, results, nullptr);
}

std::expected<void, runtime::Abrupt> CompiledExecutor::enter(runtime::Store& store,
                                                             runtime::FuncAddr function,
                                                             std::span<runtime::Value const> args,
                                                             std::span<runtime::Value> results,
                                                             compiler::EntryTrampoline trampoline)
{
    // The invocation's scope, its nesting limit and its step budget, is the entry's to open.
    Require(store.invocationDepth() > 0);
    auto& binding = bindingOf(store);
    auto const& callee = store.function(function);
    auto const& type = store.type(callee.typeId);
    // The caller's to check: arguments of the function's types, references in this store.
    Require(runtime::checkValues(store, args, type.params).has_value());
    Require(results.size() == type.results.size());
    auto& gate = binding.gate;
    if (callee.isHost())
    {
        // The callee's entry in the reference's order: the FunctionEntry poll, then the host frame.
        auto steps = gate.reload();
        auto const polled = gate.poll(runtime::PollSite::FunctionEntry, steps);
        gate.commit(steps);
        if (!polled)
            return std::unexpected { runtime::Abrupt { polled.error() } };
        return runtime::callHostFunction(store, *this, callee, std::nullopt, args, results);
    }
    Require(callee.moduleInstance < binding.instances.size()
            && binding.instances[callee.moduleInstance] != nullptr);
    auto const& instance = *binding.instances[callee.moduleInstance];
    auto const& code = *instance.code;
    auto values = std::vector<runtime::RawValue>(std::max(type.params.size(), type.results.size()));
    for (auto const [index, argument]: core::views::enumerate(args))
        values[static_cast<std::size_t>(index)] = runtime::encodeValue(argument);
    auto& state = binding.state;
    auto& record = binding.record;
    coverHostFunctions(state, record);
    auto const lowest = _stackBounds->lowestUsableAddress();
    if (!lowest)
        return exhausted(); // No limit can be set without the thread's end: nothing ran.
    auto const stackAddress = currentStackAddress();
    // What the invocation saves on its own frame and both exits restore (Decision "Per-invocation state").
    auto const savedEntryStackPointer = state.entryStackPointer;
    auto const savedLimit = state.nativeStackLimit;
    auto const savedDepth = state.callDepthLeft;
    auto const savedPending = state.pending;
    auto const restore = [&] {
        state.entryStackPointer = savedEntryStackPointer;
        state.nativeStackLimit = savedLimit;
        state.pending = savedPending;
    };
    if (binding.invocations == 0)
    {
        // Only the outermost entry sets the limit and starts the depth count; a nested one carries on from
        // its caller's, so that depth spans every invocation in progress.
        auto const floor = *lowest + static_cast<std::uintptr_t>(StubReserve);
        if (_config.mode == runtime::ExecutionMode::Deterministic
            && !hasRoom(stackAddress, floor, _config.nativeStackBytes))
            return exhausted();
        state.nativeStackLimit = nativeStackLimitOf(stackAddress, _config.nativeStackBytes, *lowest);
        state.callDepthLeft = store.limits().maxCallDepth;
    }
    auto const& compiled = code.compiled();
    auto const codeIndex = callee.codeIndex;
    auto const entryBytes = EntryOverheadBytes + compiled.adapterAreaBytes.at(codeIndex) + StubReserve;
    if (!hasRoom(stackAddress, state.nativeStackLimit, entryBytes))
    {
        restore();
        state.callDepthLeft = savedDepth;
        return exhausted();
    }
    auto const entryDepth = state.callDepthLeft;
    record.lent = gate.lend();
    auto const entryValues = record.lent.stackValues;
    state.stepsLeft = record.lent.steps;
    state.stackValuesLeft = entryValues;
    state.pending = {};
    ++binding.invocations;
    auto const entry = trampoline != nullptr ? trampoline : code.trampoline();
    auto const status =
        entry(&state, &instance.context, code.adapter(codeIndex), code.function(codeIndex), values.data());
    --binding.invocations;
    if (status == std::to_underlying(compiler::EntryStatus::Pending))
    {
        // The frames the trap exit dropped gave nothing back: the entry's values are what is charged again,
        // and the depth is the entry's.
        state.stackValuesLeft = entryValues;
        state.callDepthLeft = entryDepth;
        gate.settle(record.lent,
                    runtime::Lease { .steps = state.stepsLeft, .stackValues = state.stackValuesLeft });
        auto const abrupt = abruptOf(state.pending, record);
        restore();
        return std::unexpected { abrupt };
    }
    Require(status == std::to_underlying(compiler::EntryStatus::Returned));
    Require(state.callDepthLeft == entryDepth);
    gate.settle(record.lent,
                runtime::Lease { .steps = state.stepsLeft, .stackValues = state.stackValuesLeft });
    restore();
    for (auto const [index, result]: core::views::enumerate(type.results))
        results[static_cast<std::size_t>(index)] =
            runtime::decodeValue(values[static_cast<std::size_t>(index)], result);
    return {};
}

} // namespace isoline::execution
