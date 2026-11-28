// SPDX-License-Identifier: Apache-2.0
#pragma once

// The runtime stubs: what generated code calls for work it does not inline (compiler::RuntimeStub), through
// ExecutionState::stubs. Each is a non-member, noexcept function of the host convention taking the execution
// state, the instance context and its operands as 64-bit words, and returning a compiler::StubResult: its
// status and its value (P5 Decision "Calling conventions are rows", I8). None traps or jumps: a stub that
// fails records its Abrupt in the executor's record and returns StubStatus::Pending, and generated code
// leaves through the trampoline's trap exit (Decision "Traps never skip a C++ frame").
//
// RuntimeStubTable holds one row per stub, in RuntimeStub's order: its name, whether it settles the execution
// budget around its work, and its entry. A stub that Settles gives the lease generated code counts down back
// to the store before it polls, charges or calls out (PollGate::settle()), and lends it again before it
// returns, whatever it returns (Decision "The execution budget is lent through P4's PollGate").

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/PollGate.hpp>
#include <isoline/runtime/Store.hpp>

namespace isoline::execution
{

/// What a stub returns as StubResult::status, zero-extended to the 64-bit word generated code tests whole: no
/// narrower part of the word is left unspecified at the boundary (I8).
enum class StubStatus : uint8_t
{
    Continue, ///< The stub did its work; generated code goes on.
    Pending,  ///< ExecutionState::pending holds PendingKind::StubAbrupt; the Abrupt is in the ExecutorRecord.
};

static_assert(std::to_underlying(StubStatus::Continue) == compiler::StubSucceeded,
              "generated code takes StubSucceeded for a stub that did its work");

/// Whether a stub settles the execution budget around its work.
enum class Settles : uint8_t
{
    No,  ///< A leaf: it neither polls nor charges, so the lease stays lent.
    Yes, ///< It settles the lease first and lends it again before it returns.
};

/// The executor's per-store record, which ExecutionState::executorState points at: what the stubs reach the
/// store through. One per store and executor, shared by the invocations host functions nest.
struct ExecutorRecord
{
    runtime::Store* store = nullptr;            ///< The store.
    runtime::ICodeExecutor* executor = nullptr; ///< What runs the guest a host function invokes again.
    runtime::PollGate* gate = nullptr;          ///< The store's poll gate, which lends and settles.
    runtime::Lease lent {};                     ///< The lease outstanding: what the gate last lent.
    std::optional<runtime::Abrupt> abrupt {};   ///< The Abrupt of the stub that returned Pending last.
};

namespace detail
{
    /// One operand word of a stub.
    template <std::size_t>
    using StubWord = uint64_t;

    template <typename Indices>
    struct StubFunctionOf;

    template <std::size_t... Indices>
    struct StubFunctionOf<std::index_sequence<Indices...>>
    {
        using Type = compiler::StubResult (*)(compiler::ExecutionState*,
                                              compiler::InstanceContext const*,
                                              StubWord<Indices>...) noexcept;
    };
} // namespace detail

/// The type of a stub of @p Operands operand words.
template <std::size_t Operands>
using StubFunction = detail::StubFunctionOf<std::make_index_sequence<Operands>>::Type;

/// A stub's entry, typed by how many operand words it takes: what RuntimeStubTable holds, so that the table
/// is a constant and each stub keeps its own signature.
using StubEntry = std::variant<StubFunction<0>,
                               StubFunction<1>,
                               StubFunction<2>,
                               StubFunction<3>,
                               StubFunction<4>,
                               StubFunction<5>>;

/// How many operand words @p entry's stub takes.
[[nodiscard]] constexpr std::size_t operandCountOf(StubEntry const& entry) noexcept
{
    return entry.index();
}

/// The address generated code calls @p entry's stub at.
[[nodiscard]] void const* entryAddress(StubEntry const& entry) noexcept;

/// One runtime stub.
struct RuntimeStubRow
{
    compiler::RuntimeStub stub; ///< The stub; equal to the row's index.
    std::string_view name;      ///< Its name.
    Settles settles;            ///< Whether it settles the budget around its work.
    StubEntry entry;            ///< Its function.
};

// The stubs, each with the operands compiler::RuntimeStub's documentation lists, in that order.

/// PollSlow: a poll's slow path at PollSite @p site, through the gate (Store::poll() on an observed
/// store).
compiler::StubResult pollSlow(compiler::ExecutionState* state,
                              compiler::InstanceContext const* context,
                              uint64_t site) noexcept;

/// MemoryGrow: grows memory @p memory by @p delta pages (Store::growMemory()); its value is the old size
/// in pages, or all ones once refused.
compiler::StubResult memoryGrow(compiler::ExecutionState* state,
                                compiler::InstanceContext const* context,
                                uint64_t memory,
                                uint64_t delta) noexcept;

/// MemoryFill: memory.fill (runtime::fillItems()).
compiler::StubResult memoryFill(compiler::ExecutionState* state,
                                compiler::InstanceContext const* context,
                                uint64_t memory,
                                uint64_t destination,
                                uint64_t value,
                                uint64_t count) noexcept;

/// MemoryCopy: memory.copy (runtime::copyItems()).
compiler::StubResult memoryCopy(compiler::ExecutionState* state,
                                compiler::InstanceContext const* context,
                                uint64_t destinationMemory,
                                uint64_t sourceMemory,
                                uint64_t destination,
                                uint64_t source,
                                uint64_t count) noexcept;

/// MemoryInit: memory.init (runtime::copySegment()); a dropped segment is empty.
compiler::StubResult memoryInit(compiler::ExecutionState* state,
                                compiler::InstanceContext const* context,
                                uint64_t segment,
                                uint64_t memory,
                                uint64_t destination,
                                uint64_t source,
                                uint64_t count) noexcept;

/// DataDrop: data.drop.
compiler::StubResult dataDrop(compiler::ExecutionState* state,
                              compiler::InstanceContext const* context,
                              uint64_t segment) noexcept;

/// TableGrow: grows table @p table by @p delta slots of @p initial, a Ref's word (Store::growTable());
/// its value is the old size, or all ones once refused.
compiler::StubResult tableGrow(compiler::ExecutionState* state,
                               compiler::InstanceContext const* context,
                               uint64_t table,
                               uint64_t initial,
                               uint64_t delta) noexcept;

/// TableFill: table.fill of @p value, a Ref's word (runtime::fillItems()).
compiler::StubResult tableFill(compiler::ExecutionState* state,
                               compiler::InstanceContext const* context,
                               uint64_t table,
                               uint64_t destination,
                               uint64_t value,
                               uint64_t count) noexcept;

/// TableCopy: table.copy (runtime::copyItems()).
compiler::StubResult tableCopy(compiler::ExecutionState* state,
                               compiler::InstanceContext const* context,
                               uint64_t destinationTable,
                               uint64_t sourceTable,
                               uint64_t destination,
                               uint64_t source,
                               uint64_t count) noexcept;

/// TableInit: table.init (runtime::copySegment()); a dropped segment is empty.
compiler::StubResult tableInit(compiler::ExecutionState* state,
                               compiler::InstanceContext const* context,
                               uint64_t segment,
                               uint64_t table,
                               uint64_t destination,
                               uint64_t source,
                               uint64_t count) noexcept;

/// ElementDrop: elem.drop.
compiler::StubResult elementDrop(compiler::ExecutionState* state,
                                 compiler::InstanceContext const* context,
                                 uint64_t segment) noexcept;

/// TableGet: table.get of an imported table; its value is the element's word.
compiler::StubResult tableGet(compiler::ExecutionState* state,
                              compiler::InstanceContext const* context,
                              uint64_t table,
                              uint64_t index) noexcept;

/// TableSet: table.set of an imported table, @p value a Ref's word.
compiler::StubResult tableSet(compiler::ExecutionState* state,
                              compiler::InstanceContext const* context,
                              uint64_t table,
                              uint64_t index,
                              uint64_t value) noexcept;

/// CallHost: the FunctionEntry poll, then host function @p function through runtime::callHostFunction(),
/// its arguments decoded from ExecutionState::hostScratch, its results encoded back into it.
compiler::StubResult callHost(compiler::ExecutionState* state,
                              compiler::InstanceContext const* context,
                              uint64_t function) noexcept;

/// FloatRound: the rounding @p operation (a compiler::FloatRoundOperation) of the float bits @p bits, by
/// numerics::, as both interpreters round; its value is the rounded bits.
compiler::StubResult floatRound(compiler::ExecutionState* state,
                                compiler::InstanceContext const* context,
                                uint64_t operation,
                                uint64_t bits) noexcept;

/// Every runtime stub, in compiler::RuntimeStub's order.
inline constexpr auto RuntimeStubTable = std::to_array<RuntimeStubRow>({
    { compiler::RuntimeStub::PollSlow, "PollSlow", Settles::Yes, &pollSlow },
    { compiler::RuntimeStub::MemoryGrow, "MemoryGrow", Settles::No, &memoryGrow },
    { compiler::RuntimeStub::MemoryFill, "MemoryFill", Settles::Yes, &memoryFill },
    { compiler::RuntimeStub::MemoryCopy, "MemoryCopy", Settles::Yes, &memoryCopy },
    { compiler::RuntimeStub::MemoryInit, "MemoryInit", Settles::Yes, &memoryInit },
    { compiler::RuntimeStub::DataDrop, "DataDrop", Settles::No, &dataDrop },
    { compiler::RuntimeStub::TableGrow, "TableGrow", Settles::No, &tableGrow },
    { compiler::RuntimeStub::TableFill, "TableFill", Settles::Yes, &tableFill },
    { compiler::RuntimeStub::TableCopy, "TableCopy", Settles::Yes, &tableCopy },
    { compiler::RuntimeStub::TableInit, "TableInit", Settles::Yes, &tableInit },
    { compiler::RuntimeStub::ElementDrop, "ElementDrop", Settles::No, &elementDrop },
    { compiler::RuntimeStub::TableGet, "TableGet", Settles::No, &tableGet },
    { compiler::RuntimeStub::TableSet, "TableSet", Settles::No, &tableSet },
    { compiler::RuntimeStub::CallHost, "CallHost", Settles::Yes, &callHost },
    { compiler::RuntimeStub::FloatRound, "FloatRound", Settles::No, &floatRound },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: RuntimeStubTable)
            if (static_cast<std::size_t>(row.stub) != index++)
                return false;
        return index == static_cast<std::size_t>(compiler::LastRuntimeStub) + 1;
    }(),
    "RuntimeStubTable must hold one row per RuntimeStub, at the index of its enumerator");

/// The row of @p stub.
[[nodiscard]] constexpr RuntimeStubRow const& rowOf(compiler::RuntimeStub stub) noexcept
{
    return RuntimeStubTable[static_cast<std::size_t>(stub)];
}

/// Every stub's entry, by RuntimeStub: what ExecutionState::stubs points at. Built once, and fixed for the
/// process's lifetime.
[[nodiscard]] std::span<void const* const> stubEntries() noexcept;

/// The RawValue cells ExecutionState::hostScratch must hold for every host function @p store holds:
/// max(params, results) of the widest. The host-call scratch is read before a host runs and written after it
/// returns, so the invocations a host nests share it.
/// @param store The store.
/// @return The cells.
[[nodiscard]] uint32_t hostScratchValuesOf(runtime::Store const& store) noexcept;

} // namespace isoline::execution
