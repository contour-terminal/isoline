# Platform layer

`src/isoline/platform/` (`isoline::platform`) reaches every operating-system resource through an
interface, and says per (operating system, architecture) which implementation a build has: one row of
`PlatformTable` (`PlatformTable.hpp`), and one CMake-selected file per seam (`cmake/IsolinePlatform.cmake`).
This page records the decisions the rows carry that the code alone does not explain.

## JIT memory

Executable memory is never writable and executable at once for any thread. `platform::Protection` has no
writable-and-executable enumerator. The one mapping whose page tables are both is macOS arm64's `MAP_JIT`
region (`CodeMemoryKind::MapJitWithThreadToggle`), where `pthread_jit_write_protect_np` gives each thread
its own view: every thread starts with the region executable, and only a thread inside a `JitWriteScope`
sees it writable. A scope must end on the thread that opened it, and the process ends if it does not:
ended elsewhere, it could restore only that thread's view. Scopes cannot nest. Their constructor takes a
passkey that only the code memory's seal and the test helpers can make.

macOS x86-64 (`MapJitWithProtection`) maps `MAP_JIT` read-write and makes it read-execute with
`mprotect`. Every other platform never maps `MAP_JIT`.

## Guard-page memories

`runtime::ReservedMemoryAllocator` serves the linear memories of a guard-page engine through the injected
`IVirtualMemory`. Each memory reserves, inaccessible, the larger of the layout's reservation and the
request's maximum in whole pages, then the layout's guard, the sum rounded up to the allocation
granularity (64 KiB on Windows). It commits the pages its bytes span with `protect(ReadWrite)` and commits
more in place on every grow, so its base never moves and `guardBytes()`, the reservation past the committed
pages, is at least the layout's guard at every size up to the maximum. A maximum whose reservation is no
u64, a refused reservation and a refused commit are each `Refusal::HostMemory`, and a refused grow leaves the
memory as it was. Because `mprotect` may change part of a range before it fails, a refused commit is undone
whole with `protect(None)`, which decommits by the platform row's `DecommitKind` (madvise, a `MAP_FIXED`
remap, or `VirtualFree`): no page past the committed ones stays accessible. The process ends if that undo is
refused too. Memory64 and custom-page memories, which rely on no guard, are reserved by the same rule.
So an unbounded memory64 under a raised `StoreLimits::maxMemoryBytes` reserves that much in guard mode,
and is refused with `HostMemory` where the system cannot reserve it, though explicit mode would run it.

## Instruction-cache synchronization

`IInstructionCache` is the one seam through which code that was just written becomes executable by every
thread. `InstructionCacheTable` (`InstructionCache.hpp`) names each row's steps:

| Kind | Invalidation | Other threads | Calling thread |
|---|---|---|---|
| `Coherent` (x86-64) | none | none | none |
| `LinuxArm64` | `__builtin___clear_cache` | `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE)` | `isb` |
| `FreeBSDArm64` | `__builtin___clear_cache` | none (FreeBSD has no such command) | `isb` |
| `MacOSArm64` | `sys_icache_invalidate` | none | `isb` |
| `WindowsArm64` | `FlushInstructionCache` (documented to cover every processor) | none | `isb` |

Linux arm64 registers `SYNC_CORE` once per cache, when the cache is built, which is before any sandbox
lock-down. Every refusal is an error value carrying the system's code: a registration refused with
`ENOSYS`, `EINVAL` (no `SYNC_CORE` on this kernel) or `EPERM` (a sandbox), a synchronization refused, or an
invalidation refused (`FlushInstructionCache` can fail). None of them is ever taken for success.

**x86-64 does nothing (`Coherent`).** Instruction fetch on x86-64 is coherent with stores, so a write is
visible to the instruction fetch of every processor without a flush. The architecture asks the processor
that runs cross-modified code to execute a serializing instruction first. Isoline does not issue one: no
generated code is ever modified while it may run. Code is sealed by `mprotect`/`VirtualProtect` from
read-write to read-execute before any thread may run it, and the operating system's TLB shootdown for that
change interrupts every other processor running the process. Returning from that interrupt serializes. This
is the same reliance Wasmtime and V8 have on x86-64.

**Valgrind.** Valgrind 3.22.0 on arm64 (`ubuntu-24.04-arm`) passes `membarrier`'s `SYNC_CORE` commands
through to the kernel. `InstructionCache.sealedOnOneThreadRunsOnEight` passes under
`valgrind --tool=callgrind` (the `linux-arm64-clang` CI job), so a callgrind run can execute compiled code
on arm64.
