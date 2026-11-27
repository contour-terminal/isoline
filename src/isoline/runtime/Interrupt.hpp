// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <isoline/runtime/Value.hpp>

namespace isoline::runtime
{

/// Whether a store was interrupted.
enum class InterruptState : uint8_t
{
    Running,     ///< Not interrupted: execution goes on.
    Interrupted, ///< Interrupted, for good: every poll from now on ends the call.
};

/// A store's interrupt: raised from any thread, wait-free and lock-free, and never cleared.
///
/// Raising it ends the call in progress at its next poll (Store::poll()), and every later call or
/// instantiation fails at its first poll, before any guest instruction runs. The store owns one from
/// construction (Store::interrupter()) and hands it out shared, so a watchdog may hold it past the
/// store's lifetime.
class Interrupter
{
  public:
    /// Interrupts: one release store to a lock-free atomic. Callable from any thread, any number of
    /// times, and from a signal handler, through a raw pointer taken before (the shared_ptr a store hands
    /// out is not async-signal-safe to copy).
    void interrupt() noexcept
    {
        _state.store(static_cast<uint8_t>(InterruptState::Interrupted), std::memory_order_release);
    }

    /// Whether interrupt() was called: one acquire load.
    /// @return Interrupted once interrupt() was called on any thread, Running before.
    [[nodiscard]] InterruptState state() const noexcept
    {
        return static_cast<InterruptState>(_state.load(std::memory_order_acquire));
    }

    /// The interrupt's word itself, for generated code, which tests it at every poll with one plain
    /// load and no other way: InterruptState::Running is 0, anything else is Interrupted. The load is
    /// only a hint. Acting on an interrupt goes through Store::poll(), which reads state() with
    /// acquire, so the argument holds on every target and not by x86-64's store order alone.
    /// @return The word's address; stable, and valid as long as the interrupter.
    [[nodiscard]] std::atomic<uint8_t> const* flagForGeneratedCode() const noexcept { return &_state; }

  private:
    std::atomic<uint8_t> _state { static_cast<uint8_t>(InterruptState::Running) };
    static_assert(std::atomic<uint8_t>::is_always_lock_free, "interrupt() must be wait-free");
    static_assert(sizeof(std::atomic<uint8_t>) == sizeof(uint8_t),
                  "generated code reads the interrupt word as one byte");
    static_assert(static_cast<uint8_t>(InterruptState::Running) == 0,
                  "generated code tests the interrupt word against 0");
};

/// The places every executor polls the store (Store::poll()): the rows of PollSiteTable, in order.
enum class PollSite : uint8_t
{
    FunctionEntry, ///< Every callee entry.
    LoopBackEdge,  ///< Every transfer of control to a loop label.
    HostReturn,    ///< Every return from a host function.
    BulkGranule,   ///< Every granule of a bulk memory or table operation; an interrupt or a spent step
                   ///< budget there leaves the granules before it written (partial writes).
    Instantiation, ///< Instantiation's entry, and before its start function.
    Unwind,        ///< Every UnwindPollItems labels and catch clauses an exception unwinds past.
    Sweep,         ///< Before every SweepPollItems items an exception sweep visits.
};

/// The last PollSite enumerator, for checking PollSiteTable's size.
inline constexpr auto LastPollSite = PollSite::Sweep;

/// What a poll charges against the step budget (StoreLimits::maxSteps).
enum class StepCharge : uint8_t
{
    None,    ///< Nothing: the poll only notifies the observer and checks the interrupt.
    OneStep, ///< One step.
};

/// The bytes of a bulk memory operation between two polls: memory.fill, memory.copy, memory.init
/// and instantiation's data segment copies poll before each granule of this many bytes.
inline constexpr auto BulkPollBytes = uint64_t { 65'536 };

/// The elements of a bulk table operation between two polls: table.fill, table.copy, table.init,
/// instantiation's element segment copies and its evaluation of element segment items.
inline constexpr auto BulkPollElements = BulkPollBytes / sizeof(Ref);
static_assert(BulkPollElements == 4'096,
              "the element granule is published, and so part of every engine's contract");

/// The labels, catch clauses and frames an exception unwinds past between two polls.
inline constexpr auto UnwindPollItems = uint64_t { 4'096 };

/// The items an exception sweep (Store::addException() on a full arena) visits between two polls.
inline constexpr auto SweepPollItems = uint64_t { 4'096 };

/// One poll site: where it is, and the limit it publishes.
struct PollSiteRow
{
    PollSite site;         ///< The site.
    std::string_view name; ///< Its name, as the documentation spells it.
    std::string_view rule; ///< The published limit: where exactly an executor polls.
    StepCharge charge;     ///< What a poll there charges.
};

/// Every poll site, one row each, in PollSite order.
inline constexpr auto PollSiteTable = std::to_array<PollSiteRow>({
    { .site = PollSite::FunctionEntry,
      .name = "function entry",
      .rule = "every callee entry, before its first instruction: call, call_indirect, call_ref, the "
              "tail calls return_call*, host callees, and the embedder's entry; before the call depth "
              "and the frame's stack charge are checked, so a call refused for its stack has cost its "
              "step, and a spent step budget traps before an exhausted stack",
      .charge = StepCharge::OneStep },
    { .site = PollSite::LoopBackEdge,
      .name = "loop back-edge",
      .rule = "every transfer of control to a loop label, whatever the instruction: br, br_if, "
              "br_table, br_on_null, br_on_non_null, and a try_table catch clause",
      .charge = StepCharge::OneStep },
    { .site = PollSite::HostReturn,
      .name = "host return",
      .rule = "every return from a host function, whatever it returned; an interrupt, and then a spent "
              "step budget, replaces its outcome",
      .charge = StepCharge::OneStep },
    { .site = PollSite::BulkGranule,
      .name = "bulk granule",
      .rule = "before every 65536 bytes of memory.fill, memory.copy, memory.init and instantiation's "
              "data copies, and every 4096 elements of table.fill, table.copy, "
              "table.init and instantiation's element copies and element item evaluation; copies "
              "visit their granules backwards when the destination is above the source",
      .charge = StepCharge::OneStep },
    { .site = PollSite::Instantiation,
      .name = "instantiation",
      .rule = "on entry to instantiation, and before the start function when the module has one",
      .charge = StepCharge::OneStep },
    { .site = PollSite::Unwind,
      .name = "unwind",
      .rule = "every 4096 labels, catch clauses and frames an exception unwinds past",
      .charge = StepCharge::None },
    { .site = PollSite::Sweep,
      .name = "exception sweep",
      .rule = "before every 4096 items an exception sweep visits, the first included, when an exception "
              "is allocated on a full arena and no host function is running; the items, in order: each "
              "value of the payload being allocated, each global, each slot of each table of exn type, "
              "each item of each element segment not dropped, each operand and each local of every "
              "active frame (every one, none pruned as dead), each slot of the exception arena (which "
              "grows to the high-water mark of slots not Free, reusing the lowest free address first), then "
              "each payload value of each exception found reachable, once per exception; a sweep a poll "
              "ends reclaims nothing, and the allocation ends with that poll's trap",
      .charge = StepCharge::OneStep },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: PollSiteTable)
        {
            if (static_cast<std::size_t>(row.site) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastPollSite) + 1;
    }(),
    "PollSiteTable must hold one row per PollSite, at the index of its enumerator");

/// Whether a store's poll observer is running (Store::poll()).
enum class PollObservation : uint8_t
{
    Idle,      ///< No observer is running.
    Observing, ///< IPollObserver::onPoll() is running: the store must not change.
};

/// One entry point a poll observer must not call: it writes the store, which an executor may hold in its
/// own state across the poll (memory 0's base and bound, a table's view, a global it cached, the countdown).
struct ObserverRefusalRow
{
    std::string_view entryPoint; ///< The entry point, qualified.
    std::string_view writes;     ///< What it writes.
};

/// Every entry point that writes the store, which Requires that no poll observer is running
/// (Store::requireUnobserved(), PollObservation::Idle) and ends the process otherwise. Exhaustive for the
/// embedder's reach: api::Store reaches the runtime only through these or through read-only paths, and its
/// own writers are listed. Each row has a contract case of its own (api/ObserverContract_test.cpp, the
/// "Contract.Observer." rows of tests/CMakeLists.txt). Not listed: the runtime's own poll machinery,
/// PollGate::lend(), commit() (Store::setStepsTaken()) and settle(), which run around polls and never inside
/// an observer, and Store::sweepExceptions(), which only addException() calls (checked there too).
inline constexpr auto ObserverRefusedEntryPoints = std::to_array<ObserverRefusalRow>({
    { .entryPoint = "runtime::Store::growMemory", .writes = "a memory's size, and maybe its base" },
    { .entryPoint = "runtime::Store::growTable", .writes = "a table's size, and maybe its elements' place" },
    { .entryPoint = "runtime::Store::internType", .writes = "the type registry" },
    { .entryPoint = "runtime::Store::addHostFunction", .writes = "the functions and their dispatch rows" },
    { .entryPoint = "runtime::Store::addModuleFunction", .writes = "the functions and their dispatch rows" },
    { .entryPoint = "runtime::Store::addTable", .writes = "the tables" },
    { .entryPoint = "runtime::Store::addMemory", .writes = "the memories" },
    { .entryPoint = "runtime::Store::addGlobal", .writes = "the globals" },
    { .entryPoint = "runtime::Store::addTag", .writes = "the tags" },
    { .entryPoint = "runtime::Store::addException", .writes = "the exceptions, and may sweep them" },
    { .entryPoint = "runtime::Store::addModuleInstance", .writes = "the module instances (instantiation)" },
    { .entryPoint = "runtime::Store::releaseTrailing", .writes = "drops tables and memories" },
    { .entryPoint = "runtime::Store::poll", .writes = "the step count (a re-entrant poll)" },
    { .entryPoint = "runtime::Store::chargeFrame", .writes = "the charged stack values" },
    { .entryPoint = "runtime::Store::releaseFrame", .writes = "the charged stack values" },
    { .entryPoint = "runtime::Store::bindDispatch", .writes = "a dispatch row" },
    { .entryPoint = "runtime::InvocationScope", .writes = "anything: an invocation re-enters the guest" },
    { .entryPoint = "api::Store::setGlobal", .writes = "a global's value" },
    { .entryPoint = "api::Store::tableSet", .writes = "a table element" },
    { .entryPoint = "api::Store::memoryRange", .writes = "a memory's bytes (the mutable view)" },
});

/// Observes every poll of a store: what poll-counting tests and profilers use.
///
/// The observer may read the store and never write it, and that is a checked contract. It runs in the middle
/// of an instruction, where an executor holds the store's state in its own registers: memory 0's base and
/// bound, a table's view, globals it cached, the step countdown. A write from the observer would be missed or
/// would leave a dangling pointer, so while onPoll() runs every entry point of ObserverRefusedEntryPoints
/// Requires PollObservation::Idle and ends the process otherwise. Reads stay legal:
/// api::Store::globalValue(), tableGet(), memorySize(), the read-only memoryRange(), and the types; they mark
/// an escaped exception (Store::markEscaped()), which is bookkeeping that allocates and moves nothing. The
/// observer may also interrupt the store (Interrupter::interrupt()).
class IPollObserver
{
  public:
    virtual ~IPollObserver() = default;

    /// Called at every poll, before the interrupt is checked, on the executing thread. It may read the store
    /// and must not write it (the class's contract, checked).
    /// @param site Where execution polled.
    virtual void onPoll(PollSite site) noexcept = 0;
};

} // namespace isoline::runtime
