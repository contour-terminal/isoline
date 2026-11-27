# The fast interpreter

The fast interpreter runs code lowered once, at load, from a validated module into operations over frame
slots. This document grows with it; its sections state the design decisions an embedder or a contributor
can rely on.

## The lowering arena and the compile budget

Lowering charges every byte it allocates to the module's compile budget, through a lowering arena
(`fastinterpreter::ILoweringArena`, `CountingArena`): a `std::pmr::memory_resource` that counts what it
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
