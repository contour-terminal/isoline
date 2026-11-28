# The component model

Isoline runs WebAssembly components synchronously: it decodes, validates, loads, instantiates and calls
them, lifts and lowers values through the canonical ABI, and keeps handle tables and resource types as
the component model's `definitions.py` does. This note records how the layer is built and why: the
decisions made, how a component becomes plans, and the seams the asynchronous half (P12) builds on. How
an embedder uses it is in [`docs/embedding.md`](../embedding.md); what the conformance suites run is in
[`docs/conformance.md`](../conformance.md).

## Layers

The component layer is the library `isoline_component`, which links `isoline_core` and nothing else;
`isoline_core` links no component code. Each directory is a namespace, lowest first:

| Directory | Namespace | Holds |
|---|---|---|
| `component/` | `isoline::component` | the feature table (`Features.hpp`) and the limit table (`Limits.hpp`) |
| `binary/` | `component::binary` | the decoded AST, the decoder, the encoder and canonical form, and a test builder |
| `types/` | `component::types` | the type arena: interned value, function, component and instance types |
| `validate/` | `component::validate` | names and semver, subtyping, core type matching, the validator |
| `canonical/` | `component::canonical` | layouts, values, traps, guest memory, lift and lower, strings |
| `runtime/` | `component::runtime` | handle tables, resources, plans, calls, built-ins, the runtime store |
| `api/` | `component::api` | `Component`, `Linker`, `ComponentStore`, call errors, the typed bindings |
| `text/` | `component::text` | the component text format: reader, writer and its forms (`isoline_component_text`) |

`isoline_component_text` links `isoline_text` and `isoline_component`; `isoline_text` never links a
component (D13). The `isoline` tool links all of them.

Three rules hold across every layer. **Nothing recurses on guest or user input**: nesting, outer aliases,
subtyping, lift, lower, value destruction, instantiation, and the text reader and writer each loop over an
explicit stack. **Memory is linear in the input, and every guest-growable resource is capped**: each cap is
a row of `component::LimitTable`, listed in the embedding guide, and passing one is a refusal at load or a
trap at run time, never the host running out of memory. **Spec messages win**: a refusal reads as
wasm-tools words it, and a trap as the component model's suite does.

## Decisions

Each was made before the code was written, with the alternatives weighed in the plan; the rulings taken
while building it are folded in.

- **D1, a decoded AST, then one plan per component definition.** `binary::Component` mirrors the binary
  grammar; nested components are indices into one flat pool, never recursive members. `runtime::buildPlans`
  makes one `ComponentPlan` per pool entry: linear steps with every alias resolved to a slot of that
  definition's index spaces, and a nested instantiation as one step. Instantiation runs the plans on an
  explicit frame stack, counting component instances, core instances and steps before what each counts is
  created. Plans are immutable and shared by every store and thread; interpreting the AST anew per
  instantiation, or inlining the whole tree at load, were the alternatives (the second makes load memory
  follow the step cap instead of the input).
- **D2, one core store per component store.** Every core instance of every component instance lives in one
  `isoline::api::Store`, wired through `api::Imports`, so engine choice, embedder memory, caps and the
  interrupt carry over unchanged. The layer uses only the core's public API.
- **D3, an interpretive canonical ABI.** Each interned value type has a `Layout` (size, alignment, flat
  types up to one past the flat-parameter limit, discriminant width, offsets), computed bottom-up. Lift and
  lower are one work-stack loop switching on the value kind, driven by the destination's types. Fused
  adapters are faster but a second form of every rule: that is the proposed P5c, with this interpreter as
  its fuzzing oracle.
- **D4, strings keep their source encoding.** A string value is `definitions.py`'s (text, encoding, code
  units); lowering follows its ten-case `store_string_into_range`, so component-to-component calls make the
  reference's `realloc` calls. Host strings are UTF-8, and a host string is validated while it is copied.
- **D5, handle tables as `definitions.py`'s, resource types generative.** One table per component instance
  (slot 0 reserved, last-freed first, at most 2^28 − 1 indices), a store-wide handle budget counting slots
  and lends, and chunked storage, so no entry moves. Every misuse is checked before anything changes, with
  the reference's trap codes. Resource identity is per instantiation: each executed resource definition
  creates a runtime type, and an import binds its supplier's.
- **D6, lockdown.** Any trap of a component call or instantiation (an interrupt, the step limit, an
  uncaught core exception, a host failure, a nested trap a host swallowed, any failure after an
  instantiation's first step) locks the whole component store: no guest code of it runs again. Core
  instances keep the core contract, callable after a trap. An embedder drops the store and instantiates
  afresh.
- **D7, a dynamic API, typed on top.** `component::api` works on `canonical::Values`; `api::typed` maps C++
  types to component types by one `TypeOf<T>` row each. A WIT generator is P14's.
- **D8, linking by name and semver.** An import matches a definition of its exact name; else the highest
  semver-compatible version (the major from 1.0.0, the major and minor for 0.M.x, `0.0.N` only itself,
  pre-releases exactly, build metadata ignored); else an interface its instance import `implements`. An
  import of types only needs no definition. Types match by subtyping, host resources by identity. A host
  function's parameter labels are part of its type unless it is defined `ParameterNames::Unnamed`.
- **D9, values as flat arenas.** A `Values` is a pre-order run of nodes and one byte pool, tagged with its
  type arena, charged to the store's live-bytes budget before it grows and released when destroyed, so
  destroying one never recurses.
- **D10, core modules validated decode-only, loaded after, keyed by hash and ordinal.** Validation never
  compiles; only a component that validated reaches `ICoreModuleLoader::load(CoreModuleKey, bytes)`, so a
  cache (P8) serves compiled modules by the component's SHA-256 and the module's ordinal, and by the
  engine's identity, which load checks.
- **D11, async decoded, refused, its seams reserved** (below).
- **D12, conformance.** The component model's, wasm-tools' and a curated list of Wasmtime's suites run
  natively; every file runs, and every skip is per command, for a named reason.
- **D13, the text format.** `text::compileText` takes the table of top-level forms, so the core text
  library stays free of components.
- **D14, 32-bit addresses only.** 64-bit memories for components are off.

Rulings made while building, each binding for what follows:

- **Stage fidelity.** Every refusal happens at the stage wasm-tools reports it (decode or validation),
  because the suites assert the stage.
- **No memo keyed by attacker-chosen pairs.** Type interning is an ordered structural comparison, core type
  matching a joint canonicalisation, never a hash or pair memo that a crafted input makes quadratic.
- **Charge before allocate.** Every host allocation a guest can cause is charged to its cap first; a
  refusal leaves no partial value.
- **The host's owns are tracked.** The store keeps a table of the owns the host holds, as Wasmtime does:
  each own is dropped or passed on once, the table is capped by `maxHostOwns` and charged to the live-bytes
  budget, and the store's end destroys whatever the host forgot.
- **A host function may cap its own arguments.** `FunctionTypeSpec::maxCallBytes` lifts one call's arguments
  into a budget of that cap within the store's (every charge taken from both, whole or not at all), so an
  embedder bounds what an extension passes to one host call (Contour's `maxHostCallBytes`) apart from what it
  holds in all (`maxLiveValueBytes`, Contour's `maxRenderBytes`); a breach traps `HostCallBytes`.

## From bytes to a call

1. `Component::load` decodes the bytes under the decode caps, validates the result (each embedded core
   module decode-only, under the engine's features), hashes the bytes, loads each core module through the
   injected loader, and plans every definition.
2. `Linker::resolve` matches each root import against the linker's definitions, without a store.
3. `ComponentStore::instantiate` refuses a mismatch before anything runs, then runs the root plan: each step
   creates one core instance, lowered function, resource type or nested component instance, in order.
4. `ComponentInstance::call` checks the arguments, then lowers them into the callee through its options,
   runs it, lifts its results and runs its `post-return`, all in one invocation scope, so one step budget.

## Seams for P12

The asynchronous half of the component model is decoded and refused at validation until P12 turns on
`ComponentFeature::Async`. What it builds on:

- The handle entry is a `std::variant` of a free slot and a resource handle; P12 appends its kinds
  (streams, futures, subtasks, waitables). `resourceHandleAt` takes the resource alternative today, and must
  trap `WrongHandleType` once there are more.
- Tasks and subtasks have `State` enums, and `canon_lift` takes its task, start and resolve callbacks.
  `TaskStack::resolve` refuses a second resolve, and only the innermost task may end, as checks that hold
  for synchronous calls only: `task.return`, `task.cancel` and async tasks make both guest-reachable, so
  P12 turns them into traps (`definitions.py`'s `trap_if(state == RESOLVED)`).
- The canonical built-ins table has `Unimplemented` rows for every async built-in.
- `flattenFunction` must key its synchronous or asynchronous branch on the canonical option, not on the
  function type.
- Lift and lower keep about 10 KiB of host stack scratch at peak: a fiber with a small stack budgets for it,
  or the buffers move to the heap, charged.
- There is room for an `IAsyncHostComponentFunction` beside the synchronous host function. No fibers and
  no `core::async` enter the synchronous layer.
