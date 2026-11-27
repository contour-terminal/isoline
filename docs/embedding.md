# Embedding Isoline

This is the contract between Isoline and the program that runs WebAssembly on it, the *embedder*:
what to construct in which order, which thread may touch what, what every limit is and where a running
guest can be stopped, and how each failure is reported. Every name below is spelled in `isoline::api`
(`#include <isoline/api/Store.hpp>` brings in all of them); the feature vocabulary (`FeatureSet`,
`Feature`, `Standard`, `UnmetPrerequisite`) is defined in `isoline::` (`isoline/Features.hpp`), shared
with the decoder and validator, and re-exported there. The names the API re-exports from the lower
layers (`api/Types.hpp`) are the public subset of those layers; everything else under `runtime/`,
`binary/` and `validate/` is internal.

Fallible calls return `std::expected`; a violated precondition (a handle of another store, a null
implementation) is a programming error of the embedder and aborts. Four functions allocate while the
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
   validated with, the execution engine and mode, the `LoadLimits` and the `StoreLimits` of every store
   made for it. The feature set must be closed over its prerequisites (checked);
   `FeatureSet::withPrerequisites()` closes one.
2. **`Module`** is what `Engine::load()` makes of a module's bytes: decoded, validated and described
   (`imports()`, `exports()`), never linked. A module loads whatever it imports. It carries the
   engine's `EngineIdentity` (engine, mode, features), and only a store of an engine of the same
   identity instantiates it (`EngineMismatch` otherwise).
3. **`Store`** is where instances live, with their memories, tables, globals, tags and host functions,
   under one `StoreLimits`. Every limit caps the whole store, all its instances together.
4. **`Instance`** is a handle to one module instance of a store; `Function`, `Table`, `Memory`,
   `Global` and `Tag` (`api/Extern.hpp`) are handles to its exports, and `Extern` is any one of them, a
   `std::variant` with one alternative per `ExternKind`, in that order. All are cheap to copy; what they
   do once their store is gone is under *Lifetimes*.

`Imports` is a plain value holding definitions by module and field name: host functions
(`define(module, name, type, implementation)`), another instance's export (`define(module, name,
instance, exportName)`) and a function of a store (`define(module, name, function)`, for a host function
`Store::define()` added or an exported function). `Imports::resolve()` answers, without a store, whether
each import of a module has a definition of its name, kind and type.

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
- The `IMemoryAllocator` and the `IPollObserver` given to a `Store` must outlive it.
- A `Module` keeps its validated module alive; its descriptions (`ImportDescription`,
  `ExportDescription`, `EngineIdentity`) point into it and are valid while any copy of the module lives.
  The `const&&` overloads are deleted, so a temporary module's descriptions do not compile.
- `Imports::define(module, name, instance, exportName)` copies the export's address, and
  `define(module, name, function)` the function's: the instance's store must be alive when the
  definition is made, and only that store links it (`ForeignImport` from any other). The definition
  keeps the store's identity (`StoreId`), never the store, so it may outlive the store safely.
- Handles name their store's entities for as long as the store lives, and are safe to keep after it:
  each one holds the store's *alive-token*, a `std::weak_ptr` to a token the store owns and destroys
  first, and checks it before it reads the store. Nothing dereferences a store that is gone:
  - An `Instance` whose store is gone reports it: `call()` fails with a `CallError` whose `detail` is
    `StoreDeleted` (`CallFailure::StoreDeleted`), `memoryRange()`, `memorySize()` and `exportAt()` fail
    with `StoreDeleted`, and the by-name lookups (`function()`, `memory()`, `global()`, `table()`,
    `tag()`) find nothing: a deleted store names no export.
  - `Function::type()`, `Tag::type()` and every handle's `store()` return a reference into the store, so
    they check that it is alive and abort when it is not (a checked precondition violation, never
    undefined behaviour); `Imports::define()` of an instance or a function reads the store and checks it
    the same way. `address()` and `isOf()` read nothing of the store and answer at any time.
  - A handle is never taken for one of a later store: every `StoreId` has a serial number unique within
    the process, which handles copy and `isOf()` compares. The `Store` accessors (`globalValue()`,
    `tableGet()`, `memoryRange()`, `growMemory()`, …) refuse a handle of another store, a deleted one
    included, with `ForeignHandle`;
    passing another store's `Function` or `Memory` to an `Instance` is a checked precondition violation.
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

The store also defines entities without an instance, to bind to an import or share between instances:

| Call | Returns | Refusals (`DefineError`) |
|---|---|---|
| `define(type, function)` | a `Function` | `InvalidType`, `LimitExceeded` (`Types`), `HostMemoryExhausted` |
| `defineMemory(type)` | a `Memory` of `type.limits.min` zeroed pages | `InvalidType`, `LimitExceeded` (`Memories`, `MemoryBytes`), `AllocationFailure`, `HostMemoryExhausted` |
| `defineTable(type, init)` | a `Table` of `type.limits.min` elements, each `init` | `InvalidType`, `ValueMismatch`, `LimitExceeded` (`Tables`, `TableElements`), `AllocationFailure`, `HostMemoryExhausted` |
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

`Instance::call()` is the one checked entry into the guest, which a host function's
`HostContext::invoke()` shares. Arguments are checked against the function's params, and the result
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
| `ForeignHandle` | a `ForeignHandle`: the function is another store's (reserved: `Instance::call()` still checks that as a precondition) |
| `StoreDeleted` | a `StoreDeleted`: the instance's store is gone; nothing ran |

`Store::instantiate()` fails with an `InstantiateError`: `EngineMismatch`, `ForeignImport { index }`,
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

## Limits

A store keeps to the `StoreLimits` of its engine's configuration, fixed at construction. Each count or
size caps the whole store; each refusal names its `Limit`. `Store::used(limit)` reads what the store
holds of one: the counts and sizes of its entities; the frames' stack values and the invocations in
progress (0 between calls); and for `Steps`, the steps the latest outermost call or instantiation took,
the invocations nested in it included, kept after it ended. `CallDepth` reads 0, as the executor counts
it while it runs. A limit the guest reaches at run time traps (`CallStackExhausted`,
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
counts one step per poll of a site that charges one (below); every outermost call and every outermost
instantiation starts a fresh budget, while an instantiation a host function makes shares the budget of
the call that reached it. The default is no limit.

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
<!-- poll-sites:end -->

The block is generated from `runtime::PollSiteTable`, as the limits are. An `IPollObserver` given to the
store sees every poll, on the executing thread, before the interrupt is checked; poll-counting tests
and profilers use it.

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
