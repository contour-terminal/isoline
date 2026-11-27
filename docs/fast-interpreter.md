# The fast interpreter

The fast interpreter runs code lowered once, at load, from a validated module into operations over frame
slots. This document grows with it; its sections state the design decisions an embedder or a contributor
can rely on.

## The lowering arena and the compile budget

Lowering charges every byte it allocates to the module's compile budget, through a lowering arena
(`runtime::ICountingArena`, `runtime::CountingArena`): a `std::pmr::memory_resource` that counts what it
charges, lets lowering reserve a bound before the work it covers, and refuses past the budget by throwing
`std::bad_alloc`. `lower()` catches the refusal at its boundary and refuses the module with a
`LoweringError`, which says whether the budget or the host's memory ran out. So a module over the budget is
refused at its first allocation past it, never after its peak.

**Every container of lowering and of lowered code allocates through `LoweringAllocator`**
(`fastinterpreter/LoweringAllocator.hpp`), never through `std::pmr::polymorphic_allocator` directly. It
splits what a container allocates in two:

- **Its own storage**, allocated under the container's element type: charged to the arena, and refusable.
  A vector allocates its storage only in operations that may throw (`reserve`, `push_back`, a copy), so a
  refusal always reaches `lower()` as an exception it handles.
- **A standard library's bookkeeping**, allocated under a type the library rebinds the allocator to:
  neither charged nor refused, but taken from the process heap (`std::pmr::new_delete_resource()`).

The reason is MSVC's debug containers. With `_ITERATOR_DEBUG_LEVEL` above 0, the MSVC STL allocates a
`std::_Container_proxy` of two pointers for each container, through the container's allocator rebound to
it. It does so in the allocator-taking constructor and in the moves, which are `noexcept`. Through the
arena that broke both of the arena's promises:

- **A refusal inside a `noexcept` operation ends the process.** A budget refusal, or a host refusal the
  arena passes on, ends in `std::terminate`. A debug build an embedder ships would crash where a release
  build refuses the module.
- **The budget would differ by standard library.** libstdc++ and libc++ make no such allocation, so the
  same module would be charged 16 bytes more per container on MSVC's debug STL, and accounting tests could
  not state an exact expectation.

Routed around the arena, the bookkeeping is never refused. Every byte the arena counts on libstdc++, libc++
and the MSVC STL alike is one lowering asked for. The tests derive their expectations from the arena's own
accounting (`loweringReservation()`, `charged()`, `peak()`), never from `sizeof` of a standard type.

The split relies on a container allocating its storage as its element type, which is a vector's kind.
A node-based container (`std::unordered_map`, `std::list`) allocates its nodes and buckets under rebound
types too, which no allocator can tell from bookkeeping. So lowering uses none.

Its one map, from a forwarded local to the newest operand stack entry forwarding it, is no hash map
either. It is a `LoweringVector` indexed by local, sized to the function's locals and allocated before
the body is lowered, where a refusal is caught. Finding a local's chain is then O(1) in the worst case,
whatever locals a body forwards. An open-addressing table over a published hash could be clustered on
purpose: a 1 MB body forwarding 16'383 locals hashed into one half of the table, then setting locals hashed
into that run, took 10^9 probes. The table costs O(locals) per function, once, which the frame's own slots
for those locals already bound (`binary::DecodeLimits::maxLocals`).
`Lowering.forwardedLocalsCostTheSameHoweverChosen` keeps it so: the clustered shape must lower as fast as
one whose locals are spread, within a factor of 3. The hash map took 27 times as long.

*Rejected:* charging the bookkeeping too, and tolerating it in the tests. The `noexcept` refusals would
remain. *Rejected:* building with `_ITERATOR_DEBUG_LEVEL=0`. It changes the ABI, which an embedder's debug
build does not share. *Rejected:* refusing only at reservation points, with allocations that never throw.
The temporaries and a standard library's growth pattern cannot all be reserved exactly, and a budget
checked only at checkpoints would let the peak pass it.

## Calls, frames and the slot stack

**Calls never recurse on the host stack.** A call operation hands its call to the executor out of line
(`callFunction()`), which pushes a frame record of the invocation and returns the callee's registers; the
same dispatch loop then runs the callee, and a return pops the record and resumes the caller. A million
Wasm frames are a million records in a vector, on any host stack: `Calls.deepRecursionDoesNotRecurseOnTheHostStack`
runs a million frames deep, and `tests/stack/StackLimit.cmake` runs it again under a 1 MiB main-thread stack on
Linux. Only a host function that invokes the guest again nests on the host stack, one invocation per level,
which `StoreLimits::maxNestedInvocations` bounds; `Dispatch.nestedInvocationsFitASmallThread` runs every level on
a small thread (`runtime/testing/SmallStackThread.hpp`: 512 KiB where the dispatch files' frames are bounded,
larger in sanitizer, coverage and unoptimized builds).

**Every frame enters through one routine, in the reference interpreter's order.** The call operation names
its callee by data (`CallTable`): a function of the caller's own module by code index, with no look-up
through the store; an imported one by function index; a table element; or a function reference. A table
element or a reference is checked first, before anything else: `UndefinedElement` and
`UninitializedElement` naming the element, `IndirectCallTypeMismatch`, `NullFunctionReference`, with no
FunctionEntry poll for the callee. Then the FunctionEntry poll; then, for a Wasm callee, the depth check
against `maxCallDepth`, one count over every invocation in progress, host-nested ones included; then the
frame's static charge (`Store::chargeFrame()`); and only then are its slots placed. So when the steps and the
depth, or the steps and the stack budget, run out at the same entry, both engines trap with
`StepLimitExceeded` (`Calls.tiesFollowTheEntryOrder`). A tail call ends its caller's frame, its charge given
back, before the callee's entry polls and charges, so `return_call` recursion runs in one frame's budget.
Every frame's charge is given back however it ends, a trap included.

**A host callee runs through `runtime::callHostFunction()`**, the one host-frame routine of every engine:
charge `hostFrameCost`, escape every argument (`Store::markEscaped()`), call under a `HostCallScope` with a
`HostContext`, check the outcome, give the charge back, poll at HostReturn. The executor commits its step
countdown before and reloads it after, since the host frame charges, polls and may invoke the guest through
the store. The embedder's own invocation of a host function polls at FunctionEntry and runs it the same way,
with no caller instance.

**The slot stack is per invocation and chunked.** A callee's frame begins at its arguments, in the caller's
frame, where the whole frame fits in the caller's chunk; otherwise it starts the next chunk, and its
arguments, and nothing else, are copied there. A chunk holds `max(MinimumChunkSlots, frame slots)` slots, is
kept for the frames entered later, and is freed when the invocation ends; a chunk too small for the frame
that needs it is replaced, and every chunk after it freed. Each chunk but the last either holds a frame its
own size, or was left because the next frame did not fit, so the chunks hold at most twice the slots of the
frames active at once, plus one chunk. A frame takes at most two slots per value its charge counts (one per
number, two per vector or reference), so the slot stack is at most `2 × maxStackValues` slots plus one chunk
where every value is a number, and `4 × maxStackValues` plus one chunk in the worst case.
(`Calls.newChunkCopiesOnlyTheArguments`). Running out of host memory for a chunk terminates, as running out
of it does anywhere in `runtime::invokeChecked()`.
