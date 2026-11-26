// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <semaphore>
#include <thread>
#include <utility>

#include <isoline/runtime/Interrupt.hpp>

namespace isoline::runtime::testing
{

/// A poll that triggers a PollCounter: the @p ordinal-th poll at @p site, counting from 1.
struct PollTrigger
{
    PollSite site;    ///< Where.
    uint64_t ordinal; ///< Which poll there, the first being 1.
};

/// How a wait for a PollCounter's trigger ended.
enum class TriggerWait : uint8_t
{
    TimedOut,  ///< The deadline passed first: the guest never reached the trigger.
    Triggered, ///< The executing thread reached the trigger.
};

/// How long a test's watchdog waits for the trigger before it gives up and interrupts anyway.
inline constexpr auto WatchdogDeadline = std::chrono::seconds { 30 };

/// How long a watchdog waits for the trigger at a time before it checks whether it was stopped.
inline constexpr auto WatchdogStopSlice = std::chrono::milliseconds { 10 };

/// Counts a store's polls per site, as the poll-counting tests do; optionally triggers at one poll,
/// where it interrupts the store on the executing thread or wakes a thread waiting to interrupt it.
class PollCounter final: public IPollObserver
{
  public:
    /// A counter that never triggers.
    PollCounter() = default;

    /// A counter that triggers at @p trigger.
    /// @param trigger The poll it triggers at; none never triggers, as the default counter.
    explicit PollCounter(std::optional<PollTrigger> trigger) noexcept: _trigger { trigger } {}

    /// Interrupts @p interrupter, on the executing thread, at the trigger. An attach-style call after
    /// construction, as the wiring is cyclic: the store that owns the interrupter is constructed with
    /// this observer.
    /// @param interrupter The store's interrupter.
    void interruptAtTrigger(std::shared_ptr<Interrupter> interrupter) noexcept
    {
        _interrupter = std::move(interrupter);
    }

    void onPoll(PollSite site) noexcept override
    {
        auto& count = _counts[static_cast<std::size_t>(site)];
        ++count;
        if (_triggered.load(std::memory_order_relaxed))
        {
            ++_pollsAfterTrigger;
            return;
        }
        if (!_trigger || _trigger->site != site || _trigger->ordinal != count)
            return;
        if (_interrupter)
            _interrupter->interrupt();
        _triggered.store(true, std::memory_order_relaxed);
        _signal.release();
    }

    /// The polls at @p site so far. Read on the executing thread, or once it finished.
    /// @param site The site.
    /// @return Their count.
    [[nodiscard]] uint64_t count(PollSite site) const noexcept
    {
        return _counts[static_cast<std::size_t>(site)];
    }

    /// The polls, at any site, after the trigger.
    /// @return Their count; 0 before the trigger.
    [[nodiscard]] uint64_t pollsAfterTrigger() const noexcept { return _pollsAfterTrigger; }

    /// Blocks until the trigger or @p deadline, from another thread: what a test's watchdog waits on
    /// before it interrupts. Only one thread may wait, as often as it likes until the trigger.
    /// @param deadline How long to wait at most.
    /// @return Triggered once the executing thread reached the trigger; TimedOut at the deadline.
    [[nodiscard]] TriggerWait waitForTrigger(std::chrono::milliseconds deadline)
    {
        return _signal.try_acquire_for(deadline) ? TriggerWait::Triggered : TriggerWait::TimedOut;
    }

  private:
    std::optional<PollTrigger> _trigger;
    std::shared_ptr<Interrupter> _interrupter;
    std::array<uint64_t, PollSiteTable.size()> _counts {};
    uint64_t _pollsAfterTrigger = 0;
    std::atomic<bool> _triggered { false }; ///< Read and written on the executing thread only.
    std::binary_semaphore _signal { 0 };    ///< Released once, at the trigger.
};

/// A watchdog thread that interrupts a store once its PollCounter triggers, or at a deadline, so a
/// guest whose polls went missing ends the test instead of hanging it.
///
/// A std::thread joined explicitly, not a std::jthread, which Apple's libc++ lacks. Destroyed before
/// finish(), it stops the thread at its next wait slice (WatchdogStopSlice) and joins it.
class InterruptWatchdog
{
  public:
    /// Starts the watchdog.
    /// @param counter The store's poll observer, with a trigger; must outlive the watchdog.
    /// @param interrupter The store's interrupter.
    /// @param deadline How long to wait for the trigger before interrupting anyway.
    InterruptWatchdog(PollCounter& counter,
                      std::shared_ptr<Interrupter> interrupter,
                      std::chrono::milliseconds deadline = WatchdogDeadline):
        _thread { [this, &counter, interrupter = std::move(interrupter), deadline] {
            _outcome.store(waitForTrigger(counter, deadline), std::memory_order_relaxed);
            interrupter->interrupt();
        } }
    {
    }

    InterruptWatchdog(InterruptWatchdog const&) = delete;
    InterruptWatchdog(InterruptWatchdog&&) = delete;
    InterruptWatchdog& operator=(InterruptWatchdog const&) = delete;
    InterruptWatchdog& operator=(InterruptWatchdog&&) = delete;
    ~InterruptWatchdog()
    {
        _stopping.store(true, std::memory_order_release);
        if (_thread.joinable())
            _thread.join();
    }

    /// Waits for the watchdog to finish.
    /// @return Triggered when it interrupted at the trigger; TimedOut when at the deadline.
    [[nodiscard]] TriggerWait finish()
    {
        _thread.join();
        return _outcome.load(std::memory_order_relaxed);
    }

  private:
    /// Waits for @p counter's trigger until @p deadline, in slices, so that the destructor can stop it.
    [[nodiscard]] TriggerWait waitForTrigger(PollCounter& counter, std::chrono::milliseconds deadline)
    {
        auto const end = std::chrono::steady_clock::now() + deadline;
        while (!_stopping.load(std::memory_order_acquire))
        {
            auto const left =
                std::chrono::ceil<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
            if (left <= std::chrono::milliseconds::zero())
                break;
            if (counter.waitForTrigger(std::min(left, WatchdogStopSlice)) == TriggerWait::Triggered)
                return TriggerWait::Triggered;
        }
        return TriggerWait::TimedOut;
    }

    std::atomic<TriggerWait> _outcome { TriggerWait::TimedOut };
    std::atomic<bool> _stopping { false }; ///< Set by the destructor: the thread stops waiting.
    std::thread _thread; ///< Last: it starts at construction and uses the members before it.
};

} // namespace isoline::runtime::testing
