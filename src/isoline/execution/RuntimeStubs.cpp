// SPDX-License-Identifier: Apache-2.0
#include <isoline/execution/RuntimeStubs.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/compiler/intermediate/Guards.hpp>
#include <isoline/numerics/Float.hpp>
#include <isoline/runtime/HostCall.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/RawValue.hpp>
#include <isoline/runtime/Segment.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::execution
{

using compiler::ExecutionState;
using compiler::InstanceContext;
using compiler::RuntimeStub;
using compiler::StubResult;

namespace
{
    /// What every stub's operand rows check: the boundary is words and pointers only (I8).
    template <typename Function>
    constexpr bool IsWordFunction = false;

    template <typename... Words>
    constexpr bool
        IsWordFunction<StubResult (*)(ExecutionState*, InstanceContext const*, Words...) noexcept> =
            (std::is_same_v<Words, uint64_t> && ...);

    static_assert(std::is_trivially_copyable_v<StubResult> && sizeof(StubResult) == 2 * sizeof(uint64_t),
                  "a stub returns two 64-bit words");
    static_assert(
        [] {
            for (auto const& row: RuntimeStubTable)
                if (!std::visit([](auto function) { return IsWordFunction<decltype(function)>; }, row.entry))
                    return false;
            return true;
        }(),
        "every stub takes the state, the context and 64-bit words, and returns a StubResult");

    /// One stub's call: the state, the record, and, for a stub whose row Settles, the lease settled while it
    /// works and lent again as it ends, however it ends (a stub that forgot would leave the store's budget
    /// Settled under generated code, which every later poll, charge and exit of it would count wrongly).
    class StubCall
    {
      public:
        StubCall(ExecutionState& state, RuntimeStub stub) noexcept:
            _state { &state },
            _record { static_cast<ExecutorRecord*>(state.executorState) },
            _settles { rowOf(stub).settles }
        {
            Require(_record != nullptr);
            if (_settles == Settles::Yes)
                _record->gate->settle(
                    _record->lent,
                    runtime::Lease { .steps = state.stepsLeft, .stackValues = state.stackValuesLeft });
        }

        StubCall(StubCall const&) = delete;
        StubCall(StubCall&&) = delete;
        StubCall& operator=(StubCall const&) = delete;
        StubCall& operator=(StubCall&&) = delete;

        ~StubCall()
        {
            if (_settles == Settles::No)
                return;
            _record->lent = _record->gate->lend();
            _state->stepsLeft = _record->lent.steps;
            _state->stackValuesLeft = _record->lent.stackValues;
        }

        /// The store.
        [[nodiscard]] runtime::Store& store() const noexcept { return *_record->store; }

        /// The instance whose code called the stub.
        [[nodiscard]] runtime::ModuleInstance& instance(InstanceContext const& context) const
        {
            return _record->store->moduleInstance(context.moduleInstance);
        }

        /// Polls at @p site through the gate, the budget settled: the store's own order, observer included.
        [[nodiscard]] std::expected<void, runtime::Trap> poll(runtime::PollSite site) const noexcept
        {
            Require(_settles == Settles::Yes);
            auto& gate = *_record->gate;
            auto steps = gate.reload();
            auto polled = gate.poll(site, steps);
            gate.commit(steps);
            return polled;
        }

        /// A granule poll for the bulk routines.
        [[nodiscard]] auto granulePoll() const noexcept
        {
            return [this]() noexcept {
                return poll(runtime::PollSite::BulkGranule);
            };
        }

        /// The executor's record.
        [[nodiscard]] ExecutorRecord& record() const noexcept { return *_record; }

        /// The stub did its work, with @p value.
        [[nodiscard]] static StubResult done(uint64_t value = 0) noexcept
        {
            return StubResult { .status = std::to_underlying(StubStatus::Continue), .value = value };
        }

        /// The stub ended with @p abrupt: recorded for the executor, and pending for generated code.
        [[nodiscard]] StubResult pending(runtime::Abrupt const& abrupt) const noexcept
        {
            _record->abrupt = abrupt;
            _state->pending = compiler::PendingAbrupt { .kind = compiler::PendingKind::StubAbrupt };
            return StubResult { .status = std::to_underlying(StubStatus::Pending), .value = 0 };
        }

        /// done(), or pending() with the trap of @p outcome.
        [[nodiscard]] StubResult finish(std::expected<void, runtime::Trap> const& outcome) const noexcept
        {
            if (!outcome)
                return pending(runtime::Abrupt { outcome.error() });
            return done();
        }

      private:
        ExecutionState* _state;
        ExecutorRecord* _record;
        Settles _settles;
    };

    /// The Ref a word of generated code's holds: kind | heap.kind << 8 | address << 32 (P5 Decision "Types").
    [[nodiscard]] runtime::Ref refOfWord(uint64_t word) noexcept
    {
        constexpr auto Byte = uint64_t { 0xff };
        return runtime::Ref {
            .kind = static_cast<runtime::RefKind>(word & Byte),
            .heap = binary::HeapType { .kind = static_cast<binary::AbstractHeapType>(
                                           (word >> compiler::intermediate::RefWordHeapKindShift) & Byte) },
            .address = static_cast<uint32_t>(word >> compiler::intermediate::RefWordAddressShift),
        };
    }

    /// The word generated code holds @p ref in: refOfWord()'s inverse.
    [[nodiscard]] uint64_t wordOfRef(runtime::Ref const& ref) noexcept
    {
        return uint64_t { std::to_underlying(ref.kind) }
               | (uint64_t { std::to_underlying(ref.heap.kind) }
                  << compiler::intermediate::RefWordHeapKindShift)
               | (uint64_t { ref.address } << compiler::intermediate::RefWordAddressShift);
    }

    /// What a refused grow returns: -1 as an I64, which translation narrows for a 32-bit memory or table.
    constexpr auto RefusedGrowth = ~uint64_t { 0 };

    /// The float a FloatRound operation rounds.
    enum class RoundedFloat : uint8_t
    {
        F32, ///< Its bits are an f32's, zero-extended.
        F64, ///< Its bits are an f64's.
    };

    /// One rounding FloatRound performs.
    struct FloatRoundRow
    {
        compiler::FloatRoundOperation operation; ///< The operation; equal to the row's index.
        RoundedFloat width;                      ///< Its float.
        numerics::Rounding rounding;             ///< Its rounding: the kernel ceil, floor, trunc or nearest.
    };

    /// Every FloatRound operation, in compiler::FloatRoundOperation's order.
    constexpr auto FloatRoundTable = std::to_array<FloatRoundRow>({
        { compiler::FloatRoundOperation::F32Ceil, RoundedFloat::F32, numerics::Rounding::Up },
        { compiler::FloatRoundOperation::F32Floor, RoundedFloat::F32, numerics::Rounding::Down },
        { compiler::FloatRoundOperation::F32Trunc, RoundedFloat::F32, numerics::Rounding::TowardZero },
        { compiler::FloatRoundOperation::F32Nearest, RoundedFloat::F32, numerics::Rounding::NearestEven },
        { compiler::FloatRoundOperation::F64Ceil, RoundedFloat::F64, numerics::Rounding::Up },
        { compiler::FloatRoundOperation::F64Floor, RoundedFloat::F64, numerics::Rounding::Down },
        { compiler::FloatRoundOperation::F64Trunc, RoundedFloat::F64, numerics::Rounding::TowardZero },
        { compiler::FloatRoundOperation::F64Nearest, RoundedFloat::F64, numerics::Rounding::NearestEven },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: FloatRoundTable)
                if (static_cast<std::size_t>(row.operation) != index++)
                    return false;
            return index == static_cast<std::size_t>(compiler::FloatRoundOperation::F64Nearest) + 1;
        }(),
        "FloatRoundTable must hold one row per FloatRoundOperation, at the index of its enumerator");

    /// Element @p index of @p items, which holds it (Require): the index spaces and segments generated code
    /// names, which validation bounded.
    template <typename Items>
    [[nodiscard]] auto& itemAt(Items& items, uint64_t index) noexcept
    {
        Require(index < items.size());
        return items[static_cast<std::size_t>(index)];
    }

    /// The memory at index @p memory of @p call's instance.
    [[nodiscard]] runtime::MemoryInstance& memoryAt(StubCall const& call,
                                                    InstanceContext const& context,
                                                    uint64_t memory)
    {
        return call.store().memory(itemAt(call.instance(context).memories, memory));
    }

    /// The table at index @p table of @p call's instance.
    [[nodiscard]] runtime::TableInstance& tableAt(StubCall const& call,
                                                  InstanceContext const& context,
                                                  uint64_t table)
    {
        return call.store().table(itemAt(call.instance(context).tables, table));
    }

    /// One host call's arguments and results as values: in cells of its own frame for a narrow call, on the
    /// heap past that, so that the calls a host nests never share them.
    class HostCallValues
    {
      public:
        /// The values of a call of @p arguments params and @p results results.
        /// @throws std::bad_alloc when a wide call's values do not fit the host's memory.
        HostCallValues(std::size_t arguments, std::size_t results):
            _arguments { arguments }, _results { results }
        {
            if (arguments + results > InlineValues)
                _wide.resize(arguments + results);
        }

        /// The arguments.
        [[nodiscard]] std::span<runtime::Value> arguments() noexcept { return all().first(_arguments); }

        /// The results.
        [[nodiscard]] std::span<runtime::Value> results() noexcept
        {
            return all().subspan(_arguments, _results);
        }

      private:
        /// How many values a call keeps in its own frame.
        static constexpr auto InlineValues = std::size_t { 16 };

        [[nodiscard]] std::span<runtime::Value> all() noexcept
        {
            return _wide.empty() ? std::span<runtime::Value> { _inline }
                                 : std::span<runtime::Value> { _wide };
        }

        std::size_t _arguments;
        std::size_t _results;
        std::array<runtime::Value, InlineValues> _inline {};
        std::vector<runtime::Value> _wide;
    };
} // namespace

void const* entryAddress(StubEntry const& entry) noexcept
{
    return std::visit([](auto function) { return reinterpret_cast<void const*>(function); }, entry);
}

std::span<void const* const> stubEntries() noexcept
{
    static auto const entries = [] {
        auto addresses = std::array<void const*, RuntimeStubTable.size()> {};
        for (auto const& row: RuntimeStubTable)
            addresses[static_cast<std::size_t>(row.stub)] = entryAddress(row.entry);
        return addresses;
    }();
    return entries;
}

void coverHostFunctions(ExecutionState& state, ExecutorRecord& record)
{
    auto const& store = *record.store;
    auto values = record.hostScratch.size();
    for (auto const address: std::views::iota(record.functionsCovered, store.functionCount()))
    {
        auto const& function = store.function(static_cast<runtime::FuncAddr>(address));
        if (!function.isHost())
            continue;
        auto const& type = store.type(function.typeId);
        values = std::max({ values, type.params.size(), type.results.size() });
    }
    if (values > record.hostScratch.size())
        record.hostScratch.resize(values);
    record.functionsCovered = store.functionCount();
    state.hostScratch = record.hostScratch.data();
    state.hostScratchCapacity = static_cast<uint32_t>(record.hostScratch.size());
}

uint32_t hostScratchValuesOf(runtime::Store const& store) noexcept
{
    auto values = std::size_t { 0 };
    for (auto const address: std::views::iota(std::size_t { 0 }, store.functionCount()))
    {
        auto const& function = store.function(static_cast<runtime::FuncAddr>(address));
        if (!function.isHost())
            continue;
        auto const& type = store.type(function.typeId);
        values = std::max({ values, type.params.size(), type.results.size() });
    }
    return static_cast<uint32_t>(values);
}

StubResult pollSlow(ExecutionState* state, InstanceContext const* /*context*/, uint64_t site) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::PollSlow };
    return call.finish(call.poll(static_cast<runtime::PollSite>(site)));
}

StubResult memoryGrow(ExecutionState* state,
                      InstanceContext const* context,
                      uint64_t memory,
                      uint64_t delta) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::MemoryGrow };
    auto const address = itemAt(call.instance(*context).memories, memory);
    // The views generated code reads update themselves (Store::growMemory()).
    return StubCall::done(call.store().growMemory(address, delta).value_or(RefusedGrowth));
}

StubResult memoryFill(ExecutionState* state,
                      InstanceContext const* context,
                      uint64_t memory,
                      uint64_t destination,
                      uint64_t value,
                      uint64_t count) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::MemoryFill };
    auto poll = call.granulePoll();
    return call.finish(runtime::fillItems(memoryAt(call, *context, memory).bytes(),
                                          destination,
                                          count,
                                          static_cast<std::byte>(value),
                                          runtime::TrapCode::OutOfBoundsMemoryAccess,
                                          poll));
}

StubResult memoryCopy(ExecutionState* state,
                      InstanceContext const* context,
                      uint64_t destinationMemory,
                      uint64_t sourceMemory,
                      uint64_t destination,
                      uint64_t source,
                      uint64_t count) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::MemoryCopy };
    auto poll = call.granulePoll();
    auto& to = memoryAt(call, *context, destinationMemory);
    auto const& from = memoryAt(call, *context, sourceMemory);
    return call.finish(runtime::copyItems(std::span<std::byte const> { from.bytes() },
                                          source,
                                          to.bytes(),
                                          destination,
                                          count,
                                          runtime::TrapCode::OutOfBoundsMemoryAccess,
                                          poll));
}

StubResult memoryInit(ExecutionState* state,
                      InstanceContext const* context,
                      uint64_t segment,
                      uint64_t memory,
                      uint64_t destination,
                      uint64_t source,
                      uint64_t count) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::MemoryInit };
    auto const& data = itemAt(call.instance(*context).data, segment);
    return call.finish(runtime::copySegment(data.value_or(std::span<std::byte const> {}),
                                            source,
                                            count,
                                            memoryAt(call, *context, memory).bytes(),
                                            destination,
                                            runtime::TrapCode::OutOfBoundsMemoryAccess,
                                            call.granulePoll()));
}

StubResult dataDrop(ExecutionState* state, InstanceContext const* context, uint64_t segment) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::DataDrop };
    itemAt(call.instance(*context).data, segment).reset();
    return StubCall::done();
}

StubResult tableGrow(ExecutionState* state,
                     InstanceContext const* context,
                     uint64_t table,
                     uint64_t initial,
                     uint64_t delta) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::TableGrow };
    auto const address = itemAt(call.instance(*context).tables, table);
    return StubCall::done(call.store().growTable(address, delta, refOfWord(initial)).value_or(RefusedGrowth));
}

StubResult tableFill(ExecutionState* state,
                     InstanceContext const* context,
                     uint64_t table,
                     uint64_t destination,
                     uint64_t value,
                     uint64_t count) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::TableFill };
    auto poll = call.granulePoll();
    return call.finish(runtime::fillItems(tableAt(call, *context, table).elements(),
                                          destination,
                                          count,
                                          refOfWord(value),
                                          runtime::TrapCode::OutOfBoundsTableAccess,
                                          poll));
}

StubResult tableCopy(ExecutionState* state,
                     InstanceContext const* context,
                     uint64_t destinationTable,
                     uint64_t sourceTable,
                     uint64_t destination,
                     uint64_t source,
                     uint64_t count) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::TableCopy };
    auto poll = call.granulePoll();
    auto& to = tableAt(call, *context, destinationTable);
    auto const& from = tableAt(call, *context, sourceTable);
    return call.finish(runtime::copyItems(std::span<runtime::Ref const> { from.elements() },
                                          source,
                                          to.elements(),
                                          destination,
                                          count,
                                          runtime::TrapCode::OutOfBoundsTableAccess,
                                          poll));
}

StubResult tableInit(ExecutionState* state,
                     InstanceContext const* context,
                     uint64_t segment,
                     uint64_t table,
                     uint64_t destination,
                     uint64_t source,
                     uint64_t count) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::TableInit };
    auto const& elements = itemAt(call.instance(*context).elements, segment);
    return call.finish(runtime::copySegment(elements ? std::span<runtime::Ref const> { *elements }
                                                     : std::span<runtime::Ref const> {},
                                            source,
                                            count,
                                            tableAt(call, *context, table).elements(),
                                            destination,
                                            runtime::TrapCode::OutOfBoundsTableAccess,
                                            call.granulePoll()));
}

StubResult elementDrop(ExecutionState* state, InstanceContext const* context, uint64_t segment) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::ElementDrop };
    itemAt(call.instance(*context).elements, segment).reset();
    return StubCall::done();
}

StubResult tableGet(ExecutionState* state,
                    InstanceContext const* context,
                    uint64_t table,
                    uint64_t index) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::TableGet };
    auto const elements = tableAt(call, *context, table).elements();
    if (index >= elements.size())
        return call.pending(runtime::Abrupt { runtime::Trap { runtime::TrapCode::OutOfBoundsTableAccess } });
    return StubCall::done(wordOfRef(elements[static_cast<std::size_t>(index)]));
}

StubResult tableSet(ExecutionState* state,
                    InstanceContext const* context,
                    uint64_t table,
                    uint64_t index,
                    uint64_t value) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::TableSet };
    auto const elements = tableAt(call, *context, table).elements();
    if (index >= elements.size())
        return call.pending(runtime::Abrupt { runtime::Trap { runtime::TrapCode::OutOfBoundsTableAccess } });
    elements[static_cast<std::size_t>(index)] = refOfWord(value);
    return StubCall::done();
}

StubResult callHost(ExecutionState* state, InstanceContext const* context, uint64_t function) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::CallHost };
    auto& store = call.store();
    // The callee's entry in the reference's order: the FunctionEntry poll, then the host frame
    // (runtime::callHostFunction() charges it, calls, and polls at HostReturn).
    if (auto const polled = call.poll(runtime::PollSite::FunctionEntry); !polled)
        return call.pending(runtime::Abrupt { polled.error() });
    auto const& host = store.function(static_cast<runtime::FuncAddr>(function));
    auto const& type = store.type(host.typeId);
    Require(std::max(type.params.size(), type.results.size()) <= state->hostScratchCapacity);
    try
    {
        auto values = HostCallValues { type.params.size(), type.results.size() };
        auto const scratch = std::span { state->hostScratch, state->hostScratchCapacity };
        for (auto const [index, parameter]: core::views::enumerate(type.params))
            values.arguments()[static_cast<std::size_t>(index)] =
                runtime::decodeValue(scratch[static_cast<std::size_t>(index)], parameter);
        auto const done = runtime::callHostFunction(store,
                                                    *call.record().executor,
                                                    host,
                                                    context->moduleInstance,
                                                    values.arguments(),
                                                    values.results());
        if (!done)
            return call.pending(done.error());
        // The host may have added a wider host function, which generated code may call next.
        coverHostFunctions(*state, call.record());
        for (auto const [index, result]: core::views::enumerate(values.results()))
            state->hostScratch[static_cast<std::size_t>(index)] = runtime::encodeValue(result);
    }
    catch (std::bad_alloc const&)
    {
        return call.pending(runtime::Abrupt { runtime::Trap { runtime::TrapCode::CallStackExhausted } });
    }
    return StubCall::done();
}

StubResult floatRound(ExecutionState* state,
                      InstanceContext const* /*context*/,
                      uint64_t operation,
                      uint64_t bits) noexcept
{
    auto const call = StubCall { *state, RuntimeStub::FloatRound };
    Require(operation < FloatRoundTable.size());
    auto const& row = FloatRoundTable[static_cast<std::size_t>(operation)];
    if (row.width == RoundedFloat::F32)
        return StubCall::done(
            numerics::roundToIntegral(runtime::F32 { .bits = static_cast<uint32_t>(bits) }, row.rounding)
                .bits);
    return StubCall::done(numerics::roundToIntegral(runtime::F64 { .bits = bits }, row.rounding).bits);
}

} // namespace isoline::execution
