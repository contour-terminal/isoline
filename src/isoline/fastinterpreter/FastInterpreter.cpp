// SPDX-License-Identifier: Apache-2.0
#include <isoline/fastinterpreter/FastInterpreter.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/fastinterpreter/Backends.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Context.hpp>
#include <isoline/fastinterpreter/SlotCodec.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/ValueCheck.hpp>

namespace isoline::fastinterpreter
{

namespace
{
    using runtime::Abrupt;
    using runtime::FunctionInstance;
    using runtime::ModuleInstance;
    using runtime::PollSite;
    using runtime::Store;
    using runtime::Trap;
    using runtime::TrapCode;
    using runtime::Value;

    /// What runs code of each dispatch, indexed by the dispatch.
    using Runner = Exit (*)(State) noexcept;
    inline constexpr auto Runners = std::to_array<Runner>({ &runSwitch, &runTailCall });

    static_assert(Runners.size() == std::to_underlying(Dispatch::TailCall) + 1, "a runner per Dispatch");

    /// The code of @p instance, which must be fast-interpreter code lowered for @p dispatch (checked).
    [[nodiscard]] LoweredModule const& loweredModuleOf(ModuleInstance const& instance, Dispatch dispatch)
    {
        Require(instance.code != nullptr && instance.code->engine() == runtime::ExecutionEngine::Fast);
        auto const& lowered = static_cast<LoweredModule const&>(*instance.code);
        Require(lowered.dispatch() == dispatch);
        return lowered;
    }

    /// The slots a value of @p type takes.
    [[nodiscard]] uint32_t widthOf(binary::ValueType const& type) noexcept
    {
        return slotWidth(slotClassOf(type));
    }

    /// One invocation's execution: its context, its slots and its frames.
    ///
    /// Every frame it entered is given back when it ends, however execution stopped: the depth and the
    /// stack charge of each (~Invocation()).
    class Invocation
    {
      public:
        /// @param store The store to execute on.
        /// @param depth The frames active over every invocation in progress; kept up to date.
        /// @param dispatch The dispatch the code was lowered for.
        Invocation(Store& store, uint32_t& depth, Dispatch dispatch):
            _context { store },
            _depth { depth },
            _dispatch { dispatch },
            _stepsLeft { _context.gate.reload() }
        {
        }

        Invocation(Invocation const&) = delete;
        Invocation(Invocation&&) = delete;
        Invocation& operator=(Invocation const&) = delete;
        Invocation& operator=(Invocation&&) = delete;

        /// Gives back the depth and the stack charges of the frames it entered.
        ~Invocation()
        {
            _depth -= static_cast<uint32_t>(_frameCosts.size());
            for (auto const cost: _frameCosts)
                _context.store.releaseFrame(cost);
        }

        /// Invokes the module function @p callee with @p args and writes its results to @p results.
        [[nodiscard]] std::expected<void, Abrupt> run(FunctionInstance const& callee,
                                                      binary::FuncType const& type,
                                                      std::span<Value const> args,
                                                      std::span<Value> results)
        {
            auto const& instance = _context.store.moduleInstance(callee.moduleInstance);
            auto const& function = loweredModuleOf(instance, _dispatch).functions()[callee.codeIndex];
            auto* const fp = reserveSlots(function.frameSlots);
            auto slot = uint32_t { 0 };
            for (auto const index: std::views::iota(std::size_t { 0 }, args.size()))
            {
                auto const width = widthOf(type.params[index]);
                storeValue(args[index], std::span { fp + slot, width });
                slot += width;
            }
            if (auto const entered = enterFunction(instance, function, fp); !entered)
                return stop(entered.error());
            auto const exit = Runners[std::to_underlying(_dispatch)](State { .pc = function.code.data(),
                                                                             .fp = fp,
                                                                             .context = &_context,
                                                                             .memory0 = nullptr,
                                                                             .memory0Size = 0,
                                                                             .stepsLeft = _stepsLeft });
            switch (exit)
            {
                case Exit::Returned: break;
                case Exit::Trapped: return std::unexpected { Abrupt { *_context.trap } };
                // uncovered-begin(no-exceptions-yet)
                case Exit::Thrown:
                    // No operation throws before the fast interpreter's exceptions are lowered.
                    core::unreachable();
                    // uncovered-end(no-exceptions-yet)
            }
            slot = _context.resultSlot;
            for (auto const index: std::views::iota(std::size_t { 0 }, results.size()))
            {
                auto const width = widthOf(type.results[index]);
                results[index] = loadValue(type.results[index], std::span<Slot const> { fp + slot, width });
                slot += width;
            }
            return {};
        }

      private:
        /// The slots of a frame of @p frameSlots slots, in a slot chunk of at least MinimumChunkSlots.
        [[nodiscard]] Slot* reserveSlots(uint64_t frameSlots)
        {
            _chunk = std::make_unique_for_overwrite<Slot[]>(std::max(MinimumChunkSlots, frameSlots));
            return _chunk.get();
        }

        /// Enters @p function of @p instance, its arguments in its first slots at @p fp: the one routine
        /// every frame goes through. It polls at FunctionEntry, then checks StoreLimits::maxCallDepth (one
        /// count over every invocation in progress), then charges the frame's static cost
        /// (Store::chargeFrame()), which ~Invocation() gives back, then initializes its declared locals.
        /// @return Nothing, or the trap it stopped with: Interrupted, StepLimitExceeded or
        /// CallStackExhausted.
        [[nodiscard]] std::expected<void, Trap> enterFunction(ModuleInstance const& instance,
                                                              LoweredFunction const& function,
                                                              Slot* fp)
        {
            if (auto const polled = _context.gate.poll(PollSite::FunctionEntry, _stepsLeft); !polled)
                return std::unexpected { polled.error() };
            if (_depth >= _context.store.limits().maxCallDepth)
                return std::unexpected { Trap { TrapCode::CallStackExhausted } };
            if (auto const charged = _context.store.chargeFrame(function.frameCost); !charged)
                return std::unexpected { charged.error() };
            _frameCosts.push_back(function.frameCost);
            ++_depth;
            initializeLocals(instance, function, fp);
            _context.instance = &instance;
            _context.code = function.code.data();
            return {};
        }

        /// Sets @p function's declared locals to their defaults: a Zero run's slots to zero, a Null run's
        /// pairs to the null of its heap, which nullSlots() maps through @p instance's TypeIds once per run.
        static void initializeLocals(ModuleInstance const& instance,
                                     LoweredFunction const& function,
                                     Slot* fp)
        {
            for (auto const& run: function.localInit)
            {
                auto const slots = std::span { fp + run.slotBase, run.slotCount };
                switch (run.kind)
                {
                    case LocalInit::Zero: std::ranges::fill(slots, Slot { 0 }); break;
                    case LocalInit::Null: {
                        auto const null = nullSlots(run.heap, instance.types);
                        for (auto const pair: std::views::iota(std::size_t { 0 }, slots.size() / 2))
                            std::ranges::copy(null, slots.subspan(2 * pair, 2).begin());
                        break;
                    }
                }
            }
        }

        /// Stops before any code ran: commits the countdown, as every way out does, and ends with @p trap.
        [[nodiscard]] std::unexpected<Abrupt> stop(Trap trap) noexcept
        {
            _context.gate.commit(_stepsLeft);
            return std::unexpected { Abrupt { trap } };
        }

        Context _context;
        uint32_t& _depth;
        Dispatch _dispatch;
        uint64_t _stepsLeft;
        std::unique_ptr<Slot[]> _chunk;
        std::vector<uint64_t> _frameCosts; ///< The stack charge of each frame entered, which ends with it.
    };
} // namespace

Flow leaveTrapped(Context& context, uint64_t stepsLeft, Trap trap) noexcept
{
    context.gate.commit(stepsLeft);
    context.trap = trap;
    return Flow::Trapped;
}

Flow leaveReturning(Context& context, uint64_t stepsLeft, uint32_t resultSlot) noexcept
{
    context.gate.commit(stepsLeft);
    context.resultSlot = resultSlot;
    return Flow::Returned;
}

FastInterpreter::FastInterpreter(Dispatch dispatch): _dispatch { dispatch }
{
    Require(std::ranges::find(availableDispatches(), dispatch) != availableDispatches().end());
}

runtime::ExecutionEngine FastInterpreter::engine() const noexcept
{
    return runtime::ExecutionEngine::Fast;
}

Dispatch FastInterpreter::dispatch() const noexcept
{
    return _dispatch;
}

std::expected<void, Abrupt> FastInterpreter::invoke(Store& store,
                                                    runtime::FuncAddr function,
                                                    std::span<Value const> args,
                                                    std::span<Value> results)
{
    // Float results are the spec's only in the default floating-point environment, which the
    // invocation's scope establishes (runtime::FloatEnvironmentScope).
    assert(std::fegetround() == FE_TONEAREST);
    // The invocation's scope, its nesting limit and its step budget, is the entry's to open.
    Require(store.invocationDepth() > 0);
    auto const& callee = store.function(function);
    // A host function's direct invocation comes with calls, through the one host-call seam.
    Require(!callee.isHost());
    auto const& type = store.functionType(function);
    // The caller's to check: arguments of the function's types, references in this store.
    Require(runtime::checkValues(store, args, type.params).has_value());
    Require(results.size() == type.results.size());
    return Invocation { store, _depth, _dispatch }.run(callee, type, args, results);
}

} // namespace isoline::fastinterpreter
