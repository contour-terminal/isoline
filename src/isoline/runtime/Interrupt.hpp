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
    /// Interrupts: one release store. Callable from any thread, any number of times.
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
    BulkGranule,   ///< Every granule of a bulk memory or table operation.
    Instantiation, ///< Instantiation's entry, and before its start function.
    Unwind,        ///< Every UnwindPollItems labels and catch clauses an exception unwinds past.
};

/// The last PollSite enumerator, for checking PollSiteTable's size.
inline constexpr auto LastPollSite = PollSite::Unwind;

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
              "tail calls return_call*, host callees, and the embedder's entry",
      .charge = StepCharge::OneStep },
    { .site = PollSite::LoopBackEdge,
      .name = "loop back-edge",
      .rule = "every transfer of control to a loop label, whatever the instruction: br, br_if, "
              "br_table, br_on_null, br_on_non_null, and a try_table catch clause",
      .charge = StepCharge::OneStep },
    { .site = PollSite::HostReturn,
      .name = "host return",
      .rule = "every return from a host function, whatever it returned; an interrupt replaces its "
              "outcome",
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
