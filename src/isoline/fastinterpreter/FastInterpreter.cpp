// SPDX-License-Identifier: Apache-2.0
#include <isoline/fastinterpreter/FastInterpreter.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/fastinterpreter/Backends.hpp>
#include <isoline/fastinterpreter/Bounds.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Context.hpp>
#include <isoline/fastinterpreter/SlotCodec.hpp>
#include <isoline/runtime/HostCall.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/ValueCheck.hpp>

namespace isoline::fastinterpreter
{

namespace
{
    using runtime::Abrupt;
    using runtime::FuncAddr;
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

    /// Fills @p slots, pair by pair, with @p pair: a run of reference locals with their null.
    void fillPairs(std::span<Slot> slots, std::array<Slot, 2> const& pair) noexcept
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, slots.size() / 2))
            std::ranges::copy(pair, slots.subspan(2 * index, 2).begin());
    }

    /// Decodes the values of @p types from consecutive slots from @p slots into @p values.
    void loadValues(std::span<binary::ValueType const> types,
                    Slot const* slots,
                    std::span<Value> values) noexcept
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, types.size()))
        {
            auto const width = slotWidth(slotClassOf(types[index]));
            values[index] = loadValue(types[index], std::span { slots, width });
            slots += width;
        }
    }

    /// Encodes @p values, of @p types, into consecutive slots from @p slots.
    void storeValues(std::span<binary::ValueType const> types,
                     std::span<Value const> values,
                     Slot* slots) noexcept
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, types.size()))
        {
            auto const width = slotWidth(slotClassOf(types[index]));
            storeValue(values[index], std::span { slots, width });
            slots += width;
        }
    }

    /// A run of slots from a memory resource, given back when it ends; empty until assigned one.
    class SlotChunk
    {
      public:
        /// @p count slots, uninitialized, from @p memory.
        SlotChunk(std::pmr::memory_resource* memory, uint64_t count):
            _memory { memory },
            _count { count },
            _slots { static_cast<Slot*>(memory->allocate(count * sizeof(Slot), alignof(Slot))) }
        {
        }

        SlotChunk(SlotChunk const&) = delete;
        SlotChunk& operator=(SlotChunk const&) = delete;

        SlotChunk(SlotChunk&& other) noexcept:
            _memory { std::exchange(other._memory, nullptr) },
            _count { std::exchange(other._count, 0) },
            _slots { std::exchange(other._slots, nullptr) }
        {
        }

        SlotChunk& operator=(SlotChunk&&) = delete;

        ~SlotChunk() { release(); }

        /// Its first slot; null when empty.
        [[nodiscard]] Slot* slots() const noexcept { return _slots; }

        /// The slots it holds.
        [[nodiscard]] uint64_t count() const noexcept { return _count; }

      private:
        void release() noexcept
        {
            if (_slots != nullptr)
                _memory->deallocate(_slots, _count * sizeof(Slot), alignof(Slot));
            _slots = nullptr;
        }

        std::pmr::memory_resource* _memory = nullptr;
        uint64_t _count = 0;
        Slot* _slots = nullptr;
    };

    /// The index of the chunk before the first: where the arguments of the invocation's own function are,
    /// which come from the embedder, not from a slot.
    inline constexpr auto NoChunk = std::numeric_limits<std::size_t>::max();

    /// What memory.grow returns where the grow was refused: -1, of the memory's address type once narrowed.
    inline constexpr auto RefusedGrowth = std::numeric_limits<uint64_t>::max();

    /// The function a call resolved to: a host function, or a module function with its code.
    struct Callee
    {
        FunctionInstance const* host = nullptr;    ///< The host function; null for a module function.
        ModuleInstance const* instance = nullptr;  ///< A module function's instance.
        uint32_t instanceAddress = 0;              ///< Its address in the store.
        LoweredModule const* module = nullptr;     ///< Its module's code.
        LoweredFunction const* function = nullptr; ///< Its own code.
    };

    /// One active Wasm frame of an invocation.
    struct Frame
    {
        LoweredFunction const* function = nullptr; ///< Its code.
        LoweredModule const* module = nullptr;     ///< Its module's code, which a call by code index reaches.
        ModuleInstance const* instance = nullptr;  ///< Its instance.
        uint32_t instanceAddress = 0;              ///< The instance's address: the caller of a host callee.
        Slot* fp = nullptr;                        ///< Its first slot.
        /// Where its results go when it returns: its caller's arguments, in a chunk below its own that no
        /// tail call replaces; for the invocation's own function, its own first slot.
        Slot* results = nullptr;
        Word const* returnPc = nullptr; ///< Where its caller goes on; null for the invocation's own function.
        std::size_t chunk = 0;          ///< The chunk its slots are in.
    };

    /// The frame records of an invocation, a stack in blocks of FrameBlockSize: a push never copies the
    /// records already there, as a growing vector would (64 MiB at once, unpolled, a million frames deep),
    /// and a record stays where it is until it is popped. The blocks are kept for the frames entered later,
    /// and freed when the stack ends.
    class FrameStack
    {
      public:
        /// The records of one block.
        static constexpr auto FrameBlockSize = std::size_t { 1'024 };

        [[nodiscard]] bool empty() const noexcept { return _size == 0; }

        [[nodiscard]] std::size_t size() const noexcept { return _size; }

        /// The newest record. Precondition: the stack is not empty.
        [[nodiscard]] Frame& back() noexcept { return at(_size - 1); }

        /// The newest record. Precondition: the stack is not empty.
        [[nodiscard]] Frame const& back() const noexcept { return at(_size - 1); }

        /// The record @p index from the oldest.
        [[nodiscard]] Frame& at(std::size_t index) noexcept
        {
            return (*_blocks[index / FrameBlockSize])[index % FrameBlockSize];
        }

        /// The record @p index from the oldest.
        [[nodiscard]] Frame const& at(std::size_t index) const noexcept
        {
            return (*_blocks[index / FrameBlockSize])[index % FrameBlockSize];
        }

        /// Pushes @p frame, starting a block where the newest is full.
        void push(Frame const& frame)
        {
            if (_size == _blocks.size() * FrameBlockSize)
                _blocks.push_back(std::make_unique<std::array<Frame, FrameBlockSize>>());
            at(_size) = frame;
            ++_size;
        }

        /// Pops the newest record. Precondition: the stack is not empty.
        void pop() noexcept { --_size; }

      private:
        std::vector<std::unique_ptr<std::array<Frame, FrameBlockSize>>> _blocks;
        std::size_t _size = 0;
    };
} // namespace

/// One invocation's execution: its context, its frames and its slot chunks.
///
/// Frame records live in a FrameStack, never on the host stack: a call pushes one, a return pops one, and
/// lowered code runs on in the same dispatch loop, so no depth of Wasm calls recurses on the host stack. A
/// frame's slots are in a chunk: a callee's frame begins at its arguments, in its caller's chunk, where the
/// whole frame fits there, and at the start of the next chunk otherwise, its arguments copied there once (a
/// tail callee alone in its chunk takes that chunk). A chunk holds the frame's slots and at least
/// chunkSlotsAt() its index, is kept for the frames entered later, and is freed when the invocation ends; a
/// chunk too small for the frame that needs it is replaced, and every chunk after it freed. The bound this
/// keeps is docs/fast-interpreter.md's: 2 × A + min(2 × A, MaximumGrownChunkSlots) + FirstChunkSlots slots
/// per invocation, A the slots of its frames active at once.
///
/// Every frame it entered is given back when it ends, however execution stopped: the depth and the stack
/// charge of each (~Invocation()).
class Invocation
{
  public:
    /// @param store The store to execute on.
    /// @param executor What a host function it calls re-enters the guest through: the interpreter.
    /// @param depth The frames active over every invocation in progress; kept up to date.
    /// @param memoryRefreshes The times memory 0 was read again because a memory grew; counted on.
    /// @param dispatch The dispatch the code was lowered for.
    /// @param slotMemory What the slot chunks come from.
    Invocation(Store& store,
               runtime::ICodeExecutor& executor,
               uint32_t& depth,
               uint64_t& memoryRefreshes,
               Dispatch dispatch,
               std::pmr::memory_resource& slotMemory):
        _context { store, *this },
        _executor { executor },
        _depth { depth },
        _memoryRefreshes { memoryRefreshes },
        _dispatch { dispatch },
        _slotMemory { &slotMemory },
        _stepsLeft { _context.gate.reload() }
    {
    }

    Invocation(Invocation const&) = delete;
    Invocation(Invocation&&) = delete;
    Invocation& operator=(Invocation const&) = delete;
    Invocation& operator=(Invocation&&) = delete;

    /// Gives back the depth and the stack charges of the frames a trap left behind.
    ~Invocation()
    {
        _depth -= static_cast<uint32_t>(_frames.size());
        for (auto const index: std::views::iota(std::size_t { 0 }, _frames.size()))
            _context.store.releaseFrame(_frames.at(index).function->frameCost);
    }

    /// Invokes @p function, of @p type, with @p args and writes its results to @p results: a module
    /// function's code runs from its frame, which enters as every frame does; a host function runs through
    /// runtime::callHostFunction(), after its FunctionEntry poll, the embedder its caller.
    [[nodiscard]] std::expected<void, Abrupt> run(FuncAddr function,
                                                  binary::FuncType const& type,
                                                  std::span<Value const> args,
                                                  std::span<Value> results)
    {
        auto const callee = calleeAt(function);
        if (auto const polled = _context.gate.poll(PollSite::FunctionEntry, _stepsLeft); !polled)
            return stop(polled.error());
        if (callee.host != nullptr)
        {
            // Committed first: the host call charges, polls and may invoke the guest through the store.
            _context.gate.commit(_stepsLeft);
            return runtime::callHostFunction(
                _context.store, _executor, *callee.host, std::nullopt, args, results);
        }
        auto const entered = enterFrame(callee, nullptr, NoChunk, 0, nullptr, nullptr);
        if (!entered)
            return stop(entered.error());
        storeValues(type.params, args, *entered);
        auto state = State { .pc = callee.function->code.data(),
                             .fp = *entered,
                             .context = &_context,
                             .memory0 = nullptr,
                             .memory0Size = 0,
                             .stepsLeft = _stepsLeft };
        loadMemory0(state);
        auto const exit = Runners[std::to_underlying(_dispatch)](state);
        switch (exit)
        {
            case Exit::Returned: break;
            case Exit::Trapped: return std::unexpected { *_context.abrupt };
            // uncovered-begin(no-exceptions-yet)
            case Exit::Thrown:
                // No operation throws before the fast interpreter's exceptions are lowered.
                core::unreachable();
                // uncovered-end(no-exceptions-yet)
        }
        loadValues(type.results, _context.results, results);
        return {};
    }

    /// Calls what @p site names from the current frame (callFunction()).
    [[nodiscard]] OutOfLineStep call(State state, CallSite const& site) noexcept
    {
        // An indirect callee is resolved, and its traps raised, before anything else: before the frame a
        // tail call replaces ends, and before the FunctionEntry poll.
        auto const callee = resolve(state, site);
        if (!callee)
            return stopping(state, leaveTrapped(_context, state.stepsLeft, callee.error()));
        auto* const arguments = state.fp + site.argumentSlot;
        if (site.mode == CallMode::Tail)
            return tailCall(state, *callee, arguments);
        // Copied: entering the callee's frame grows the frames, which may move them.
        auto const caller = _frames.back();
        if (auto const polled = _context.gate.poll(PollSite::FunctionEntry, state.stepsLeft); !polled)
            return stopping(state, leaveTrapped(_context, state.stepsLeft, polled.error()));
        if (callee->host != nullptr)
        {
            if (auto const called =
                    callHost(state.stepsLeft, *callee->host, caller.instanceAddress, arguments, arguments);
                !called)
                return stopping(state, leaveAbruptly(_context, state.stepsLeft, called.error()));
            // The host may have grown a memory, directly or through a guest it invoked.
            refreshMemory0(state);
            state.pc += site.words;
            return { .state = state, .flow = Flow::Continue };
        }
        auto const entered =
            enterFrame(*callee, arguments, caller.chunk, caller.chunk + 1, state.pc + site.words, arguments);
        if (!entered)
            return stopping(state, leaveTrapped(_context, state.stepsLeft, entered.error()));
        state.pc = callee->function->code.data();
        state.fp = *entered;
        enterMemory0(state, caller.instance);
        return { .state = state, .flow = Flow::Continue };
    }

    /// Returns from the current frame (returnFromFunction()).
    [[nodiscard]] OutOfLineStep returnFrom(State state, uint32_t resultSlot) noexcept
    {
        auto const* const results = state.fp + resultSlot;
        if (_frames.size() == 1)
            return stopping(state, leaveReturning(_context, state.stepsLeft, results));
        auto const ending = _frames.back();
        std::memmove(ending.results, results, std::size_t { ending.function->resultSlots } * sizeof(Slot));
        leaveFrame();
        return resumeCaller(state, ending.returnPc, ending.instance);
    }

    /// Runs memory.grow from the current frame (growMemory()).
    [[nodiscard]] OutOfLineStep grow(State state,
                                     uint32_t resultSlot,
                                     uint32_t deltaSlot,
                                     uint32_t memory) noexcept
    {
        auto const address = _context.instance->memories[memory];
        auto const addressType = _context.store.memory(address).type.limits.addressType;
        // An i32 delta is held zero-extended already; the result must be too, as an access that takes it for
        // its address relies on (accessMemory()): memory.grow's -1 is all ones of the address type.
        auto const grown = _context.store.growMemory(address, state.fp[deltaSlot]).value_or(RefusedGrowth);
        state.fp[resultSlot] = narrowToAddressType(grown, addressType);
        // Whatever grew, memory 0 may be the memory that did, under another index too.
        refreshMemory0(state);
        return { .state = state, .flow = Flow::Continue };
    }

    /// The current frame's instance, as the store holds it: what a drop changes.
    [[nodiscard]] ModuleInstance& currentInstance() noexcept
    {
        return _context.store.moduleInstance(_frames.back().instanceAddress);
    }

  private:
    /// The callee of a function address: a host function, or a module function and its code.
    [[nodiscard]] Callee calleeAt(FuncAddr address) const
    {
        auto const& function = _context.store.function(address);
        if (function.isHost())
            return Callee { .host = &function };
        auto const& instance = _context.store.moduleInstance(function.moduleInstance);
        auto const& module = loweredModuleOf(instance, _dispatch);
        return Callee { .instance = &instance,
                        .instanceAddress = function.moduleInstance,
                        .module = &module,
                        .function = &module.functions()[function.codeIndex] };
    }

    /// The callee @p site names in the current frame: a function of its own module by code index, any
    /// other through the store. A table element and a reference are checked as the reference interpreter
    /// checks them, in its order.
    /// @return The callee, or the trap: UndefinedElement and UninitializedElement naming the element,
    ///         IndirectCallTypeMismatch, or NullFunctionReference.
    [[nodiscard]] std::expected<Callee, Trap> resolve(State const& state, CallSite const& site) const
    {
        auto const& frame = _frames.back();
        auto const& instance = *frame.instance;
        switch (site.callee)
        {
            case CalleeKind::Internal:
                return Callee { .instance = frame.instance,
                                .instanceAddress = frame.instanceAddress,
                                .module = frame.module,
                                .function = &frame.module->functions()[site.index] };
            case CalleeKind::Import: return calleeAt(instance.functions[site.index]);
            case CalleeKind::Table: {
                auto const& table = _context.store.table(instance.tables[site.table]);
                // A table of i32 addresses reads its index as 32 bits, whatever the slot's high bits hold.
                auto const index =
                    narrowToAddressType(state.fp[site.operandSlot], table.type.limits.addressType);
                // Spectre: the element is read at the masked index (elementChecked()), not at the guest's.
                auto const checked = elementChecked(index, table.elements.size());
                if (!checked)
                    return std::unexpected { Trap { TrapCode::UndefinedElement,
                                                    std::bit_cast<int64_t>(index) } };
                auto const& element = table.elements[*checked];
                if (element.kind == runtime::RefKind::Null)
                    return std::unexpected { Trap { TrapCode::UninitializedElement,
                                                    std::bit_cast<int64_t>(index) } };
                if (_context.store.function(element.address).typeId != instance.types[site.index])
                    return std::unexpected { Trap { TrapCode::IndirectCallTypeMismatch } };
                return calleeAt(element.address);
            }
            case CalleeKind::Reference: break;
        }
        // A reference's slots are its tag word, then its address (storeValue()).
        if (isNullReference(state.fp[site.operandSlot]))
            return std::unexpected { Trap { TrapCode::NullFunctionReference } };
        auto const address = static_cast<FuncAddr>(state.fp[site.operandSlot + 1]);
        // Validation typed the reference, and every reference from a host is checked against its type, so
        // a function of another type is the interpreter's own error (the reference interpreter's too).
        Require(_context.store.function(address).typeId == instance.types[site.index]);
        return calleeAt(address);
    }

    /// Enters the Wasm frame of @p callee: the one routine every Wasm frame goes through, right after the
    /// FunctionEntry poll its caller took (a call polls before it tells a host callee from a Wasm one). It
    /// checks StoreLimits::maxCallDepth (one count over every invocation in progress), then charges the
    /// frame's static cost (Store::chargeFrame()), which leaveFrame() or ~Invocation() gives back; only then
    /// does it place the frame's slots and initialize its declared locals, so a frame refused at its entry
    /// allocates nothing.
    /// @param callee The module function.
    /// @param arguments Its arguments, in consecutive slots of @p argumentsChunk; null for the invocation's
    ///        own function, whose arguments the caller stores into the frame.
    /// @param argumentsChunk The chunk of @p arguments; NoChunk for the invocation's own function.
    /// @param target The chunk the frame takes where it does not fit after its arguments (place()).
    /// @param returnPc Where the caller goes on; null for the invocation's own function.
    /// @param results Where its results go when it returns; null for the invocation's own function.
    /// @return The frame's first slot; or Trap{CallStackExhausted}, past the depth or the stack budget.
    [[nodiscard]] std::expected<Slot*, Trap> enterFrame(Callee const& callee,
                                                        Slot* arguments,
                                                        std::size_t argumentsChunk,
                                                        std::size_t target,
                                                        Word const* returnPc,
                                                        Slot* results) noexcept
    {
        if (_depth >= _context.store.limits().maxCallDepth)
            return std::unexpected { Trap { TrapCode::CallStackExhausted } };
        auto const& function = *callee.function;
        if (auto const charged = _context.store.chargeFrame(function.frameCost); !charged)
            return std::unexpected { charged.error() };
        auto const [fp, chunk] = place(function, arguments, argumentsChunk, target);
        _frames.push(Frame { .function = &function,
                             .module = callee.module,
                             .instance = callee.instance,
                             .instanceAddress = callee.instanceAddress,
                             .fp = fp,
                             .results = results != nullptr ? results : fp,
                             .returnPc = returnPc,
                             .chunk = chunk });
        ++_depth;
        initializeLocals(*callee.instance, function, fp);
        _context.instance = callee.instance;
        _context.code = function.code.data();
        return fp;
    }

    /// Where the frame of @p function goes, its arguments at @p arguments in @p argumentsChunk: there, where
    /// the whole frame fits in the chunk; else at the start of chunk @p target, which is reused where it is
    /// large enough and replaced (every chunk after it freed) where it is not, the arguments copied there
    /// first. A new chunk holds the frame's slots, and at least chunkSlotsAt(@p target).
    /// @param target The chunk after the arguments' (a call), or theirs where no other frame is left in it
    ///        (a tail call), or 0 for the invocation's own function.
    /// @return The frame's first slot, and its chunk.
    [[nodiscard]] std::pair<Slot*, std::size_t> place(LoweredFunction const& function,
                                                      Slot* arguments,
                                                      std::size_t argumentsChunk,
                                                      std::size_t target) noexcept
    {
        if (argumentsChunk != NoChunk)
        {
            auto const& chunk = _chunks[argumentsChunk];
            auto const offset = static_cast<uint64_t>(arguments - chunk.slots());
            if (chunk.count() - offset >= function.frameSlots)
                return { arguments, argumentsChunk };
        }
        auto const needed = std::max(chunkSlotsAt(target), uint64_t { function.frameSlots });
        if (target < _chunks.size() && _chunks[target].count() >= needed)
        {
            // Only a call reuses a chunk: its arguments are in the chunk before, and a tail callee alone in a
            // chunk large enough fits after its arguments, at the chunk's start, already.
            auto* const fp = _chunks[target].slots();
            std::copy_n(arguments, function.paramSlots, fp);
            return { fp, target };
        }
        // No chunk after the arguments' holds an active frame, so they go before the new one is allocated:
        // only a tail callee's arguments may be in the chunk it replaces, which is freed once they are
        // copied.
        auto const keep = argumentsChunk == target ? target + 1 : target;
        while (_chunks.size() > keep)
            _chunks.pop_back();
        auto replacement = SlotChunk { _slotMemory, needed };
        if (arguments != nullptr)
            std::copy_n(arguments, function.paramSlots, replacement.slots());
        while (_chunks.size() > target)
            _chunks.pop_back();
        _chunks.push_back(std::move(replacement));
        return { _chunks.back().slots(), target };
    }

    /// The slots a new chunk @p index holds at least: FirstChunkSlots for an invocation's first, twice the
    /// chunk before for every later one, up to MaximumGrownChunkSlots. So an invocation that calls little
    /// (a host re-entry, say) holds a small chunk, and one that recurses deep grows to large ones quickly.
    [[nodiscard]] uint64_t chunkSlotsAt(std::size_t index) const noexcept
    {
        if (index == 0)
            return FirstChunkSlots;
        return std::min(MaximumGrownChunkSlots, 2 * _chunks[index - 1].count());
    }

    /// Sets @p function's declared locals to their defaults: a Zero run's slots to zero, a Null run's
    /// pairs to the null of its heap, which nullSlots() maps through @p instance's TypeIds once per run.
    static void initializeLocals(ModuleInstance const& instance, LoweredFunction const& function, Slot* fp)
    {
        for (auto const& run: function.localInit)
        {
            auto const slots = std::span { fp + run.slotBase, run.slotCount };
            switch (run.kind)
            {
                case LocalInit::Zero: std::ranges::fill(slots, Slot { 0 }); break;
                case LocalInit::Null: fillPairs(slots, nullSlots(run.heap, instance.types)); break;
            }
        }
    }

    /// Ends the current frame: gives back its depth and its stack charge.
    void leaveFrame() noexcept
    {
        _context.store.releaseFrame(_frames.back().function->frameCost);
        _frames.pop();
        --_depth;
    }

    /// Goes on in the frame now current at @p returnPc, after the call that left it, whose registers held
    /// the memory 0 of @p from: memory 0 is the caller's again.
    [[nodiscard]] OutOfLineStep resumeCaller(State state,
                                             Word const* returnPc,
                                             ModuleInstance const* from) noexcept
    {
        auto const& caller = _frames.back();
        state.pc = returnPc;
        state.fp = caller.fp;
        _context.instance = caller.instance;
        _context.code = caller.function->code.data();
        enterMemory0(state, from);
        return { .state = state, .flow = Flow::Continue };
    }

    /// Reads memory 0 of the current frame's instance into the registers @p state: its bytes and size, or
    /// none where it has no memory; and records the generation they are of.
    void loadMemory0(State& state) noexcept
    {
        _context.memoryGeneration = _context.store.memoryGeneration();
        auto const& memories = _context.instance->memories;
        if (memories.empty())
        {
            state.memory0 = nullptr;
            state.memory0Size = 0;
            return;
        }
        auto const bytes = _context.store.memory(memories.front()).bytes();
        state.memory0 = bytes.data();
        state.memory0Size = bytes.size();
    }

    /// Reads memory 0 into the registers @p state again where a memory grew since they were read
    /// (Store::memoryGeneration()), as code the executor does not see may have grown it: a host function, or
    /// a memory.grow of another index. Counted (FastInterpreter::memoryRefreshes()).
    void refreshMemory0(State& state) noexcept
    {
        if (_context.store.memoryGeneration() == _context.memoryGeneration)
            return;
        ++_memoryRefreshes;
        loadMemory0(state);
    }

    /// Makes the registers @p state hold the current frame's memory 0, where they held @p from's: read
    /// afresh where the frame is another instance's (a call into, or a return from, another instance),
    /// refreshed where it is the same instance's.
    void enterMemory0(State& state, ModuleInstance const* from) noexcept
    {
        if (_context.instance != from)
            loadMemory0(state);
        else
            refreshMemory0(state);
    }

    /// Tail-calls @p callee, whose arguments are at @p arguments: the current frame ends first, its charge
    /// given back, and the callee enters in its place. A Wasm callee's frame replaces it, at its first slot,
    /// so neither the depth nor the slots grow; a host callee runs, and its results are the frame's.
    [[nodiscard]] OutOfLineStep tailCall(State state, Callee const& callee, Slot* arguments) noexcept
    {
        auto const ending = _frames.back();
        if (callee.host == nullptr)
            std::memmove(ending.fp, arguments, std::size_t { callee.function->paramSlots } * sizeof(Slot));
        leaveFrame();
        if (auto const polled = _context.gate.poll(PollSite::FunctionEntry, state.stepsLeft); !polled)
            return stopping(state, leaveTrapped(_context, state.stepsLeft, polled.error()));
        if (callee.host != nullptr)
        {
            // A host callee is called by the instance whose frame it replaces.
            if (auto const called = callHost(
                    state.stepsLeft, *callee.host, ending.instanceAddress, arguments, ending.results);
                !called)
                return stopping(state, leaveAbruptly(_context, state.stepsLeft, called.error()));
            if (_frames.empty())
                return stopping(state, leaveReturning(_context, state.stepsLeft, ending.results));
            return resumeCaller(state, ending.returnPc, ending.instance);
        }
        // Where no other frame is left in the ending frame's chunk, the callee takes that chunk, replacing it
        // where it is too small: a chain of ever larger tail-called frames holds one chunk, not one each.
        auto const alone = _frames.empty() || _frames.back().chunk != ending.chunk;
        auto const target = alone ? ending.chunk : ending.chunk + 1;
        // The invocation's own frame keeps its results in its own slots, which the replaced chunk may hold:
        // its callee's go to the callee's own first slot (null), wherever that lands. Any other frame's go to
        // its caller's arguments, in a chunk below, which no tail call replaces.
        auto* const results = _frames.empty() ? nullptr : ending.results;
        auto const entered = enterFrame(callee, ending.fp, ending.chunk, target, ending.returnPc, results);
        if (!entered)
            return stopping(state, leaveTrapped(_context, state.stepsLeft, entered.error()));
        state.pc = callee.function->code.data();
        state.fp = *entered;
        enterMemory0(state, ending.instance);
        return { .state = state, .flow = Flow::Continue };
    }

    /// Runs the host function @p function, its arguments in slots from @p arguments, its results to slots
    /// from @p results, through runtime::callHostFunction(), @p caller its caller: the countdown committed
    /// before, since the host frame charges, polls and may invoke the guest through the store, and reloaded
    /// after.
    /// @return Nothing, or how the call ended abruptly.
    [[nodiscard]] std::expected<void, Abrupt> callHost(uint64_t& stepsLeft,
                                                       FunctionInstance const& function,
                                                       uint32_t caller,
                                                       Slot const* arguments,
                                                       Slot* results) noexcept
    {
        auto const& type = _context.store.type(function.typeId);
        _hostArguments.resize(type.params.size());
        _hostResults.resize(type.results.size());
        loadValues(type.params, arguments, _hostArguments);
        _context.gate.commit(stepsLeft);
        auto const done = runtime::callHostFunction(
            _context.store, _executor, function, caller, _hostArguments, _hostResults);
        stepsLeft = _context.gate.reload();
        if (!done)
        {
            if (std::holds_alternative<runtime::Thrown>(done.error()))
                // uncovered-begin(no-exceptions-yet)
                core::todo("a host function's exception unwinds the fast interpreter's frames with its "
                           "exceptions (Task 13)");
            // uncovered-end(no-exceptions-yet)
            return done;
        }
        storeValues(type.results, _hostResults, results);
        return {};
    }

    /// Stops before any code ran: commits the countdown, as every way out does, and ends with @p trap.
    [[nodiscard]] std::unexpected<Abrupt> stop(Trap trap) noexcept
    {
        _context.gate.commit(_stepsLeft);
        return std::unexpected { Abrupt { trap } };
    }

    /// The registers @p state and the stopping @p flow, which left the context already.
    [[nodiscard]] static OutOfLineStep stopping(State state, Flow flow) noexcept
    {
        return { .state = state, .flow = flow };
    }

    Context _context;
    runtime::ICodeExecutor& _executor;
    uint32_t& _depth;
    uint64_t& _memoryRefreshes;
    Dispatch _dispatch;
    std::pmr::memory_resource* _slotMemory;
    uint64_t _stepsLeft;
    std::vector<SlotChunk> _chunks;    ///< The slot chunks, in the order the frames reached them.
    FrameStack _frames;                ///< The active frames, the invocation's own function first.
    std::vector<Value> _hostArguments; ///< A host call's arguments, reused by every host call.
    std::vector<Value> _hostResults;   ///< A host call's results, reused by every host call.
};

OutOfLineStep callFunction(State state, CallSite const& site) noexcept
{
    return state.context->invocation.call(state, site);
}

OutOfLineStep returnFromFunction(State state, uint32_t resultSlot) noexcept
{
    return state.context->invocation.returnFrom(state, resultSlot);
}

OutOfLineStep growMemory(State state, uint32_t resultSlot, uint32_t deltaSlot, uint32_t memory) noexcept
{
    return state.context->invocation.grow(state, resultSlot, deltaSlot, memory);
}

void dropDataSegment(Context& context, uint32_t segment) noexcept
{
    context.invocation.currentInstance().data[segment].reset();
}

void dropElementSegment(Context& context, uint32_t segment) noexcept
{
    context.invocation.currentInstance().elements[segment].reset();
}

Flow leaveTrapped(Context& context, uint64_t stepsLeft, Trap trap) noexcept
{
    return leaveAbruptly(context, stepsLeft, Abrupt { trap });
}

Flow leaveAbruptly(Context& context, uint64_t stepsLeft, Abrupt abrupt) noexcept
{
    context.gate.commit(stepsLeft);
    context.abrupt = abrupt;
    return Flow::Trapped;
}

Flow leaveReturning(Context& context, uint64_t stepsLeft, Slot const* results) noexcept
{
    context.gate.commit(stepsLeft);
    context.results = results;
    return Flow::Returned;
}

FastInterpreter::FastInterpreter(Dispatch dispatch):
    FastInterpreter { dispatch, *std::pmr::new_delete_resource() }
{
}

FastInterpreter::FastInterpreter(Dispatch dispatch, std::pmr::memory_resource& slotMemory):
    _dispatch { dispatch }, _slotMemory { &slotMemory }
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

uint64_t FastInterpreter::memoryRefreshes() const noexcept
{
    return _memoryRefreshes;
}

std::expected<void, Abrupt> FastInterpreter::invoke(Store& store,
                                                    FuncAddr function,
                                                    std::span<Value const> args,
                                                    std::span<Value> results)
{
    // Float results are the spec's only in the default floating-point environment, which the
    // invocation's scope establishes (runtime::FloatEnvironmentScope).
    assert(std::fegetround() == FE_TONEAREST);
    // The invocation's scope, its nesting limit and its step budget, is the entry's to open.
    Require(store.invocationDepth() > 0);
    auto const& type = store.functionType(function);
    // The caller's to check: arguments of the function's types, references in this store.
    Require(runtime::checkValues(store, args, type.params).has_value());
    Require(results.size() == type.results.size());
    return Invocation { store, *this, _depth, _memoryRefreshes, _dispatch, *_slotMemory }.run(
        function, type, args, results);
}

} // namespace isoline::fastinterpreter
