# Embedding Isoline

This is the contract between Isoline and the program that runs WebAssembly on it, the *embedder*:
what to construct in which order, which thread may touch what, what every limit is and where a running
guest can be stopped, and how each failure is reported. Every name below is spelled in `isoline::api`
(`#include <isoline/api/Store.hpp>` brings in all of them); the feature vocabulary (`FeatureSet`,
`Feature`, `Standard`, `UnmetPrerequisite`) is defined in `isoline::` (`isoline/Features.hpp`), shared
with the decoder and validator, and re-exported there. The names the API re-exports from the lower
layers (`api/Types.hpp`) are the public subset of those layers; everything else under `runtime/`,
`binary/` and `validate/` is internal.

Fallible calls return `std::expected`, and a handle of another store, or of a store that is gone, is
one of their error values wherever it is given. A violated precondition (a null implementation, a host
function type `Imports` cannot hold, a reference read from a handle whose store is gone) is a
programming error of the embedder and aborts. Four functions allocate while the
embedder sets up, and may throw `std::bad_alloc` (or `std::length_error` for a container asked past its
`max_size()`), leaving nothing half-built: the `Store` constructor, `makeHostFunction()` (which also
throws what moving the callable throws), and `Imports::define()` and `Imports::resolve()`, of which a
throwing `define()` leaves the definitions as they were. Every other function of the API is `noexcept`;
see *Out of memory* below for what it does when the host runs out of memory.

## Life cycle

```cpp
auto const engine = Engine { EngineConfig { .features = FeatureSet::wasm3() } };
auto const module = engine.load(bytes);                // std::expected<Module, LoadError>
auto allocator = HeapMemoryAllocator {};
auto store = Store { engine, allocator };              // the limits of engine.config().store
auto imports = Imports {};
auto const i32 = ValueType { .kind = ValueKind::I32 };
imports.define("env", "log", FuncType { .params = { i32 }, .results = {} }, makeHostFunction(log));
auto const instance = store.instantiate(*module, imports);  // std::expected<Instance, InstantiateError>
auto results = std::array<Value, 1> {};
auto const called = instance->call(*instance->function("run"), args, results);
```

1. **`Engine`** holds an `EngineConfig` and nothing else: the features modules are decoded and
   validated with, the execution engine and mode, the `LoadLimits`, and the `StoreLimits` every store
   made for it keeps to unless it overrides them (`StoreOptions`). The feature set must be closed over
   its prerequisites (checked); `FeatureSet::withPrerequisites()` closes one.
2. **`Module`** is what `Engine::load()` makes of a module's bytes: decoded, validated and described
   (`imports()`, `exports()`), never linked. A module loads whatever it imports. It carries the
   engine's `EngineIdentity` (engine, mode, features), and only a store of an engine of the same
   identity instantiates it (`EngineMismatch` otherwise).
3. **`Store`** is where instances live, with their memories, tables, globals, tags and host functions,
   under one `StoreLimits`: its engine's, or its own (`StoreOptions`, under *Limits*). Every limit caps
   the whole store, all its instances together.
4. **`Instance`** is a handle to one module instance of a store; `Function`, `Table`, `Memory`,
   `Global` and `Tag` (`api/Extern.hpp`) are handles to its exports, and `Extern` is any one of them, a
   `std::variant` with one alternative per `ExternKind`, in that order. All are cheap to copy; what they
   do once their store is gone is under *Lifetimes*.

`Imports` is a plain value holding definitions by module and field name: host functions
(`define(module, name, type, implementation)`), another instance's export (`define(module, name,
instance, exportName)`) and an entity of a store (`define(module, name, extern)`, for any `Extern`: an
exported one, or one the store made with `define()`, `defineMemory()`, `defineTable()` or
`defineGlobal()`). `Imports::resolve()` answers, without a store, whether each import of a module has a
definition of its name, kind and type.

`Store::instantiateByPosition(module, externs)` binds the imports by position instead, as `wasm.h`'s
`wasm_instance_new()` does: one `Extern` of the store per import, in import order. Names play no part,
so a module importing `("m", "x")` twice, as a function and as a global, links both, which one `Imports`
definition of `("m", "x")` cannot. The module's engine is checked first (`EngineMismatch`), then the count
(`ImportCountMismatch { expected, given }`), then each extern's store (`ForeignImport { index }`, its
position as index), and linking checks kinds and types as it does for named definitions. Both forms
share one linking path, so the limits, the nested-instantiation budget and the errors after the lookup
are the same.

## Threads

- `Engine` and `Module` are immutable: share them, load and instantiate on any number of threads at
  once.
- A `Store` and every handle of it (its `Instance`s, and every `Function`, `Memory`, `Global`, `Table`
  and `Tag`) are one unit, used by one thread at a time. The unit may move to another thread between
  calls. Destroying the store on one thread while another thread holds or uses one of its handles is a
  contract violation: a handle's alive-token check (see *Lifetimes*) catches a store destroyed before
  the use, on the unit's thread, and synchronizes nothing.
- The store's `Interrupter` (`Store::interrupter()`) is the one thing another thread may use while the
  store runs.
- A host function implementation defined in an `Imports` that several stores use is called on each of
  their threads at once, so it must be thread-safe.
- A thread calls `Engine::prepareThread()` before its first call into guest code and keeps the
  `ThreadPreparation` token while it runs any. Interpreters need nothing of a thread: their token holds
  nothing and the call cannot fail. Tokens nest: a thread may hold any number at once, of one engine
  and of several, destroyed in any order, and stays prepared while any of them lives; a compiling
  engine releases a resource only when the last token needing it goes. Destroy a token on the thread it
  prepared, after that thread's last call into guest code.

## Lifetimes

- A `Store` copies its engine's configuration: the `Engine` may be destroyed first.
- The `IMemoryAllocator` and the `IPollObserver` (`StoreOptions::observer`) given to a `Store` must
  outlive it.
- A `Module` keeps its validated module alive; its descriptions (`ImportDescription`,
  `ExportDescription`, `EngineIdentity`) point into it and are valid while any copy of the module lives.
  The `const&&` overloads are deleted, so a temporary module's descriptions do not compile.
- `Imports::define(module, name, instance, exportName)` copies the export's address, and
  `define(module, name, extern)` the extern's, each with the entity's type as the store holds it then:
  the store must be alive when the definition is made, and only that store links it (`ForeignImport`
  from any other). The definition keeps the store's identity (`StoreId`), never the store, so it may
  outlive the store safely.
- Handles name their store's entities for as long as the store lives, and are safe to keep after it is
  destroyed between calls (destroying it from inside one of its own host functions, while a call into it
  runs, is a contract violation nothing catches: the call still uses the store):
  each one holds the store's *alive-token*, a `std::weak_ptr` to a token the store owns and destroys
  first, and checks it before it reads the store. Nothing dereferences a store that is gone:
  - An `Instance` whose store is gone reports it: `call()` fails with a `CallError` whose `detail` is
    `StoreDeleted` (`CallFailure::StoreDeleted`), `memoryRange()`, `memorySize()` and `exportAt()` fail
    with `StoreDeleted`, and the by-name lookups (`function()`, `memory()`, `global()`, `table()`,
    `tag()`) find nothing: a deleted store names no export.
  - `Function::type()`, `Tag::type()` and every handle's `store()` return a reference into the store, so
    they check that it is alive and abort when it is not (a checked precondition violation, never
    undefined behaviour); `Imports::define()` of an instance or an extern reads the store and checks it
    the same way. `address()` and `isOf()` read nothing of the store and answer at any time.
  - A handle is never taken for one of a later store: every `StoreId` has a serial number unique within
    the process, which handles copy and `isOf()` compares. Every operation given a handle of another
    store, a deleted one included, refuses it with `ForeignHandle` and reads nothing: the `Store`
    accessors (`call()`, `globalValue()`, `tableGet()`, `memoryRange()`, `growMemory()`, …) and the
    `Instance` ones (`call()`, `memoryRange()`, `memorySize()`) alike; `instantiateByPosition()`
    refuses it as `ForeignImport`. The only checks that abort on a handle's store are those above: the
    accessors returning a reference into it (`Function::type()`, `Tag::type()`, `store()`), and
    `Imports::define()` of an instance or an extern, a builder the C API does not reach.
- A memory view (`Store::memoryRange()`, `Instance::memoryRange()`) is valid until the next call into the
  store or grow of the memory, either of which may move it.

## Memory provision

The store allocates every linear memory through the `IMemoryAllocator` it was given; the seam is how an
embedder places guest memory in storage of its own.

- `MemoryRequest` names the memory's type, its `initialBytes`, the `maximumBytes` it may grow to (its
  maximum, capped by its address type's reach and by `StoreLimits::maxMemoryBytes`, in whole pages) and
  the `requiredGuardBytes` the engine's code needs past the end (`Engine::requiredGuardBytes()`, 0 for
  the interpreters, which check every access). A memory whose `guardBytes()` is smaller is refused with
  `Refusal::Guard`; the guard must hold after every grow, as long as the memory lives.
- Growth is the embedder's decision. The store calls `ILinearMemory::grow()` only within the request's
  maximum and the store's budget, and charges the budget only when it succeeds. A memory that declines
  returns `Refusal::Embedder` (`HostMemory` when the host has none), and `memory.grow` returns -1. Any
  other refusal an allocator invents is reported as `Embedder`.
- `bytes()` may move on every grow; whoever holds a span re-fetches it after every grow and every host
  call. A view of size 0 is valid at any address, even past the end, and views nothing.
- `HeapMemoryAllocator`, the default, keeps each memory in a `std::vector` without a guard; a store
  that requires a guard refuses it.
- An allocator modelled on Contour's provision (the test double `ProvidedMemoryAllocator` is one) hands
  out exactly one memory, in a fixed buffer, and refuses every further request with `Embedder`. A store
  on such an allocator can hold one memory in all: a second instance with a memory, a module of several
  memories, or a component whose core instances each define one, fails to instantiate with
  `AllocationFailure { Memory, Embedder }`. Such an embedder gives each store its own allocator, or one
  that can provide several memories.

## Globals, tables, memories and exports

An instance's exports are reached by name, of one kind (`Instance::function()`, `memory()`, `global()`,
`table()`, `tag()`), or by index, of any kind: `Instance::exportAt(index)` returns the export at
`index` in the module's export order (`Module::exports()`) as an `Extern`, or `OutOfRange`.

The store reads and writes a global, table or memory of any of its instances, or one it defined itself,
without an instance:

| Call | Returns | Refusals |
|---|---|---|
| `globalValue(global)` | its value | `ForeignHandle` |
| `setGlobal(global, value)` | nothing | `ForeignHandle`, `Immutable` (a const global), `ValueMismatch` |
| `globalType(global)` | its `GlobalType` | `ForeignHandle` |
| `tableSize(table)` | its size in elements | `ForeignHandle` |
| `tableGet(table, index)` | the `Ref` at `index` | `ForeignHandle`, `OutOfBounds` |
| `tableSet(table, index, ref)` | nothing | `ForeignHandle`, `OutOfBounds`, `ValueMismatch` |
| `growTable(table, delta, init)` | the old size | `TableGrowthError`: `ForeignHandle`, `InitValueMismatch`, or a `Refusal` |
| `tableType(table)` | its `TableType`, its minimum the current size | `ForeignHandle` |
| `memorySize(memory)` | its `MemorySize`, in pages and bytes | `ForeignHandle` |
| `memoryType(memory)` | its `MemoryType`, its minimum the current size in pages | `ForeignHandle` |
| `memoryRange(memory, offset, length)` | a view of its bytes | `ForeignHandle`, `OutOfBounds` |
| `growMemory(memory, deltaPages)` | the old size in pages | `MemoryGrowthError`: `ForeignHandle`, or a `Refusal` |

A refused call changes nothing. A value is checked as a call's arguments are: of the global's type, or
of the table's element type, a reference of another heap or one naming what the store does not hold
included (`ValueMismatch`; `InitValueMismatch` for `growTable()`'s `init`). `growTable()` goes through
the guest's own path: it is charged against `maxTableElements` and refused as `table.grow` would be
(`TypeMaximum`, `Limit`, `HostMemory`). A refusal while a call into the store runs (a host function
growing a table) belongs to that call: it is recorded in the call's `refusedGrowth.table`, as a refused
`table.grow` of the guest's is, and a `CallError` the call ends with reports it. Types are in their
store's form, as `Function::type()` is: a concrete heap names a type of the store. An `exnref` that
`globalValue()` or `tableGet()` returns is escaped (see *Exceptions*).

`growMemory()` is `memory.grow`'s path, as `growTable()` is `table.grow`'s: charged against
`maxMemoryBytes`, refused alike (`TypeMaximum` past the memory's maximum or its address type's reach,
`Limit`, or the memory's own `Embedder` or `HostMemory`), and a refusal during a call is recorded in its
`refusedGrowth.memory`. Each growth error is a variant of what its call can produce, so a `Refusal`
alternative always means the guest's own refusal: `MemoryGrowthError` is `ForeignHandle` or a `Refusal`,
`TableGrowthError` adds `InitValueMismatch`. A view of a memory (`memoryRange()`, or
`Instance::memoryRange()`) is valid until the next call into the store or grow of that memory; a view
of length 0 is valid at any offset.

`Store::function(ref)` names the function a funcref refers to, one read from a table or a global,
returned by a call or passed to a host function, as a `Function` to call or to ask the type of, an
element-segment function no instance exports included. It refuses a null reference
(`FunctionRefError::Null`), a reference to a host object or an exception (`NotFunction`), and an
address at which the store holds no function (`UnknownAddress`). A reference names no store, only an
address, so one of another store at an address this store holds a function at names this store's
function: pass a reference of this store.

The store also defines entities without an instance, to bind to an import or share between instances:

| Call | Returns | Refusals (`DefineError`) |
|---|---|---|
| `define(type, function)` | a `Function` | `InvalidType`, `LimitExceeded` (`Types`), `HostMemoryExhausted` |
| `defineMemory(type)` | a `Memory` of `type.limits.min` zeroed pages | `InvalidType`, `LimitExceeded` (`Memories`, `MemoryBytes`), `AllocationFailure`, `HostMemoryExhausted` |
| `defineTable(type, init)` | a `Table` of `type.limits.min` elements, each `init` | `InvalidType` (`NonReferenceElement` too), `ValueMismatch`, `LimitExceeded` (`Tables`, `TableElements`), `AllocationFailure`, `HostMemoryExhausted` |
| `defineGlobal(type, value)` | a `Global` of `value` | `InvalidType`, `ValueMismatch`, `HostMemoryExhausted` |

A defined memory or table takes a module's own path: the same charges against the store's limits and
the same refusals, every size checked before a byte or element is allocated. A minimum above its
maximum, or past what its type reaches, is `AllocationFailure { …, TypeMaximum }`; a maximum
past that reach is clamped to it, and to the store's limit, not allocated. An initial value is checked
as `setGlobal()`'s and `tableSet()`'s are, before anything is charged. A global charges no limit, as a
module's does not: its globals are bounded only by the decoder's limits, so an embedder that defines
globals on a guest's behalf bounds them itself. Types are in store form. A type the host may not
define is refused, with nothing added, as `InvalidType`: `ConcreteTypeIndex` when it names a concrete
type index (only a module's own types may), `BottomHeap` when it names the bot heap (the validator's
own type, which no module may declare), `PageSize` for a memory whose page size custom-page-sizes
does not admit (1 byte and 64 KiB are the two it does). Each define adds its entity in one step that
leaves the store unchanged when the host runs out of memory, which is then `HostMemoryExhausted`; a
memory's bytes and a table's elements are allocated before that step, and their exhaustion is
`AllocationFailure { …, HostMemory }`.

## Calls and re-entrancy

`Store::call(function, args, results)` is the one checked entry into the guest, which
`Instance::call()` (once it found its store alive) and a host function's `HostContext::invoke()`
share. It needs no instance: it calls a host function `Store::define()` added as readily as an exported
one. Arguments are checked against the function's params, and the result
slots against its results, before any guest code runs (`ArgumentMismatch` otherwise). Calls are
re-entrant: a host function may call back into this or another instance of the store, within the call
that called it. A failure in a nested call stays nested unless the host function returns it, and an
instance stays callable after a trap.

Every call and every instantiation is an *invocation*, and so is every `HostContext::invoke()` within
one. `StoreLimits::maxNestedInvocations` bounds how many are in progress at once; one more traps with
`CallStackExhausted`. A component-model call that crosses from one core instance to another through a
host adapter takes one invocation per crossing (the adapter's `invoke()`), so a chain of N crossings
nests N + 1 invocations, within the same limit. The frames of every invocation in progress share one
stack budget (`maxStackValues`), one call depth (`maxCallDepth`: a Wasm frame a host function's
`invoke()` reaches is one deeper than the frame that called the host) and one step budget (`maxSteps`,
opened by the outermost).

**One invocation across calls.** A component call is several core calls: `realloc`, the callee and
`post-return`. `Store::openInvocation()` returns an `InvocationScope` (move-only) that holds one
invocation open across them until it is destroyed: every call made through the store with no other
call running shares its step budget and one `GrowthRecord` (read with `refusedGrowth()`, and reported
as each failed call's `refusedGrowth`), and is the one outermost invocation for `maxNestedInvocations`,
counted while it runs, the scope counting none of its own. Each hop a host function makes back into the
guest counts one more, as ever. A scope opened from a host function while a call runs holds a record of
its own for the calls the host function makes in it, while the budget stays the call's; the outer
call's refusals stay in the outer record. A scope opened while another is open at the same depth joins
it and changes nothing. An instantiation made in a scope shares its step budget, its start function
included, but keeps its refused grows to itself. Destroying a scope while a call made in it still runs
is a contract violation (checked); destroying it after its store is fine, and does nothing.

**Polling host work.** Host work done on a call's behalf (a component call's copies and transcodes)
polls with `Store::poll(BulkGranule { bytes })` at least every `BulkPollBytes` (65536) bytes, from a
host function or inside an `InvocationScope`: it notifies the observer at `PollSite::HostBulk`, checks
the interrupter, and charges `max(1, bytes / BulkPollBytes)` steps against the invocation's budget,
whole or not at all (`stepsCharged()`, the `host bulk` row below). It fails as a call would: with
`Trap { Interrupted }`, else `Trap { StepLimitExceeded }`.

Host functions are `IHostFunction`s, or `makeHostFunction()` of a `noexcept` callable. A host function
reads its arguments, writes its results, or ends the call abruptly:

- `Trap`: ends the call as a trap. `Trap { TrapCode::HostTrap, subject }` is reserved for the
  embedder (WASI's `proc_exit`, say, with the exit code as subject): every engine hands it back
  unchanged, subject included.
- `HostFailure { HostFailureKind::Embedder, code }`: the host function failed on its own account.
- `Thrown`: throws an exception of the store into its Wasm caller, whose handlers may catch it.

Results not of their types, a thrown exception the store never allocated, or a host write of the wrong
type is a `HostFailure` of `ResultMismatch` or `ArgumentMismatch`. `HostContext` reaches the store
read-only (`store()`); a host writes only through its checked members: `invoke()`, `addException()`,
`callerMemory()` (valid until the next grow or invoke) and `markEscaped()`.

**Out of memory.** Where the host runs out of memory before anything is added to the store
(`Engine::load()`, `Store::instantiate()` while it prepares, `Store::define()` while it reserves, and
`defineMemory()`, `defineTable()` and `defineGlobal()` wholly, as each adds its entity in one step that
leaves the store unchanged when it fails), the call is refused with `HostMemoryExhausted`. Once the store is being changed (an instantiation
committing, a host function being added, a call running) the calls are `noexcept`, and running out of
memory terminates the process: the store is not exception-safe midway through a change.

MSVC's debug STL (Debug builds with MSVC or clang-cl) allocates a container proxy inside container
constructors that are `noexcept`, so there even a refusal before the store changes is an allocation
failure in a `noexcept` frame: the process terminates instead of returning `HostMemoryExhausted`. Release
builds with MSVC, and every other standard library, refuse as described; the memory suite's
`[host-memory]` cases check it everywhere but on MSVC Debug.

## Errors

`Engine::load()` fails with a `LoadError`: `ModuleTooLarge` (before a byte is decoded),
`HostMemoryExhausted`, a `DecodeError` or a `ValidationError`. `isImplementationLimit()` tells a refusal
that raised `LoadLimits` would lift. A module that uses a proposal the engine does not enable is refused
with the error the spec suite expects of an engine without it (an unknown opcode, a malformed section
id, a second memory), and `requiredFeature()` names the proposal; the message appends it:
`illegal opcode 0c (simd not enabled)`. Enabling it, closed over its prerequisites, gets the module past
that point, where another proposal may be named.

A failed call is a `CallError`: its `detail` says how it ended, `failure()` reduces it to the
`CallFailure` an embedder branches on, and `refusedGrowth` holds this call's own first refused
`memory.grow` and `table.grow` (never a nested call's):

| `CallFailure` | `detail` |
|---|---|
| `Trap` | a `Trap` of any code but `Interrupted` |
| `Interrupted` | `Trap { TrapCode::Interrupted }`: the interrupter stopped the call, or had before it began |
| `HostFailure` | a `HostFailure` a host function the guest called returned |
| `UncaughtException` | an `UncaughtException`: the exception's tag and payload, copied out of the store |
| `ArgumentMismatch` | a `ValueMismatch`: the call was refused before it entered the guest |
| `ForeignHandle` | a `ForeignHandle`: the function is another store's, or a store's that is gone; nothing ran |
| `StoreDeleted` | a `StoreDeleted`: the instance's store is gone; nothing ran |

`Store::instantiate()` fails with an `InstantiateError`: `EngineMismatch`, `ImportCountMismatch
{ expected, given }` (`instantiateByPosition()` only), `ForeignImport { index }`,
`LinkError` (`UnknownImport` for an import with no definition, `IncompatibleImportType` for one of
another kind or type), `LimitExceeded { limit }` (the instance would exceed one of the store's limits,
checked before anything is allocated), `AllocationFailure { kind, reason }`, `HostMemoryExhausted`, or a
`CallError` when initializing it (a segment, the start function) failed as a call does; that error's
`refusedGrowth` is the start function's.

Contour's `IWasmBackend` contract maps onto these as follows:

| Contour | Isoline |
|---|---|
| `CallFailure::Trap` | `CallFailure::Trap`, or `UncaughtException` |
| `CallFailure::Interrupted` | `CallFailure::Interrupted` |
| `CallFailure::MemoryGrowthRefused` | `CallFailure::Trap` with `refusedGrowth.memory` `Limit` or `Embedder` (a hosted memory refuses as `Embedder`) |
| `CallFailure::HostFailure` | `CallFailure::HostFailure` |
| `OutOfMemory` at instantiation | `LimitExceeded { Limit::MemoryBytes }`, or `{ Limit::TableElements }` for a table |

### Failed checks

A precondition the embedder breaks (a handle used after its store is gone, an engine configured with a
feature set not closed over its prerequisites) is a programmer error, not an error value: the check
reports it and ends the process. Host misuse that C input can reach stays an error value. A check
reports through one of two channels:

- `Require()` reports through core-cpp's `core::setFailHandler()`, whose handler an embedder may
  replace, then aborts.
- Where the compiler has C++26 contracts (GCC 16.2 and later; `docs/language-features.md`), the checks
  of `noexcept` functions are contracts, and some declarations carry a `pre` or `post` of their own
  (`Engine`'s constructor, the handles' `type()` and store accessors). They report through the compiler's
  contract violation handler instead (GCC's default prints the condition to standard error), then
  terminate. An embedder's fail handler does not see them.

A program linking the C++ API (`isoline::core`) may install its own `handle_contract_violation`. Through the
C API's shared library it cannot: `libisoline_c.so` exports `wasm_*` and `isoline_*` alone (`isoline_c.map`,
`local: *`), so it binds its contract checks to the copy of GCC's default handler it links from
`libstdc++exp`, and a handler the program defines is never called there (measured on GCC 16.2.1: a check
failing in such a library prints GCC's report and terminates, the program's handler unseen). A static link of
the C API (`pkg-config --static isoline-c`) is the C++ API's case. A handler that throws is unsupported: every
contract Isoline compiles sits in a `noexcept` function, so the exception terminates the process where
the check failed, and never reaches the embedder. Isoline compiles its contracts under the `enforce`
semantic only, and refuses at configure time a build that would check them under another.

A package built with native contracts links `libstdc++exp`, so its consumer compiles with the GCC release
that built it (16.2 or later) over libstdc++: libstdc++exp's ABI is that release's alone, and a libc++ or
MSVC consumer cannot link it at all. `find_package(isoline)` refuses any other toolchain at configure
time, naming both compilers. Its headers' declarations carry those contracts, so every file that
includes one compiles as C++26, with contracts enabled (GCC 16.2's default there): `isoline::core` asks
a CMake consumer for it, and `isoline/Contracts.hpp` stops a file compiled without contracts with an
`#error`. A consumer that does not use CMake passes `-std=c++26` itself, and adds `-lstdc++exp` after the
library. A program cannot mix a native and a fallback package's objects: each mode's
headers reference a symbol only that mode's library defines, so the link fails.

A consumer may compile with GCC's `-fcontracts-client-check=pre` (or `=all`), which checks a declaration's
`pre` at the call site too. Measured on GCC 16.2.1 with a violation handler that throws, called from inside a
`try` block: the caller-side check runs in a `noexcept` clone of the callee (`<function> [clone
.contract_wrapper]`), so the exception terminates there, and the caller's `catch` never sees it. That holds
with the callee's own check switched off (`-fcontracts-definition-check=off`). A caller-side check under the
consumer's `observe` semantic reports and continues, and Isoline's callee-side check, under its pinned
`enforce`, then terminates. So the flag can add a report, never let a violated precondition through, and
Isoline does not refuse it. A native-lane canary pins this: `isoline_require_canary_client_check` is the
throwing-handler canary with its calls compiled with `-fcontracts-client-check=all`, and every `pre` and
`post` case must end the process from inside the canary's `try`, so a GCC whose caller-side check lets the
exception escape fails CI. No macro tells a translation unit compiled with it from one without (measured),
so `isoline/Contracts.hpp` cannot name it in its `#error`. A
package built on the fallback has none of these requirements (`docs/language-features.md`, "The consumer
side of a native package").

## Limits

A store keeps to the `StoreLimits` of its engine's configuration, or to those its `StoreOptions`
override them with, fixed at construction:

```cpp
auto limits = engine.config().store;                   // start from the engine's, change what differs
limits.maxMemoryBytes = uint64_t { 16 } << 20;
auto store = Store { engine, allocator, StoreOptions { .limits = limits, .observer = nullptr } };
```

`StoreOptions::limits` replaces the engine's limits whole; `std::nullopt` (the default) keeps them.
An override binds that store alone, and may raise a limit above the engine's as well as lower it.
Limits are no part of `EngineIdentity`, so every store of an engine, whatever its overrides,
instantiates the engine's modules.

No limit is checked against the host's capacity yet, and the constructor refuses none. A limit set past
what the host can hold turns the guest's trap at the limit into the process's termination
(`std::bad_alloc` midway through a call, or a stack overflow). Above all, `maxNestedInvocations` bounds
recursion on the calling thread's stack in every engine, the interpreters included: each
Wasm → host → Wasm nesting takes host stack, so its value must fit that thread's stack. A fallible
factory (`Store::create`) is to check it for every engine, against the store's effective limits rather
than only its engine's, and to report a refusal as a value; so must any engine code that relies on a
limit (a guard region sized for `maxMemoryBytes`).

Each count or size caps the whole store; each refusal names its `Limit`. `Store::used(limit)` reads
what the store holds of one: the counts and sizes of its entities; the frames' stack values and the
invocations in progress (0 between calls); and for `Steps`, the steps the latest outermost call or
instantiation took, the invocations nested in it included, kept after it ended. `CallDepth` reads 0, as
the executor counts it while it runs. A limit the guest reaches at run time traps (`CallStackExhausted`,
`ExceptionLimitExceeded`, `StepLimitExceeded`) or makes a `grow` return -1 (`Refusal::Limit`); one an
instantiation would exceed refuses it with `LimitExceeded`. The defaults:

<!-- limits:begin -->
- `maxCallDepth` (call depth): 10000 frames
- `maxStackValues` (stack values): 4194304 values
- `maxNestedInvocations` (nested invocations): 100 invocations
- `maxMemoryBytes` (memory bytes): 4294967296 bytes
- `maxTableElements` (table elements): 10000000 elements
- `maxMemories` (memories): 10000 memories
- `maxTables` (tables): 100000 tables
- `maxInstances` (instances): 10000 module instances
- `maxExceptions` (exceptions): 65536 exceptions
- `maxExceptionValues` (exception values): 1048576 values
- `maxTypes` (types): 1000000 type units
- `maxTags` (tags): 1000000 tags
- `maxSteps` (steps): unlimited
<!-- limits:end -->

The block is generated from `runtime::LimitTable` and `StoreLimits {}`; `PublishedLimits.docsMatchTables`
fails, printing the block as it should read, when the two part. A frame costs its static size against
`maxStackValues` (params, locals, its most operands, its deepest label nesting, and 1; a host frame its
params, results and 1), so the call that exhausts the stack is the same in every engine. `maxSteps`
counts the steps each poll site charges (below); every outermost call and every outermost
instantiation outside any `InvocationScope` starts a fresh budget, while one made inside a scope shares
the scope's, its start function included, and an instantiation a host function makes shares the budget
of the call that reached it. The default is no limit.

`LoadLimits` bound loading: `maxModuleBytes` (64 MiB), and the decode limits of
`binary::DecodeLimits` (function body size, locals, operand height and the rest), so that what loading
holds is linear in the module's size.

Validating a body takes time linear in its size times the widest signature it names: an instruction that
takes or gives a whole signature's operands (a call, a block's params, a branch) costs time in its arity,
which `maxParams` and `maxResults` bound. Branches check their label's operands in place, never popping
and pushing them. `br_if`, `br_on_null` and `br_on_non_null` leave the label's types in the operands'
slots, as the spec's pop and push would, and the validator remembers one thing: which label type list the
top was last checked against, at which height. A branch through that same type list at that height costs
O(1) until an operand beneath the top is popped; any other type list is checked in full, and becomes the
one remembered. So the cost is one check per change of label type list: `br_if 0` and `br_if 1`
alternating over two equally wide labels check each time. A `br_table` checks all its labels against one
run of operands, and checks each block its labels name at most once, however the labels alternate; a
label whose block carries the same type list (the same block type) as the label checked before it is
skipped as well.

The refusal order, which a fuzzer comparing engines must expect: `br` and `br_if` look their label up
first, then (`br_if`) pop the condition, then check the label's operands; `br_on_non_null` looks its
label up, requires the label's last type to be a reference, pops the reference, then checks the rest;
`br_on_null` peeks the reference first (only a top that is no reference fails there), then looks its
label up, then pops. `br_table` runs in Isoline's own order: the default label's lookup, the condition,
then each label's lookup, arity and types in turn, and the default label's types last. That differs from
the reference interpreter's, which checks the default label's types first and pops the condition last:
`block (result i32) i64.const 0 i32.const 0 br_table 5 0` is `unknown label` in Isoline and `type
mismatch` there. Every way the operands themselves fail is one `type mismatch`, so which of them fails
first is not observable.

A component is decoded under `component::ComponentLimits`: `maxComponentBytes` (256 MiB), `maxNesting`
(100 scopes, the outermost component counted), `maxDefinitions` (1000000 per component), `maxNameBytes`
(100000) and `maxDecodedItems` (1000000), which counts every definition, nested component and element
of a definition's vectors of the component and all components nested in it, each before it is read.
The decoded pool then holds at most about `maxDecodedItems` × the largest item's size × 2 (vector
growth), plus its copy of the input and the names copied out of it, each at most `maxComponentBytes`.
The largest item is a nested component, its definition and its pool entry: 112 bytes with libstdc++ on
x86-64, so the defaults bound the pool at about 1000000 × 112 × 2 = 224 MB beyond 2 × 256 MiB. The
sizes differ with libc++ and the MSVC STL. Component and instance types are scopes too: `maxNesting` counts them together
with the components around them, the outermost component included, where wasm-tools caps the nesting
of component types (100) apart from that of components, so a type nested 100 deep inside the outermost
component is refused here and accepted there.

Loading a component (`component::api::Component::load`) takes time linear in its bytes, bounded by
the same caps: one decode pass, one validation pass, each charged as it goes (the type caps,
`maxSubtypingSteps`, `maxCoreMatchingSteps`), one SHA-256 pass over the bytes, then each embedded core
module validated decode-only and, only once the whole component validated, loaded once through the
`ICoreModuleLoader` (each within the engine's `LoadLimits::maxModuleBytes`), then one plan step per
definition. Nothing in it is interruptible but the loader, which may refuse a module and so end the
load; an embedder that loads untrusted components lowers `maxComponentBytes` to bound the time, as
Contour's extension host (P9) does for extensions. The loaded modules must be the engine's: a module
of another engine identity, or a validator whose core features are not the engine's, is refused at
load (`CoreModuleEngineMismatch`, `CoreFeaturesMismatch`), so a module cache keys by the engine
identity as well as by the component hash and ordinal.

A component store's handles are capped by `maxHandles` (1000000), where the component model's
`definitions.py` caps only each table's indices (2^28 − 1) and has no store-wide cap at all. The cap
counts handle *slots*, every table's together, plus the handles lent to calls in progress: a table
keeps a slot it grew by once the handle in it is gone (`definitions.py`'s array never shrinks, and its
free list hands the slot out again), and gives it back only when the store goes. So the store's handle
memory is bounded however handles move, at a price the reference does not have: moving handles from
one instance's table to another's can trap `Handles` at a full budget although fewer handles are live,
and a call passing a `list<borrow<r>>` of N handles to another instance costs about 3N: the caller's N
handles, N lends while the call runs, and N slots in the callee's table, which the callee keeps.

A component store (`component::runtime::ComponentStore`) holds every core instance of every component
instance in one core `Store`, of its engine's limits, allocator and interrupter. Instantiating runs the
component's plans on an explicit stack, so nesting takes no host stack, and counts each component
instance (`maxComponentInstances`), core instance (`maxCoreInstances`) and plan step, a frame's slots
included (`maxInstantiationSteps`), before what it counts is created. All core instantiations of one
`instantiate` share one invocation, so one step budget, start functions included. A refusal before the
first step (a store already instantiating or calling, code of another engine, imports that do not fit,
no instance left) changes nothing; **any failure after it locks the store down**, as does any failure of
a component call: a trap, an interrupt, the step limit, an uncaught core exception, a host function's
failure, and a nested trap even when a host function swallowed it. A locked-down store runs no guest code
again (`CannotEnter`); core instances alone keep the core contract, staying callable after a trap. The
embedder drops the store and instantiates afresh. One component call runs in one invocation scope, so the
argument `realloc`s, the callee and `post-return` share one step budget; synchronous re-entrance, a host
function calling back into the store, is allowed, and each such hop is one more nested invocation of
the store's core store. Arguments a call lifts are destroyed once lowered into the callee, so a chain of
hops holds one copy of them live at a time. The host running out of memory during a call (`canonLift`,
`callComponentFunction`, a lowered function) terminates the process, as it does in a core call;
`instantiate` reports it as `HostMemoryExhausted`.

**Stack per hop.** Each hop takes host stack on the calling thread, more than a core-only nesting does,
so the component store's core store nests at most the lower of `ComponentLimits::maxNestedCalls` (80)
and the engine's `maxNestedInvocations` (100): the component limit only ever tightens the engine's, and an
embedder that chose a lower one for a small stack keeps it. One more traps `CallStackExhausted` and locks
the store down. The thread that calls into a component store needs

    stack >= hops × bytes per hop + reserve

with `hops` the store's nesting limit (the lower of the two), the bytes per hop of the build below, and
a reserve for what the embedder's own frames and the outermost call take (the base column, plus the
embedder's frames).
Measured with `isoline_component_test "Calls.stackPerHop"` (a lowered import of a lifted export
calling itself, between depth 10 and 40, on the Reference engine):

| Build                        | Bytes per hop | Base   | 80 hops, 2× margin |
|------------------------------|---------------|--------|--------------------|
| clang, release               | 4 640         | 5 200  | 742 KB             |
| MSVC (cl.exe), release       | 5 440         | 8 000  | 870 KB             |
| GCC, debug                   | 8 192         | 10 368 | 1.31 MB            |
| clang, debug + ASan + UBSan  | 11 520        | 13 056 | 1.84 MB            |
| MSVC (cl.exe), debug         | 15 584        | 19 152 | 2.49 MB            |

So 80 hops fit the 1 MiB a Windows thread reserves by default with a 2× margin in release builds, and
fit an 8 MiB Linux or macOS main thread in every build; a debug build on Windows, or an embedder that
raises `maxNestedCalls`, runs component calls on a thread sized by the formula.

**The embedding API.** `component::api` wraps the runtime for an embedder (D7). A `Linker` holds what
components are instantiated with, each under a *path* of extern names joined by `#`: `ns:p/i@0.1.2#f` is
the function `f` of the interface instance `ns:p/i@0.1.2`, and its leading segments make the linker's
instances. It defines host functions (`defineFunction`, a `FunctionTypeSpec` of its own arena's types),
host resource types (`defineResource`), an instance of a store (`defineInstance`), an instance of no
definitions yet (`defineEmptyInstance(path)`, as Wasmtime's `Linker::instance`), a core module (`defineModule`)
and a component (`defineComponent`). Every misuse is a named `LinkerError` and leaves
the linker as it was: a path that is not extern names (`InvalidPath`), one defined already
(`Redefinition`: nothing is replaced), one inside or around another definition (`PathConflict`), a null
function, a type not of the linker's arena, a parameter label that is not kebab-case or repeats, a
resource name not in UTF-8. Once built, a linker is read by any number of stores and threads.

An import matches the definition of its exact name; else, for an interface name, the highest
semver-compatible version (from 1.0.0 the major, `0.M.x` the major and minor, `0.0.N` only itself); else,
for an instance import that `implements` an interface, that interface. An import Wasmtime would check
against no definition needs none: a type that is no resource, a resource `eq` to one an earlier import
bound, or an instance of only such exports and of instances of them, however deep (WIT's `use
types.{…}`). A defined core module satisfies an import of a module type only as a subtype of it, checked
when the import links, as Wasmtime checks one: its type is read from the module's descriptions and its
type section (`api::Module::types()`). Types match by subtyping as
wasm-tools decides it, and a mismatch names the import and says why in wasm-tools' words
(`ImportTypeMismatch`); `Linker::resolve` reports every import's outcome without a store. A function's
parameter names are part of its type, as the Explainer compares them, so a host function is matched under
its labels (`ParameterNames::Named`, the default) and two guests under theirs. A host function defined with
`FunctionTypeSpec::names = ParameterNames::Unnamed` names no parameter, as a Wasmtime `func_wrap` names
none: it satisfies an import whatever the import calls its parameters, their count, their types (equal, as
value types are) and the result still compared; a mismatch is reported under the import's parameter name.
The wast runner's spectest defines its host functions so. Host resource
types match by identity: copies of one `HostResourceType` are one resource type, so two imports defined
with copies pass handles between them. `ComponentStore::instantiate` refuses a component of another
engine, an unsatisfied or mistyped import, and an instance of another store before anything runs.

`ComponentInstance::module(path)` and `ComponentInstance::resource(path)` give a core module and a
resource type an instance exports, as Wasmtime's script runner defines a named component's in its linker:
the resource type as a `HostResourceType` another store's linker defines with `defineResource`, a host's
type re-exported being that type, a guest's one identity per store and runtime type, without a host
destructor.

`ComponentInstance::function(path)` gives a function the instance lifts, and as well a host function it
re-exports from its imports (Wasmtime's `restrictions.wast` refuses such a component; the component model
does not). The host calls the latter as a guest would: its arguments are checked against, and its results
built in, the types the instance exports it with, its handles' resources bound as the instance binds them;
the owns among the arguments pass from the host to the host function, which holds them as its arguments,
and the owns it returns pass back, so `hostHolds` counts each as one call through a guest does.

`ComponentInstance::call` lends its arguments and checks them first: a function of another store
(`ForeignFunction`), a locked-down store (`LockedDown`), and arguments not of the function's arena, count
or types, or a handle of another resource type than its parameter's (`ArgumentMismatch`) are refused
without entering a guest or locking the store down. Every failure is a `ComponentCallError` whose
failure is a row of `ComponentCallFailureTable`, which names its detail and what it does to the store:

<!-- component-call-failures:begin -->
- `Trap` (`canonical::Trap`): locked down; component call trapped.
- `Interrupted` (`canonical::Trap`): locked down; component call interrupted.
- `HostFailure` (`isoline::api::HostFailure`): locked down; host function failed.
- `LockedDown` (`std::monostate`): unchanged; component store is locked down.
- `ArgumentMismatch` (`canonical::ValueError`): unchanged; arguments do not match the function.
- `ForeignFunction` (`std::monostate`): unchanged; function or resource of another component store.
- `StoreDeleted` (`std::monostate`): unchanged; component store is gone.
- `Instantiating` (`std::monostate`): unchanged; component store is instantiating.
- `ResourceExhausted` (`canonical::Trap`): locked down; component store resources exhausted.
<!-- component-call-failures:end -->

The block is generated from `api::ComponentCallFailureTable`; `CallError.docsMatchTable` fails, printing
the block as it should read, when the two part. An instance or function whose store is gone refuses with
`StoreDeleted` rather than reach it.

A host function (`IHostComponentFunction`) is `noexcept`: it fails by returning a `HostFailure`, which
ends the call and locks the store down; results not of its type are `HostFailure{ResultMismatch}`. An
own handle among its arguments is the host's (lifting removed it from the guest's table). The store keeps
a table of the owns the host holds, as Wasmtime does: lifting an own to the host, as an argument or a
call's result, and `hostResource(Own)` add one; `drop`, passing it to a call and returning it from a host
function take one, and an own the host does not hold is refused (`ArgumentMismatch`, or the host
function's `HostFailure`), so a resource is destroyed once. The table is capped by `maxHostOwns`
(1000000), and each of its entries is charged `HostOwnEntryBytes` (64) to `maxLiveValueBytes`, so no
guest grows it without bound: an own past either cap fails the call that would hand it to the host
(`ResourceExhausted`, which locks the store down; the owns it could not hold are destroyed at the store's
end), and `hostResource` refuses it (`HostOwns` or `LiveValueBytes`). The store's end destroys every own
the host still holds once, by its host type's destructor (no guest destructor runs, as for the owns guest
tables hold), so an own the host forgot no longer outlives the store. A host destructor that calls back
into the store during that teardown (a parent's destructor dropping a child's own) is refused with
`StoreDeleted`: the store names no state once its teardown starts, and its instances' tokens have expired. The host hands an own back or drops it with
`ComponentStore::drop`, which runs a host type's destructor, or a guest
type's through the canonical ABI (refused in a locked-down store), and refuses a borrow. A host type's
destructor also runs once for each own a guest table still holds when the store is destroyed, never at
lockdown. The store is move-only; its instances and functions stay valid across a move, and moving it
while a call or an instantiation of it runs is a precondition violation (`isBusy()` tells). A drop while
the store instantiates, from a host function a start function calls, is refused (`Instantiating`).

**Typed bindings.** `component::api::typed` (`Typed.hpp`) puts C++ types over the same calls, one
`TypeOf<T>` row per type: `bool`, the integers, `float`, `double`, `char32_t`, `std::string`,
`std::vector<T>` (`list`), `std::optional<T>`, `std::tuple<T…>`, `std::expected<T, E>` (`result`, with
`Unit` for a case without payload), `std::map<K, V>` (a duplicated key read from a guest keeps its last
value), records and enums and flags the embedder describes by specializing `Record`, `Enum` and `Flags`,
`std::variant` of `Case<"label", T>` alternatives, and `Own<R>` and `Borrow<R>` for the host resource type
a `ResourceTypes<R…>` binds to `R`. `functionType<Signature>(linker)` gives `defineFunction` the type of
`Result(Params…)`, its parameters labelled `p0`, `p1`, … unless named; `makeTypedHostFunction<Signature>`
takes a `noexcept` callable (with or without the `HostComponentContext&` first, returning the result or
`std::expected<Result, HostFailure>`) and refuses arguments not of their types with
`HostFailure{ArgumentMismatch}`; a callable that may throw does not compile. `TypedFunction<Signature>::bind`
finds an export of that type (labels of parameters aside) or none. Every value crosses as `Values`, one
copy. An `Own` is move-only and dropped explicitly by design, as Wasmtime's `Resource<T>` is: destroying one
runs nothing, and the store keeps the own until it is dropped or the store ends. `typed::drop` and a
call (which takes owns as non-const rvalues only) consume an own exactly when the store no longer holds
it afterwards: a drop or call that ran, the owns a failed call's lowering took, never a refusal. An
empty own is refused before the store sees it (`ArgumentMismatch`; a host body returning one fails
`HostFailure{ResultMismatch}`), so no own is dropped twice. `TypedFunction::bind(instance, path,
resources)` checks that an `Own<R>` or `Borrow<R>` of an `R` bound in `resources` names that host
resource type; a resource the guest defines, which no `HostResourceType` stands for, matches by kind. `HostResourceTable<T>` keeps the host's objects behind their reps: a rep carries its slot
and the slot's generation, so an erased object's rep finds nothing however the slot is reused, and a slot
whose generations run out is retired; a full table is a `Handles` trap.

## Components

A component is hosted in five steps: describe its world's types in C++, define the host's side in a
`Linker`, load the component once, instantiate it in a `ComponentStore` of its own, and call its exports.
`src/isoline/component/api/Extension_test.cpp` does all five for a world shaped like Contour's
extensions (`tests/component/extension/extension.wit`: Contour's status-line world, a host-owned
`surface` resource and a control-shaped interface), against two guests of it, one written by hand in the
text format and one built by the C toolchain (`wit-bindgen c` and wasi-sdk). It is written only against
`component::api` and its typed bindings, and is the template for an embedder; the excerpts below are
from it and from the status-line world's types it shares (`src/isoline/component/api/testing/StatuslineWorld.hpp`).

`tests/component/contour-clock` hosts one of Contour's own extensions the same way: its Rust clock, built for
`wasm32-wasip1` as Contour builds it and componentized with the preview-1 reactor adapter. Such a component imports a
narrow part of WASI 0.2 besides its world, which the embedder binds in its own `Linker` (Isoline ships no WASI 0.2);
its README lists the interfaces and what the adapter imports of each.

### Hosting an extension

**The world's types.** Each WIT type is a C++ type with a `TypeOf` row: records, enums and flags by
specializing `Record`, `Enum` and `Flags`, variants as `std::variant`s of `Case`s, `result` as
`std::expected`, `option` as `std::optional`, and a host resource as a tag type that `Own` and `Borrow`
name:

```cpp
struct Size { uint32_t cols = 0; uint32_t rows = 0; };
template <> struct Record<Size> {
    static constexpr auto Fields = std::tuple { field("cols", &Size::cols), field("rows", &Size::rows) };
};
using Event = std::variant<Case<"title", std::string>, /* … */ Case<"timer", Unit>, Case<"reply", std::vector<uint8_t>>>;
struct Surface {};                                     // host.surface
```

**The host's side.** A `Linker` holds every definition, each under the path of its interface's name and
version: `isoline-test:extension/host@0.1.0#log`. A host resource type is a `HostResourceType`, its
destructor erasing the host's object; the objects themselves sit in a `HostResourceTable`, whose reps the
handles carry. Each function is `functionType<Signature>()` and `makeTypedHostFunction<Signature>()` over a
`noexcept` body:

```cpp
auto surface = HostResourceType { "surface", std::make_shared<SurfaceDestructor>(surfaces, destroyed) };
auto resources = ResourceTypes<Surface> { surface };
if (auto defined = linker.defineResource("isoline-test:extension/host@0.1.0#surface", surface); !defined)
    return std::unexpected { defined.error() };                     // a LinkerError: the linker is unchanged
// A host body fails with a HostFailure whose code says which failure it was.
auto const failure = [](HostError error) {
    return HostFailure { .kind = HostFailureKind::Embedder, .code = static_cast<uint32_t>(error), .mismatch = {} };
};
auto spec = functionType<Own<Surface>()>(linker, resources);
spec->maxCallBytes = 64 * 1024;                                     // Contour's maxHostCallBytes
linker.defineFunction("isoline-test:extension/host@0.1.0#open-surface", *spec,
    makeTypedHostFunction<Own<Surface>()>(
        [&](HostComponentContext& context) noexcept -> std::expected<Own<Surface>, HostFailure> {
            auto const rep = surfaces.insert(SurfaceObject {});
            if (!rep)
                return std::unexpected { failure(HostError::SurfaceTableFull) };
            auto own = hostOwn<Surface>(context.store(), resources, *rep);   // the guest's own, once returned
            if (!own)
            {
                surfaces.erase(*rep);                                // the store holds no own of it
                return std::unexpected { failure(HostError::OwnRefused) };   // maxHostOwns or maxLiveValueBytes
            }
            return std::move(*own);
        }));
```

The functions of `host` are labelled as the WIT names their parameters (`functionType`'s third argument),
which an import must match (`ParameterNames::Named`); those of `control`, a generated catalog, are defined
`ParameterNames::Unnamed`, matching whatever the import calls them. `FunctionTypeSpec::maxCallBytes` caps
the host bytes the arguments of one call take once lifted from the guest, within the store's
`maxLiveValueBytes`: one byte more traps `HostCallBytes` before the body runs, and locks the store down. It
is no part of the type, so an import matches the function whatever it is. The `types` interface and the world's
`use` of its types are imports of types only and need no definition. A body that is given an `Own` drops
it (`typed::drop(context.store(), own)`) unless it hands it back: an own it forgets stays held, against
`maxHostOwns`, until the store ends.

**Loading.** `Component::load` decodes and validates the component, then loads each embedded core module
through the `ICoreModuleLoader` it is given, keyed by the component's SHA-256 and the module's ordinal: the
hand-written guest has two core modules, the C toolchain's three (its code, and wit-component's shim and
fixup). A `Component` is immutable and shared, so it is loaded once and instantiated any number of times.

**Instantiating.** Each instance of an extension gets a store of its own, with an allocator of its own:

```cpp
auto store = ComponentStore { engine, allocator, ComponentConfig { .limits = limits } };
auto instance = store.instantiate(component, linker);
```

`Linker::resolve(component)` reports how each import resolves without a store. A guest built against
`@0.1.0` links to a host that defines `@0.1.2` (0.M.x matches on the major and minor); one defining
`@0.2.0` leaves the import unsatisfied, refused before anything runs.

**Calling.** `TypedFunction<Signature>::bind(instance, "render")` finds an export of that type, or none.
Each call converts its arguments, lends them, and converts the results back:

```cpp
auto render = TypedFunction<std::vector<uint8_t>(uint32_t, uint32_t)>::bind(instance, "render");
auto const bytes = (*render)(80, 1);   // std::expected<std::vector<uint8_t>, ComponentCallError>
```

A watchdog on another thread stops a call that runs past its deadline (`render`, or `load` itself) with
`store.interrupter()->interrupt()`: the call fails `Interrupted` and the store is locked down, so every
later call fails `LockedDown`. The embedder then drops the store and instantiates the component afresh in
a new one, of the same caps and with a fresh memory provision (the old store is destroyed before its
allocator). The guests' `mode` configuration makes them misbehave, each contained:

| The guest | Fails |
|---|---|
| returns a list its memory does not hold (a `list<u8>` of 2^28 − 1 bytes from one page) | `Trap` `OutOfBounds`, before the host allocates a byte of it, whatever the store's `maxLiveValueBytes` |
| returns 4 MiB its memory holds, against a `maxLiveValueBytes` of 1 MiB | `Trap` `LiveValueBytes`, charged before it is allocated |
| passes 80 KiB to a host function of a 64 KiB `maxCallBytes` | `Trap` `HostCallBytes`, before the host function runs |
| executes `unreachable` | `Trap` `Core` (`unreachable`) |
| keeps every surface it opens, until the host's 64-slot table is full | the host body's `HostFailure`, its code `SurfaceTableFull`; the store's end destroys the 64 it kept |
| hands out memory it does not have from `cabi_realloc` while `host.state` is lowered | `Trap` `OutOfBounds`, before a byte is written |

Each locks the store down.

**Embedder memory.** Contour hands each extension one memory of 32 MiB followed by a 32 MiB guard, refusing
any further request (`ProvidedMemoryAllocator` models it). Both guests allocate exactly one memory, so
each runs there; a second instance in the same store is refused (`AllocationFailure { Memory, Embedder }`),
and so is a provision smaller than the guest's initial memory. The Reference engine needs no guard
(`Engine::requiredGuardBytes()` is 0), so no guard is too small for it; an engine that compiles will need
one, and refuse a memory with less (`Refusal::Guard`).

### Component limits

Every cap a component, its store and its calls are held to, with its default:

<!-- component-limits:begin -->
- `maxComponentBytes` (268435456): bytes of one encoded component, refused before the decoder copies it.
- `maxNesting` (100): scopes open at once, the outermost component counted: nested components, component and instance types, nested scopes, in the binary and the text format.
- `maxTypeDepth` (100): nesting depth of one value, function, component or instance type, refused where it is defined.
- `maxTypeSize` (1000000): effective type size of one type, and the sum of the effective sizes of the types aliased into one component from outside it, as wasm-tools measures them; refused when reached.
- `maxTypes` (1000000): entries of every index space of sort type, per component.
- `maxDefinitions` (1000000): definitions of every section, custom sections included, per component.
- `maxDecodedItems` (1000000): items of one decoded component and every component nested in it: definitions, nested components, and the elements of the vectors inside definitions (arguments, inline exports, attributes), charged before each is read.
- `maxNameBytes` (100000): bytes of one name, attribute or custom section name, refused before it is copied.
- `maxCoreInstances` (10000): core instances of one ComponentStore.
- `maxComponentInstances` (10000): component instances of one ComponentStore.
- `maxInstantiationSteps` (1000000): plan steps one instantiate() executes.
- `maxResourceTypes` (100000): runtime resource types of one ComponentStore, one per resource definition executed; and the resource types one validation mints, one per resource definition and `(sub resource)` import or export.
- `maxHandles` (1000000): handle slots of every handle table of one ComponentStore together, each kept by its table for reuse once freed, plus the handles lent to calls in progress; one table hands out indices up to 2^28 - 1.
- `maxLiveValueBytes` (268435456): host bytes of every live Values of one ComponentStore.
- `maxSubtypingSteps` (1000000): steps of one type relation: a whole validation's subtype checks together (instantiations and ascribed exports share one relation), or one linker resolve; one per pair of types compared and per name looked up or compared, plus one per 64 bytes of the names compared; it bounds the relation's memo at as many entries.
- `maxSubstitutedTypes` (1000000): units the resource substitutions of one type arena charge, one per type created plus one per type it names: each instantiation, and each import or export of an instance type that defines resources, rewrites the types naming them, sharing their names.
- `maxModules` (1000): entries of one component's core module index space: embedded, imported, aliased and exported modules.
- `maxComponents` (1000): entries of one component's component index space: nested, imported, aliased and exported components.
- `maxInstances` (4096): entries of one component's core and component instance index spaces together.
- `maxFunctions` (1000000): entries of one component's core and component function index spaces together.
- `maxExports` (1000000): exports of one component.
- `maxCoreTables` (1000000): entries of one component's core table index space, each an alias of a core instance's export.
- `maxCoreMemories` (1000000): entries of one component's core memory index space, each an alias of a core instance's export.
- `maxCoreGlobals` (1000000): entries of one component's core global index space, each an alias of a core instance's export.
- `maxCoreTags` (1000000): entries of one component's core tag index space, each an alias of a core instance's export.
- `maxCoreMatchingSteps` (10000000): steps of all core instantiations of one validation: one per import looked up and matched, and per export or inline export measured, plus one per 64 bytes of the names its lookups compare.
- `maxNestedCalls` (80): invocations in progress at once in one ComponentStore's core store: the outermost component call and each hop back into a guest; one more traps CallStackExhausted.
- `maxHostOwns` (1000000): owns one ComponentStore's host holds at once, each lifted to a host function or a call's caller or made by hostResource(); one more fails the call ResourceExhausted.
<!-- component-limits:end -->

The block is generated from `component::LimitTable` and `ComponentLimits {}`; `Limits.docsMatchTable`
fails, printing the block as it should read, when the two part. The decode caps are fixed when a
component is loaded (`ComponentConfig::limits` given to `Component::load`), the store's when it is
constructed, and the linker's (`maxSubtypingSteps` for one resolve) when it is.

### Mapping failures

An embedder reports a failed extension in its own terms. Contour's extension host has four
(`ExtensionError`: `Trapped`, `DeadlineExceeded`, `OutputTooLarge`, `WorldMismatch`), and maps Isoline's
failures onto them by a table, the first matching row winning, as `Extension_test.cpp` does:

| `ComponentCallFailure` | Its trap | `ExtensionError` |
|---|---|---|
| `Trap` | `LiveValueBytes` (Contour's `maxRenderBytes`, the store's `maxLiveValueBytes`) | `OutputTooLarge` |
| `Trap` | `HostCallBytes` (Contour's `maxHostCallBytes`, a host function's `maxCallBytes`) | `OutputTooLarge` |
| `Trap` | any other | `Trapped` |
| `Interrupted` | | `DeadlineExceeded` |
| `ArgumentMismatch`, `ForeignFunction` | | `WorldMismatch` |
| `HostFailure`, `LockedDown`, `StoreDeleted`, `Instantiating`, `ResourceExhausted` | | `Trapped` |

A failed instantiation (`InstantiateComponentError`) is a `WorldMismatch` when the component is of another
engine or an import is unsatisfied, mistyped or foreign (`ImportMismatch` among the runtime's failures), and
`Trapped` otherwise; an export `TypedFunction::bind` does not find at its type is a `WorldMismatch` too. A host
body's own failures are told apart by their `HostFailure::code`: the extension test's host reports a refused
own (a `HostOwns` cap breach inside a body) as `OwnRefused`, and a full table as `SurfaceTableFull`.

### Lockdown and core modules

A component store locks down whole on the first failure whose row says so (D6): a trap, an interrupt, the
step limit, a host function's failure, a nested trap even when a host swallowed it, and any failure of an
instantiation after its first step. No guest code of that store runs again. That is the component model's
rule, and it is stricter than the core's: a core `Store`'s instances stay callable after a trap, and a
store built on the core API alone keeps that contract. What lockdown leaves untouched is the `Component`:
its core modules were loaded once, are shared and immutable, and serve every new store, so re-instantiating
after a lockdown costs an instantiation, not a load (nor, with a cache behind the loader, a compile).

### Checklist for an embedder hosting extensions

What Contour's extension host (P9) does, each item shown by `Extension_test.cpp`:

- Load each extension's component once, through an `ICoreModuleLoader` backed by the module cache, keyed by
  the engine's identity, the component's hash and the module's ordinal.
- Give every instance a `ComponentStore` of its own, with an allocator of its own: Contour's provision holds
  one memory, and each guest's instantiation allocates one.
- Set the caps: a lower `maxComponentBytes` for untrusted components, `maxLiveValueBytes` (Contour's
  `maxRenderBytes`), each host function's `maxCallBytes` (Contour's `maxHostCallBytes`), `maxHostOwns`, and
  the engine's `maxSteps` as a backstop for the interrupt.
- Run component calls on a thread whose stack fits the store's nesting limit (the formula above).
- Define each interface at the host's own version; older guests link by semver. Interfaces of types only
  need no definition.
- Label host functions as the WIT does, or define a generated catalog's `ParameterNames::Unnamed`.
- Write every host body `noexcept`, failing with a `HostFailure`, never by throwing.
- Drop every `Own` a body is given and does not hand back.
- Keep the host's resource objects (the `HostResourceTable`, and what its destructors reach) alive until
  the store is gone: the store's end destroys every own the host still holds, once.
- Interrupt a call past its deadline from a watchdog thread; after any failure that locks the store down,
  drop the store and instantiate afresh.
- Refuse an extension whose exports `TypedFunction::bind` does not find at their types.
- Map every failure onto the embedder's own errors by a table (Mapping failures, above), and tell a host
  body's failures apart by their codes.

## Interrupts and poll sites

`Store::interrupter()` returns the store's `Interrupter`, shared. `interrupt()` is one release store to
a lock-free atomic, callable from any thread, any number of times; it is final, never cleared. It exists
before any instantiation, so instantiation and start functions are interruptible too, and a watchdog
may keep it past the store. Raising it ends the running call at its next poll with
`Trap { Interrupted }`, and every later call and instantiation fails at its first poll, before any guest
instruction runs.

`interrupt()` is async-signal-safe, but copying the `std::shared_ptr` is not: an embedder that
interrupts from a signal handler keeps a raw `Interrupter*` (`store.interrupter().get()`) where the
handler finds it, and keeps the `shared_ptr` alive for as long as the handler may run.

Every engine polls at the same Wasm-level places, so a limit cuts off at the same point in each.
At one poll the interrupt is checked before the step charge, so `Interrupted` beats `StepLimitExceeded`:

<!-- poll-sites:begin -->
- **function entry** (one step): every callee entry, before its first instruction: call, call_indirect, call_ref, the tail calls return_call*, host callees, and the embedder's entry; before the call depth and the frame's stack charge are checked, so a call refused for its stack has cost its step, and a spent step budget traps before an exhausted stack.
- **loop back-edge** (one step): every transfer of control to a loop label, whatever the instruction: br, br_if, br_table, br_on_null, br_on_non_null, and a try_table catch clause.
- **host return** (one step): every return from a host function, whatever it returned; an interrupt, and then a spent step budget, replaces its outcome.
- **bulk granule** (one step): before every 65536 bytes of memory.fill, memory.copy, memory.init and instantiation's data copies, and every 4096 elements of table.fill, table.copy, table.init and instantiation's element copies and element item evaluation; copies visit their granules backwards when the destination is above the source.
- **instantiation** (one step): on entry to instantiation, and before the start function when the module has one.
- **unwind** (no step): every 4096 labels, catch clauses and frames an exception unwinds past.
- **exception sweep** (one step): before every 4096 items an exception sweep visits, the first included, when an exception is allocated on a full arena and no host function is running; the items, in order: each value of the payload being allocated, each global, each slot of each table of exn type, each item of each element segment not dropped, each operand and each local of every active frame (every one, none pruned as dead), each slot of the exception arena (which grows to the high-water mark of slots not Free, reusing the lowest free address first), then each payload value of each exception found reachable, once per exception; a sweep a poll ends reclaims nothing, and the allocation ends with that poll's trap.
- **host bulk** (a step per 65536 bytes, at least one): where the embedder polls its own bulk work (Store::poll), at least every 65536 bytes of a component call's copies and transcodes; it charges the step budget of the invocation in progress, which an invocation scope (Store::openInvocation) shares between the calls made directly in it, and a charge the budget cannot take whole is refused, charging nothing.
<!-- poll-sites:end -->

The block is generated from `runtime::PollSiteTable`, as the limits are. An `IPollObserver` given to the
store (`StoreOptions::observer`) sees every poll, on the executing thread, before the interrupt is
checked; poll-counting tests and profilers use it.

**Unpolled host operations.** A few operations are the host's work, bounded by a limit or by the
module's size but not interruptible midway:

- a `HeapMemoryAllocator` grow, resizing and zero-filling its vector (bounded by `maxMemoryBytes`); an
  embedder's own allocator decides how long its grow takes;
- allocating a table, and filling a `table.grow`'s new elements (bounded by `maxTableElements`);
- at instantiation, by the module's size and the decode limits: interning its types (`maxTypes`),
  resolving and checking its imports, allocating its functions and its tags (`maxTags`), evaluating
  its globals' initializers and allocating the globals (up to `DecodeLimits::maxGlobals`, a million),
  re-filling each table that has an initializer with its value, and building the instance's exports.

**Latency.** The specification bounds the 99th percentile of interrupt-to-return at 10 ms on the
reference machine, a fifth of the 50 ms Contour gives a guest before it kills the helper. Release builds
measure it: `ctest --preset clang-release-perf` (the other test presets leave the `perf` label out, as it
asserts wall-clock time) runs `Latency.interruptToReturnP99`, 200 trials of a spinning call that a
watchdog thread interrupts, and prints the p50, p99 and maximum.

## Bulk operations

`memory.fill`, `memory.copy`, `memory.init`, `table.fill`, `table.copy`, `table.init` and
instantiation's segment copies work in granules (65536 bytes, 4096 elements) and poll before each. An
`Interrupted` or `StepLimitExceeded` midway leaves the granules already written in place: a bulk
operation is not atomic, the guest sees partial writes after the trap, and so does the embedder. The
granule order is part of the cross-engine contract, and depends on the addresses alone: a copy whose
destination address is above its source address goes backwards, whether or not the two ranges overlap
and when they lie in two different memories or tables; every other copy, and every fill and init, goes
forwards. Backwards is what lets an overlapping copy read every byte before it overwrites it; applying
it by address alone keeps the visible partial writes after an interrupt the same in every engine.

## Exceptions

An exception the guest throws and nothing catches ends the call as `UncaughtException`, a copy of its
tag and payload. `UncaughtException::tag` compares with the `address()` of the `Tag` an instance
exports (`Instance::tag(name)`), whose `type()` lists the payload's types.

A store holds its exceptions in an arena capped by `maxExceptions` and `maxExceptionValues`; when it is
full, an allocation sweeps it and reclaims what no root reaches. An `exnref` handed to the embedder (in
a call's results or an uncaught exception's payload) or to a host function (as an argument) is
*escaped*: it stays valid, and its slot taken, for the store's life. An arena full of escaped
exceptions therefore traps every further throw with `ExceptionLimitExceeded` until the store is
dropped. A host function that keeps an `exnref` it only borrowed (one read through
`HostContext::store()`, or returned by `addException()` or `invoke()`) past its frame calls
`HostContext::markEscaped()`, or a later sweep may reclaim it.

## Floating point

Wasm's float operations assume the IEEE 754 defaults, which a host may have changed: a rounding mode set
with `std::fesetround`, or subnormals flushed to zero. Every invocation therefore pins the calling
thread's environment to the defaults for its duration and gives the host its own back at the end; nested
invocations do the same, and the defaults are re-established whenever a host function returns. On x86-64
and on arm64 (GCC and Clang) the defaults are complete: round-to-nearest-even, no flush-to-zero or
denormals-are-zero, no float traps, so a host function that changes the environment cannot change the
guest's results. On the portable row (below) only the rounding mode is pinned: a host that flushes
subnormals there changes subnormal results.

- What is restored is the host's *control* state, its rounding mode and the bits of its control register
  the target's row governs. The sticky exception flags are status, not control: the guest's operations
  may raise them, as the host's own would, and they stay raised after the call. Where switching the
  rounding mode back (at the call's end, or when a host function returns) goes through a C library whose
  `fesetround()` rewrites the whole register, flags included (wine's ucrtbase clears them), only the
  x86-64 row carries them across: its flags are in MXCSR, which is read before the switch and written
  after it. On arm64 the flags are in FPSR, which the scopes do not touch, and the portable row has no
  register: there the flags stay raised only because the C library leaves them alone, as glibc's and
  Microsoft's do. A host that reads them clears them before the call.
- x86-64 pins MXCSR (rounding, FTZ, DAZ, exception masks), and arm64 FPCR (rounding, FZ, FZ16, default
  NaN, trap enables, and FEAT_AFP's FIZ, AH and NEP). Every other target, and a build for several architectures at once, takes the portable row,
  which pins only the rounding mode, through `<cfenv>`: flushing to zero cannot be controlled there.
- MSVC on arm64 takes the portable row too, as MSVC has no inline assembly for FPCR, so FZ, AH, FIZ and
  NEP are not pinned there yet, and nothing carries the FPSR flags across a flag-clearing C library. That
  is a known follow-up, which must land before Windows arm64 runs compiled code.
- The environment belongs to a thread. An executor that resumes guest code on another thread (none does
  today) must establish the defaults there too.

## Deterministic mode

`ExecutionMode::Deterministic` makes every engine compute the same results on every host: canonical NaNs
where the specification allows any NaN, the deterministic profile of relaxed SIMD, and the step limit
counted alike everywhere. `maxSteps` and the static stack cost apply in both modes, by every engine.
