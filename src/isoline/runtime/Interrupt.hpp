// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <isoline/meta/EnumRows.hpp>
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

  private:
    std::atomic<uint8_t> _state { static_cast<uint8_t>(InterruptState::Running) };
    static_assert(std::atomic<uint8_t>::is_always_lock_free, "interrupt() must be wait-free");
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
    HostBulk,      ///< The embedder's poll of its own bulk work (api::Store::poll()): a component
                   ///< call's copies and transcodes, at least every BulkPollBytes bytes.
};

/// The last PollSite enumerator, for checking PollSiteTable's size.
inline constexpr auto LastPollSite = PollSite::HostBulk;

/// What a poll charges against the step budget (StoreLimits::maxSteps).
enum class StepCharge : uint8_t
{
    None,       ///< Nothing: the poll only notifies the observer and checks the interrupt.
    OneStep,    ///< One step.
    PerGranule, ///< A step per BulkPollBytes of the work done since the last poll, at least one.
};

/// The last StepCharge enumerator, for checking StepChargeTable; it must name whichever is listed last above.
inline constexpr auto LastStepCharge = StepCharge::PerGranule;

/// The bytes of a bulk memory operation between two polls: memory.fill, memory.copy, memory.init
/// and instantiation's data segment copies poll before each granule of this many bytes.
inline constexpr auto BulkPollBytes = uint64_t { 65'536 };

/// One way a poll charges: how the documentation names it.
struct StepChargeRow
{
    StepCharge charge;     ///< The charge.
    std::string_view name; ///< How the documentation names it.
};

/// Every StepCharge, one row each, in StepCharge order.
inline constexpr auto StepChargeTable = std::to_array<StepChargeRow>({
    { .charge = StepCharge::None, .name = "no step" },
    { .charge = StepCharge::OneStep, .name = "one step" },
    { .charge = StepCharge::PerGranule, .name = "a step per 65536 bytes, at least one" },
});

static_assert(meta::firstMisplacedRow<LastStepCharge>(StepChargeTable, &StepChargeRow::charge)
                  == meta::NoMisplacedRow,
              "StepChargeTable must hold one row per StepCharge, at the index of its enumerator");

/// The steps a poll charges: the published rule every engine, and every embedder's bulk work,
/// reproduces.
/// @param charge What its site charges (PollSiteRow::charge).
/// @param bytes The bytes of work done since the last poll; only StepCharge::PerGranule reads them.
/// @return 0, 1, or max(1, bytes / BulkPollBytes).
[[nodiscard]] constexpr uint64_t stepsCharged(StepCharge charge, uint64_t bytes) noexcept
{
    switch (charge)
    {
        case StepCharge::None: return 0;
        case StepCharge::OneStep: return 1;
        case StepCharge::PerGranule: return bytes < BulkPollBytes ? 1 : bytes / BulkPollBytes;
    }
    return 1; // uncovered(exhaustive-switch)
}

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
    { .site = PollSite::HostBulk,
      .name = "host bulk",
      .rule = "where the embedder polls its own bulk work (Store::poll), at least every 65536 bytes of a "
              "component call's copies and transcodes; it charges the step budget of the invocation in "
              "progress, which an invocation scope (Store::openInvocation) shares between the calls made "
              "directly in it, and a charge the budget cannot take whole is refused, charging nothing",
      .charge = StepCharge::PerGranule },
});

static_assert(meta::firstMisplacedRow<LastPollSite>(PollSiteTable, &PollSiteRow::site)
                  == meta::NoMisplacedRow,
              "PollSiteTable must hold one row per PollSite, at the index of its enumerator");

/// Observes every poll of a store: what poll-counting tests and profilers use.
class IPollObserver
{
  public:
    virtual ~IPollObserver() = default;

    /// Called at every poll, before the interrupt is checked. It runs on the executing thread, in the
    /// middle of an instruction, so it must not touch the store; it may interrupt it.
    /// @param site Where execution polled.
    virtual void onPoll(PollSite site) noexcept = 0;
};

} // namespace isoline::runtime
