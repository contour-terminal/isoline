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
same dispatch loop then runs the callee, and a return pops the record and resumes the caller. The records are
a stack in blocks of 1'024, so a push never copies the records below it (a growing vector would copy 64 MiB at
once, a million frames deep, with no poll). A million Wasm frames are a million records, on any host stack:
`Calls.deepRecursionDoesNotRecurseOnTheHostStack` runs a million frames deep, alone, under the memory caps and,
on Linux, a 1 MiB main-thread stack (`tests/stack/StackLimit.cmake`).

**Only a host function that invokes the guest again nests on the host stack**, one invocation per level, which
`StoreLimits::maxNestedInvocations` bounds. What one nesting takes is measured per engine and build flavour
(the hidden case `Calls.measureNestingStack`, the distance between the host frames of consecutive nestings) and
published as `runtime::nestedInvocationBytes()`, generated from the flavour's row in `src/isoline/CMakeLists.txt`:
the most measured, rounded up by half again. `Calls.nestingStackCostIsPublished` checks every build against its
row, and `Dispatch.nestedInvocationsFitASmallThread` runs every level on a small thread
(`runtime/testing/SmallStackThread.hpp`) of the flavour's row. Measured, in bytes per nesting (the fast engine's
under Switch and TailCall):

| Flavour | Builds measured | Reference | Fast | Published (reference, fast) | Small thread |
|---|---|---|---|---|---|
| release | clang-release | 1'472 | 1'984, 2'016 | 2'304, 3'072 | 512 KiB |
| debug | gcc-debug; clangcl-cross-debug (wine); clang-aarch64-cross (qemu); msvc-debug, real cl.exe (wine) | 3'712; 5'344; 4'784; 8'592 | 3'344, 3'440; 4'992, 5'168; 4'656, 4'752; 8'352 (Switch only) | 13'056, 12'544 | 3 MiB |
| address | clang-asan | 8'096 | 7'104, 7'424 | 12'288, 11'264 | 3 MiB |
| thread | clang-tsan | 7'040 | 6'400, 6'320 | 10'752, 9'728 | 3 MiB |
| coverage | clang-coverage | 4'816 | 4'624, 4'704 | 7'424, 7'168 | 2 MiB |

A multi-config generator's build and MSVC's take the debug row: their frames no option bounds. The rows are this
machine's toolchains' (Fedora's Clang 22 and GCC 16, clang-cl over the MSVC 2022 headers); before the fast
interpreter merges, each row is raised to the most CI's GCC 14, Apple Clang and MSVC jobs measure, which
`Calls.nestingStackCostIsPublished` prints with the published figure on every run.

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
frame, where the whole frame fits in the caller's chunk; otherwise it starts the next chunk, and its arguments,
and nothing else, are copied there. A tail callee left alone in its chunk (no other frame of the invocation is
in it) takes that chunk instead, replacing it where it is too small, so a chain of ever larger tail-called frames
holds one chunk, not one per frame (`Calls.tailChainKeepsOneChunk`). An invocation's first chunk holds at least
`FirstChunkSlots` (256) slots, and every later one at least twice the one before it, up to
`MaximumGrownChunkSlots` (65'536), and at least its first frame's slots. A chunk is kept for the frames entered
later and freed when the invocation ends; a chunk too small for the frame that needs it is replaced, every chunk
after it freed first.

The bound, per invocation in progress, with A the slots of its frames active at once (each frame's whole
`frameSlots`, though a caller's frame and its callee's overlap at the arguments):

    chunk slots ≤ 2 × A + min(2 × A, MaximumGrownChunkSlots) + FirstChunkSlots

Each chunk but the last was left because the next frame did not fit after the frames in it, so it holds fewer
slots than its own frames and the next chunk's first frame together: all of them hold fewer than 2 × A. The last
holds its first frame, or at most twice the chunk before it, at most `MaximumGrownChunkSlots`; or, as an
invocation's first, `FirstChunkSlots`. While a tail callee's chunk is replaced, the smaller chunk it replaces is
held too, until the arguments are copied. So each nested invocation of one small frame costs `FirstChunkSlots`
(2 KiB), not a grown chunk (`Calls.nestedInvocationsHoldSmallChunks`, 100 levels).

The stack budget is charged in values, the same in every engine (`StoreLimits::maxStackValues`); a value takes one
slot as a number and two as a vector or a reference (`SlotBytesPerValue`, 16 bytes). A frame's slots are its
highest operand stack, and at least its locals' and its results' slots: a frame holds its results whether or not
its body pushes them, which a body ending in a `return_call` or in dead code does not, though a host function it
tail-calls writes them there. The charge (`ValidatedModule::frameCost()`) counts only what the body pushes, so
such a frame's result slots are beyond it; but every frame's results are counted in its caller's charge, where a
call pushes them (a tail call passes the caller's own on), all but the invocation's own function's. So A is at
most twice the active frames' charge for their operands and locals, plus twice it again for the results, plus the
invocation's own function's result slots (at most `2 × DecodeLimits::maxResults`): `A ≤ 4 × charge + 2'000`
slots, and `A ≤ 2 × charge + 1'000` where every value is a number. The chunks hold at most the bound above of
that A, per invocation in progress (`Calls.newChunkCopiesOnlyTheArguments`, `Calls.framesHoldTheirResults`).
The reference interpreter takes `interpreter::ValueBytes` (one `runtime::Value`) per value, on vectors that grow
as needed: it writes no fixed frame, so it has no such overflow to guard against, and its own results take the
same uncharged room. Running out of host memory for a chunk or a block of
frame records terminates, as running out of it does anywhere in `runtime::invokeChecked()`.

## Linear memory and the cached memory 0

**Memory 0 lives in the registers.** Its base and size are two of the handler arguments (`State::memory0`,
`State::memory0Size`), so a load or store of memory 0 reads no memory before it reaches the bytes. Each scalar
row of `binary::MemoryAccessTable` lowers to two operations of its own, one for a memory of i32 addresses and a
`.memory64` twin for i64 addresses; an access to any other memory is `load.indexed` or `store.indexed`, which
names the memory and the memory 0 operation whose body it runs over that memory's bytes, read from the store
each time. `memory.size` and `memory.grow` go through the store too. The vector accesses are rows of the same
table and lower alike: `v128.load` and its extending, splatting, zeroing and lane forms, `v128.store` and its
lane forms, each with its `.memory64` twin, reach another memory through `load.indexed` and `store.indexed` too,
whose word then also names the lane. A lane load replaces one lane of the vector above its address: lowering
moves that vector to its home slots, right above the address, where the access reads it before the result
replaces both.

**Bounds checks are explicit and never overflow.** For i32 addresses the address and the offset are below
2^32, so `address + offset + width ≤ size` is exact in 64 bits. For memory64 the sum is checked for its carry
and the width compared against what is left. Custom page sizes need nothing of their own: the check is in
bytes. Every access traps exactly where the reference interpreter does, with the same `Trap`
(`Memory.boundsMatchReferenceAtTheEdges`: every row, scalar and vector, at both edges of both address types in
memory 0 and memory 1, the sums that reach or pass 2^32 and 2^64, pages of one byte, memories of no pages and of
15, 16 and 17 bytes for the 16-byte accesses). A vector access is checked by the same `boundsChecked()` at its own
width, 16 bytes for `v128.load` and `v128.store`, the lane's for a lane, splat or zero access, 8 for an extending
load; no vector access has a check or a mask of its own.

**Spectre.** On the checked path the effective address is masked by a data-dependent select: all ones where
the access fits, zero where it does not, so code running ahead of a mispredicted check reads memory's first
bytes and nothing past them. The mask is `runtime::rangeMask()` of the access's bytes, derived from the same
bound as the check but not from its outcome, which the compiler would know on the predicted path and fold
away: the sign of the bytes left after the access, or'ed with the sign of the bytes left after its offset and
with the sign of the offset itself. An offset of 2^63 or more never fits (memories are allocated, so their sizes
stay below 2^63) and its bytes left would otherwise wrap to a positive count, a speculative read below the base
the guest chooses; a memory smaller than the access leaves its bytes left after the access negative for every
offset below 2^63 (`accessMask()`, `Memory.accessMaskAtTheEdges`, whose rows include the 16-byte width at
memories of 0, 15, 16 and 17 bytes and offsets near 2^32 and 2^64). What remains is the
masked access itself, which reads the access's width of bytes at the memory's base: for a memory smaller than
the access, code ahead of the check reads past its end: `runtime::MaxAccessWidth` bytes past a memory of none,
whatever its pages. They are the memory's own: a memory of `size` bytes keeps `MaxAccessWidth − min(size,
MaxAccessWidth)` bytes past its end, readable or guarded, at every size it grows to (`requiredTailBytesOf()`,
`ILinearMemory::tailBytes()`). `HeapMemoryAllocator` keeps `MaxAccessWidth` zeroed bytes past every memory's end,
and the store refuses, with `Refusal::Guard`, an embedder's memory that keeps fewer than its size needs
(`MemoryRequest::requiredTailBytes`). The check itself still traps. An i32 address is read as 32 bits whatever its slot holds, so the
one comparison of the i32 check is exact even for a slot left with high bits set
(`Memory.i32AddressIgnoresTheSlotsHighBits`). Task 19 checks in the optimized handlers' disassembly that the
select survived.

**The cache is invalidated by `Store::memoryGeneration()`.** Its contract, which P5's compiled code relies on
too: it starts at 0 and increases by one with every `Store::growMemory()` that adds at least one page, to any
memory of the store, whoever asked and whether or not the storage moved; refused grows, grows by zero, and
adding or dropping memories leave it unchanged. While it is unchanged, every memory's `bytes()` are the data
and size last read. The executor records the generation whenever it reads memory 0, and compares it after
each point where code it does not see may have grown a memory: after a host function returns, after a Wasm
callee returns to the same instance, and after `memory.grow` of any index (two imports may be one memory). It
reads memory 0 again only when the generation moved (`FastInterpreter::memoryRefreshes()` counts those,
`Memory.generationSkipsUnchangedRefresh`). A call into another instance, a tail call into one, and a return
from one read the frame's memory 0 afresh, whatever the generation: the registers held the other instance's
(`Memory.crossInstanceCallSwitchesMemory0`). `Memory.cacheRefreshedAfterEveryGrowPath` grows memory 0 in the
function, in a callee, in a host function and in a host function's re-entry, on the heap and on an embedder's
memory that moves on every grow.

## Tables, references and the bulk operations

**One body per bulk operation, shared with the reference.** `memory.fill`, `memory.copy`, `table.fill` and
`table.copy` run through `runtime::fillItems()` and `runtime::copyItems()`, and `memory.init` and `table.init`
through `runtime::copySegment()` (`runtime/Segment.hpp`), the same functions the reference interpreter runs. Each
checks its ranges before it writes anything, without overflowing (`runtime::fits()`, so a memory64 range whose end
`destination + count` passes 2^64 is out of bounds), so a trapping operation writes nothing
(`Bulk.boundsCheckedBeforeAnyWrite`). Then it runs in granules of 65'536 bytes or 4'096 references
(`runtime::GranuleItems`), polling at `BulkGranule` before each, through the executor's `PollGate` and its
countdown, never through the store directly. A copy visits its granules backwards whenever its destination is
above its source (`runtime::copyOrder()`), within one memory or table or across two, and copies within each
granule in the same direction, so an overlapping copy is a `memmove` and an interrupted one leaves the reference's
partial writes. `Bulk.granulesMatchReference` checks a 64 MiB `memory.fill` and a 64 MiB overlapping
`memory.copy` with the destination above: 1'024 granule polls on both engines, and interrupted at the first, the
513th and the last poll, the written prefix (the fill's first granules, the copy's last) against a model of its
own and against the reference byte for byte. `Tables.opsMatchReference` does the same for `table.init` and a
`table.copy` one up over three granules, and `Bulk.stepLimitStopsAtTheSameGranule` for a step budget spent
mid-fill on an unobserved store.

**Spectre and the bulk loops: the ranges are masked.** After the explicit checks, `fillItems()`, `copyItems()`
and `copySegment()` mask their offsets and counts with `runtime::rangeMask()`, once per operation, so that code
running ahead of a mispredicted check sees empty ranges at the start of the memory or table. Without it a guest
could train the check in bounds, then copy one byte from an out-of-bounds source: the speculative copy moves a
host byte into guest memory, store-to-load forwarding hands it to the guest's next load, and a load dependent on
that leaks it. The mask is the sign of the items left after the range, of those left after its first item, and
of the offset and the count themselves; sizes stay below 2^63, so for an offset and a count below 2^63 the
differences cannot wrap past the sign. Every bounds check derives its mask from it: `accessMask()` is the mask of
an access's bytes, `elementMask()` of one element (`Segment.rangeMaskAtTheEdges`, every pair of 8-bit offsets and
counts against a model, and the 64-bit edges). The bodies are the ones the reference interpreter and P5's compiled
stubs run too, so the mask protects every engine.

**Table indices are masked like addresses.** `table.get`, `table.set` and `call_indirect` read their element at
the index `elementChecked()` returns: the explicit `index >= size` check, then the index anded with
`elementMask()`, which is `runtime::rangeMask()` of a one-element range, the same select derived from the same
bound. An index of
2^63 or more, or any index into a table of no elements, masks to zero (`Tables.elementMaskAtTheEdges`, around
0, size − 1, size, 2^32 − 1, 2^63 and 2^64 − 1). What remains: a masked index reads the table's first element,
and for a table of no elements code ahead of the check may read one element at its base. Optimized for x86-64,
the mask is `sub`, `or`, `or` and `cmovns` before the element's address is formed, in the `table.get` and
`table.set` handlers and in `call_indirect`'s resolution.

**i32 operands of memories and tables are read as 32 bits.** An index, address, size, count or delta of a
memory or table of i32 addresses is read as the low 32 bits of its slot (`narrowToAddressType()`), whatever
the high bits hold, and `table.grow`'s and `table.size`'s results, like `memory.grow`'s, are written as 32
bits, `-1` included (`Tables.table32OperandsIgnoreTheSlotsHighBits`, `Bulk.memory32OperandsIgnoreTheSlotsHighBits`,
which run code written by hand with high bits planted in those slots).

**References keep their heap.** `ref.null` is never a constant: lowered code is shared by every instance in
every store, while a concrete heap names a `TypeId` of the running instance's store, so the operation names the
heap as the module does and maps it through the instance (`nullSlots()`), as a reference local's
initialization does (`Tables.referencesKeepTheirHeap`, with a duplicate type so that the `TypeId` is not the
type index). `ref.func` reads the instance's function address. `ref.is_null` and `ref.as_non_null` test the
reference's tag word. `data.drop` and `elem.drop` change the instance, which only the store hands out
mutable (`dropDataSegment()`, `dropElementSegment()`, out of line).

## Vector instructions, relaxed SIMD and wide arithmetic

**One kernel per instruction, resolved once.** A v128 takes two slots, lane 0's bits first. Every vector operator
of a kernel shape (`numerics::SimdShape`: unary, binary, ternary, test, shift) lowers to its shape's generic
operation, `simd.unary` to `simd.shift`, whose last word is the operator's kernel as `numerics::simd*Kernel()`
returns it: the look-up happens at lowering, and running the operation is a call through that word, of the very
function the reference interpreter calls (`Simd.lowersEachOpcodeToItsKernelOnce`). A listing names that word by
the first instruction whose kernel it is, never by its address. Kernel addresses are code words, so the fast
interpreter needs 64-bit pointers, as its tail-call handlers already do. `i32x4.add` and `f32x4.mul`
have dedicated operations that call the same kernel inline (`SimdInlineKernel`, checked equal to the table's,
`Simd.dedicatedRowsRunTheTablesKernels`). The lane operations take or give a scalar slot, which no kernel does, so
each has a dedicated operation over the lanes its row of `SimdLaneTable` names; `i8x16.shuffle` (the shared
`numerics::shuffle()`) and `v128.const` carry their 16 bytes as two immediates. Wide arithmetic
(`WideArithmeticTable`) writes its low and high halves to two consecutive slots, by the kernels of
`numerics/Integer.hpp`. The family is out of line in the switch dispatch and has its own tail-call handler file,
`TailCallSimd.cpp`.

**Deterministic in both modes.** The kernels are numerics', so NaN canonicalization and every lane-wise float
rule are the reference's by construction, and relaxed SIMD follows the Wasmtime-deterministic profile that
`numerics/SimdKernels.hpp` documents, in `Production` as in `Deterministic` (Decision C2): no result depends on
the host. `Simd.everySimdOpcodeAgreesWithReference` runs every SIMD row of the opcode table, relaxed SIMD's
included, and every vector access over 64 fixed-seed vectors (NaNs with payloads and either sign, infinities,
zeros, denormals, integer and saturation edges) with its operands forwarded and in their home slots, under every
dispatch and in both modes, and compares results bit for bit, traps, steps and memory with the reference's;
`Simd.relaxedUsesTheDeterministicProfile` pins the profile's own values (a fused, singly rounded madd, swizzle's
zero for every index of 16 or more, bit-wise laneselect, saturating trunc, the 16-bit wrap of the i7 dot product).
