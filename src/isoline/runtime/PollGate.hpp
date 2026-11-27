// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>

namespace isoline::runtime
{

/// An execution budget lent to generated code (PollGate::lend()): the steps and the stack values
/// left, which it counts down inline until it settles them back (PollGate::settle()).
struct Lease
{
    uint64_t steps = 0;       ///< The steps left: StoreLimits::maxSteps minus those taken; 0 when observed.
    uint64_t stackValues = 0; ///< The stack values left: StoreLimits::maxStackValues minus those charged.
};

/// The executor-local fast path of Store::poll(): the same order (observer, interrupt, step), the same
/// result and the same charge, with the step budget counted down in the executor's own variable.
///
/// The executor owns the countdown, the steps left (StoreLimits::maxSteps minus the steps taken): it
/// takes it from reload() once its invocation is open, passes it to every poll(), and writes it back
/// with commit() before anything that may poll through the store (a host call, a nested invocation,
/// Store::addException(), whose sweep polls at PollSite::Sweep), then takes it again from reload().
/// It commits once more before its invocation ends, on every path out: a return, a trap, and an
/// exception that escapes, so that Store::used(Limit::Steps) counts every step the guest took.
///
/// On an unobserved store a poll is a test of the gate's path byte, one acquire load of the interrupt
/// word (through the cached pointer: two dependent loads) and a test and decrement of the countdown.
/// On an observed store every poll goes through Store::poll() itself, so that a poll-counting test
/// exercises the store's own order, and checks first that the countdown is the store's: an executor
/// that forgot a commit or a reload fails there, where the differential tests, which observe, see it.
///
/// Generated code holds more of the budget than the countdown: it charges and releases its frames'
/// stack values inline too. It takes both as a Lease (lend()) and gives both back (settle()) around
/// every call into the runtime and on every way out, and between the two the store's budget is Lent
/// (Store::budgetState()), so that nothing polls or charges through the store against stale counts.
class PollGate
{
  public:
    /// Caches @p store's interrupt, step limit, stack-value limit (StoreLimits::maxStackValues) and
    /// whether an observer is attached.
    /// @param store The store; must outlive the gate.
    explicit PollGate(Store& store) noexcept:
        _store { &store },
        _interrupt { store._interrupt },
        _maxSteps { store._limits.maxSteps },
        _maxStackValues { store._limits.maxStackValues },
        _path { store._pollObserver != nullptr ? PollPath::ThroughStore : PollPath::Direct }
    {
    }

    PollGate(PollGate const&) = delete;
    PollGate(PollGate&&) = delete;
    PollGate& operator=(PollGate const&) = delete;
    PollGate& operator=(PollGate&&) = delete;
    ~PollGate() = default;

    /// The steps left of the outermost invocation's budget, as the store counts them.
    /// @return StoreLimits::maxSteps minus the steps taken.
    [[nodiscard]] uint64_t reload() const noexcept { return _maxSteps - _store->stepsTaken(); }

    /// Writes the countdown back to the store, as the steps taken.
    /// Precondition (checked): @p stepsLeft is at most StoreLimits::maxSteps.
    /// @param stepsLeft The steps left: reload()'s value, counted down by poll() since.
    void commit(uint64_t stepsLeft) noexcept
    {
        Require(stepsLeft <= _maxSteps);
        _store->setStepsTaken(_maxSteps - stepsLeft);
    }

    /// Lends the budget left to generated code: the steps left (reload()), none on an observed store,
    /// so that every poll there takes the slow path through Store::poll(); and the stack values left.
    /// The store's budget is Lent until settle().
    ///
    /// Precondition (checked): the budget is Settled: a lease over another would count its steps twice.
    /// @return The lease.
    [[nodiscard]] Lease lend() noexcept
    {
        Require(_store->_budgetState == BudgetState::Settled);
        _store->_budgetState = BudgetState::Lent;
        return Lease { .steps = _path == PollPath::Direct ? reload() : 0,
                       .stackValues = _maxStackValues - _store->_chargedStackValues };
    }

    /// Settles a lease: makes what generated code left of it the store's budget again, absolutely, so
    /// that a frame charged under one lease and released under a later one balances. The steps taken
    /// become StoreLimits::maxSteps minus @p left's steps (commit()), except on an observed store,
    /// whose own polls counted every step; the stack values charged become
    /// StoreLimits::maxStackValues minus @p left's. The store's budget is Settled again.
    ///
    /// Precondition (checked): the budget is Lent; @p left holds no more steps than @p lent, as no
    /// poll adds one, and no more stack values than maxStackValues, as no frame is released uncharged.
    /// @param lent The lease lend() returned.
    /// @param left What generated code left of it.
    void settle(Lease const& lent, Lease const& left) noexcept
    {
        Require(_store->_budgetState == BudgetState::Lent);
        Require(left.steps <= lent.steps);
        Require(left.stackValues <= _maxStackValues);
        if (_path == PollPath::Direct)
            commit(left.steps);
        _store->_chargedStackValues = _maxStackValues - left.stackValues;
        _store->_budgetState = BudgetState::Settled;
    }

    /// Polls, exactly as Store::poll() does: notifies the observer, checks the interrupt, then charges
    /// what the site's PollSiteTable row charges; at one poll, Interrupted beats StepLimitExceeded.
    ///
    /// Precondition (checked on an observed store): @p stepsLeft is reload()'s value, counted down by
    /// this gate's polls since: the executor committed and reloaded around every poll through the store.
    /// @param site Where execution polls.
    /// @param stepsLeft The countdown; decremented by a charged poll that goes on.
    /// @return Nothing to go on; Trap{Interrupted} once the store was interrupted; or
    ///         Trap{StepLimitExceeded}, no step charged, when no step is left.
    [[nodiscard]] std::expected<void, Trap> poll(PollSite site, uint64_t& stepsLeft) noexcept
    {
        if (_path == PollPath::ThroughStore) [[unlikely]]
        {
            // No commit first: on this path every charge is the store's own, so the countdown is
            // reload()'s value from the poll before, unless the executor broke the commit/reload
            // contract, which this checks. The countdown never leaves this function, so it stays in
            // a register on the fast path.
            Require(stepsLeft == reload());
            auto polled = _store->poll(site);
            stepsLeft = reload();
            return polled;
        }
        if (_interrupt->state() == InterruptState::Interrupted) [[unlikely]]
            return trap(TrapCode::Interrupted);
        if (PollSiteTable[static_cast<std::size_t>(site)].charge == StepCharge::None)
            return {};
        if (stepsLeft == 0) [[unlikely]]
            return trap(TrapCode::StepLimitExceeded);
        --stepsLeft;
        return {};
    }

  private:
    /// Where a poll goes.
    enum class PollPath : uint8_t
    {
        Direct,       ///< No observer: the gate checks the interrupt and counts down itself.
        ThroughStore, ///< An observer: every poll goes through Store::poll() and reloads.
    };

    Store* _store;
    Interrupter const* _interrupt;
    uint64_t _maxSteps;
    uint64_t _maxStackValues;
    PollPath _path;
};

} // namespace isoline::runtime
