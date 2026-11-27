# The C API

Isoline's C API is the standard WebAssembly C API, `wasm.h` at wasm-c-api revision `9d6b937` (the
revision Wasmtime v48 LTS through v51 vendor), and `isoline.h`, the `isoline_` functions for what
`wasm.h` cannot say. A C program includes `<isoline/c/isoline.h>`, which includes `wasm.h` after giving
its functions the library's export annotation, and links `isoline_c` (the shared library) or
`isoline_c_static`.

This page grows with the C API; it describes what is implemented so far.

## What this release provides

The C API arrives in two steps. This release is its foundation; the next adds engines, stores, modules,
instances and calls.

- **The library.** `isoline_c` (shared) and `isoline_c_static`, installed by the install component
  `isoline-c`: the headers beneath `include/isoline/`, the static library and the archives it links beneath
  `<libdir>/isoline/`, a pkg-config file (`isoline-c`), and the component `c` of the CMake package
  (`find_package(isoline COMPONENTS c)`, target `isoline::c`). The shared library's file name carries the
  ABI version.
- **Vectors.** Every `wasm_<name>_vec_*` function of `wasm.h`'s vector families: `new_empty`,
  `new_uninitialized`, `new`, `copy` and `delete`.
- **Types.** Every `wasm.h` type function: value, function, global, table, memory, tag, extern, import and
  export types, with v128, exnref and 64-bit limits through `val.h`.
- **Errors.** `isoline_error_t`, its codes and its stable detail numbers (below).
- **Twenty `isoline_` functions:** `isoline_abi_version`, `isoline_abi_compatible`; `isoline_error_new`,
  `_delete`, `_code`, `_code_name`, `_detail`, `_message`, `_message_length`, `_cause`;
  `isoline_valtype_nullability`, `_heap`, `_concrete_index`; `isoline_memorytype_new`, `_limits64`,
  `_page_size_log2`, `_address_bits`; `isoline_tabletype_new`, `_limits64`, `_address_bits`.

The rest of `wasm.h` (configs, engines, stores, modules, instances, functions, globals, tables, memories,
values, references, traps and frames) is declared by the header and not yet defined by this release: a
program that calls one of those functions does not link against it.

## Where Isoline departs from wasm.h

- **A wasm_ function takes its `own` arguments, also when it refuses.** `wasm.h` marks an argument the
  callee takes with `own`, and Isoline's `wasm_` functions take it whatever they return: a constructor that
  returns NULL deletes what it was given, so `wasm.h`'s inline helpers, which pass temporaries
  (`wasm_functype_new_1_1(wasm_valtype_new(WASM_I32), wasm_valtype_new(77))`), leak nothing. This holds for
  `wasm_functype_new()` (both vectors and their elements), `wasm_globaltype_new()` (the content),
  `wasm_tabletype_new()` (the element type), `wasm_tagtype_new()` (the function type),
  `wasm_importtype_new()` and `wasm_exporttype_new()` (the names and the extern type), and
  `wasm_<name>_vec_new()` (the elements). The one exception is input the function cannot walk: a vector
  whose size no vector can have, or that has a size and no data, is left untouched, and stays the caller's.
- **An isoline_ function that returns an error leaves its arguments untouched.** They stay the caller's,
  `isoline_tabletype_new()`'s element type included.
- **NULL is an argument like any other.** A type function given NULL where a type is due returns NULL,
  or a value `val.h` names for the case (`ISOLINE_VALKIND_INVALID`, `ISOLINE_EXTERNKIND_INVALID`,
  `ISOLINE_MUTABILITY_INVALID`), never aborts; deleting or copying NULL does nothing.
- **A type no module could declare is refused.** `wasm.h` leaves validation open; Isoline's constructors
  return NULL for an unknown kind (`wasm_valtype_new()`) or mutability (`wasm_globaltype_new()`), a table
  whose elements are no reference type (`wasm_tabletype_new()`), limits whose minimum is above their maximum
  or whose sizes pass what the address type reaches (`wasm_tabletype_new()`, `wasm_memorytype_new()`), a tag
  whose function type has results (`wasm_tagtype_new()`), a function type of a vector holding NULL
  (`wasm_functype_new()`), and an import or export of a NULL or unwalkable name or a NULL type
  (`wasm_importtype_new()`, `wasm_exporttype_new()`). A C value without a row in the library's tables is
  refused, never cast to an enumeration it may not name.

## Names

`isoline_` names are spelled out (`isoline_interrupt_handle_t`, never `isoline_intr_t`), with one exception:
an `isoline_` function that extends a `wasm.h` family keeps that family's spelling, so that it sits beside
the `wasm_` functions it extends. The spellings so taken from `wasm.h`:

| Spelling | `wasm.h` family | Isoline's functions so far |
|---|---|---|
| `valtype` | `wasm_valtype_*` | `isoline_valtype_nullability`, `_heap`, `_concrete_index` |
| `memorytype` | `wasm_memorytype_*` | `isoline_memorytype_new`, `_limits64`, `_page_size_log2`, `_address_bits` |
| `tabletype` | `wasm_tabletype_*` | `isoline_tabletype_new`, `_limits64`, `_address_bits` |
| `externtype` | `wasm_externtype_*` | none yet |
| `func` | `wasm_func_*` | none yet |
| `val` | `wasm_val_*` | none yet |

## Value kinds

`wasm_valkind_t` names i32, i64, f32 and f64 (0–3), externref and funcref (128, 129), and nothing for
v128 or exnref. `wasm.h` reads a kind below 128 as a number (`wasm_valkind_is_num()`) and every other as a
reference (`wasm_valkind_is_ref()`). Isoline numbers the two it adds on the side of 128 those tests read
right, until upstream numbers them:

| Kind | Value | Origin |
|---|---|---|
| `WASM_I32` … `WASM_F64` | 0–3 | wasm.h |
| `WASM_EXTERNREF`, `WASM_FUNCREF` | 128, 129 | wasm.h |
| `ISOLINE_VALKIND_V128` | `0x7B` | Isoline: v128's byte in the binary format, a number to `wasm_valkind_is_num()` |
| `ISOLINE_VALKIND_EXNREF` | `0xF1` | Isoline: from the range `0xF0`–`0xFF` it keeps for itself, a reference |
| `ISOLINE_VALKIND_INVALID` | `0xFF` | Isoline: what `wasm_valtype_kind(NULL)` gives; no type has it |

`ISOLINE_VALKIND_INVALID` is `0xFF`, which `wasm_valkind_is_ref()` reports as a reference: test a kind for
it before asking `wasm.h`. `isoline_valtype_heap()` is the reliable test of a type
(`ISOLINE_HEAP_NOT_REFERENCE` for numbers and vectors).

No `wasm_` function makes a `wasm_val_t` of kind `ISOLINE_VALKIND_V128`: `wasm_val_t` has no 16-byte member
to hold one.

The numbering is to be coordinated upstream (an issue on WebAssembly/wasm-c-api proposing kinds for v128
and exnref, drafted and not yet filed); until a number is agreed there, Isoline's stay where they are.

A reference type's kind names its hierarchy: `(ref func)`, `nullfuncref` and a module's `(ref $t)` all
report `WASM_FUNCREF`. `isoline_valtype_heap()`, `isoline_valtype_nullability()` and
`isoline_valtype_concrete_index()` tell them apart. `wasm_valtype_new()` makes the nullable abstract
references only.

## Limits

`wasm_limits_t` holds 32-bit sizes. For a memory or table whose sizes need more (memory64, table64),
`wasm_memorytype_limits()` and `wasm_tabletype_limits()` report each size past `UINT32_MAX` as
`wasm_limits_max_default`, which `wasm.h` also uses for "no maximum"; the clamp goes one way only.
`isoline_memorytype_limits64()` and `isoline_tabletype_limits64()` give the sizes exactly, in an
`isoline_limits64_t`, and `isoline_memorytype_new()` and `isoline_tabletype_new()` make types of any
address width and, for memories, either page size custom-page-sizes admits (64 KiB or 1 byte).

`isoline_limits64_t` is size first: the caller sets `struct_size = sizeof(isoline_limits64_t)` of the
header it was built with. The library accepts any `struct_size` from 32, the size the struct was first
published with, up to 4096, and refuses every other with `ISOLINE_ERROR_INVALID_ARGUMENT`. It reads and
writes only the fields that fit in `struct_size`: a caller built against an older, smaller struct gets the
prefix it knows, and nothing past `struct_size` is written. Read as input, a nonzero `reserved` byte, a
nonzero `maximum` without `ISOLINE_MAXIMUM_GIVEN`, and, in a struct larger than the library's, a nonzero
byte past its fields (a later minor's field it does not know) are `ISOLINE_ERROR_INVALID_ARGUMENT`. Written
as output, every byte past the library's fields, up to `struct_size`, is zeroed.

A size-first struct is otherwise an output's shape. `isoline_memorytype_new()` and `isoline_tabletype_new()`
take one as input, because `wasm.h`'s `wasm_limits_t` cannot hold 64-bit limits; the same prefix rule holds
for them.

## Errors

A fallible `isoline_` function returns an `isoline_error_t*`, NULL on success. An error has a code, which
says what failed, and a detail, which says why: a number from its code's family of details, 0 when its
code has none or the family names no cause. Codes and details are stable: a new one is appended, and none
is renumbered. `error.h` names the codes, and the details of a refused type
(`ISOLINE_INVALID_TYPE_PAGE_SIZE` is 2, `ISOLINE_INVALID_TYPE_NON_REFERENCE_ELEMENT` is 4); the tables below
give every value.

<!-- c-api-error-docs: begin (generated by scripts/c-api-error-docs.py; do not edit) -->

| Code | Value | Meaning | Detail |
|---|---|---|---|
| `ISOLINE_ERROR_INVALID_ARGUMENT` | 1 | An argument is not one the function accepts. | [InvalidTypeReason](#details-invalidtypereason) |
| `ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED` | 2 | The host ran out of memory. | always 0 |
| `ISOLINE_ERROR_UNKNOWN_OPTION` | 3 | An option key the library does not know. | always 0 |
| `ISOLINE_ERROR_UNMET_FEATURE_PREREQUISITE` | 4 | A feature enabled without one it needs. | always 0 |
| `ISOLINE_ERROR_ENGINE_UNAVAILABLE` | 5 | No engine of the kind asked for is available. | always 0 |
| `ISOLINE_ERROR_MODULE_TOO_LARGE` | 6 | A module past the engine's size limit. | always 0 |
| `ISOLINE_ERROR_DECODE` | 7 | A module that is malformed. | [DecodeErrorCode](#details-decodeerrorcode) |
| `ISOLINE_ERROR_VALIDATION` | 8 | A module that is invalid. | [ValidationErrorCode](#details-validationerrorcode) |
| `ISOLINE_ERROR_COMPILE_LIMIT_EXCEEDED` | 9 | A module past what the engine compiles. | always 0 |
| `ISOLINE_ERROR_ENGINE_MISMATCH` | 10 | A module loaded by an engine of another identity. | always 0 |
| `ISOLINE_ERROR_FOREIGN_STORE` | 11 | An entity or a handle of another store. | always 0 |
| `ISOLINE_ERROR_STORE_DELETED` | 12 | A handle whose store is deleted. | always 0 |
| `ISOLINE_ERROR_UNKNOWN_IMPORT` | 13 | An import nothing was given for. | always 0 |
| `ISOLINE_ERROR_INCOMPATIBLE_IMPORT` | 14 | An import given an extern of another type. | always 0 |
| `ISOLINE_ERROR_IMPORT_COUNT_MISMATCH` | 15 | Not one extern per import. | always 0 |
| `ISOLINE_ERROR_LIMIT_EXCEEDED` | 16 | One of the store's limits would be exceeded. | [Limit](#details-limit) |
| `ISOLINE_ERROR_ALLOCATION_FAILURE` | 17 | A memory or table could not be allocated. | [Refusal](#details-refusal) |
| `ISOLINE_ERROR_CALL` | 18 | A call into the guest failed. | [CallFailure](#details-callfailure) |
| `ISOLINE_ERROR_THREAD_PREPARATION_UNAVAILABLE` | 19 | The thread could not be prepared. | [PrepareError](#details-prepareerror) |
| `ISOLINE_ERROR_CPU_FEATURES_MISSING` | 20 | The host CPU lacks what the engine needs. | always 0 |
| `ISOLINE_ERROR_VALUE_MISMATCH` | 21 | A value not of the type it must have. | [MismatchKind](#details-mismatchkind) |
| `ISOLINE_ERROR_OUT_OF_BOUNDS` | 22 | An index or range outside its entity. | always 0 |
| `ISOLINE_ERROR_GROWTH_REFUSED` | 23 | A memory or table was not grown. | [Refusal](#details-refusal) |
| `ISOLINE_ERROR_IMMUTABLE_GLOBAL` | 24 | A const global that was to be set. | always 0 |
| `ISOLINE_ERROR_PRECOMPILED_REFUSED` | 25 | Precompiled code that was refused. | always 0 |
| `ISOLINE_ERROR_CACHE_KEY` | 26 | A cache key that could not be made. | always 0 |
| `ISOLINE_ERROR_CACHE_STORAGE` | 27 | The cache's storage failed. | always 0 |
| `ISOLINE_ERROR_UNSUPPORTED_VALUE` | 28 | A value the function cannot pass. | always 0 |
| `ISOLINE_ERROR_EMBEDDER` | 29 | The embedder's own error (isoline_error_new). | always 0 |
| `ISOLINE_ERROR_REENTRANT_CALL` | 30 | A call not allowed from inside a callback. | always 0 |

<a id="details-invalidtypereason"></a>
### Details: InvalidTypeReason

Reported with `ISOLINE_ERROR_INVALID_ARGUMENT`.

| Detail | Cause |
|---|---|
| 1 | `ConcreteTypeIndex` |
| 2 | `PageSize` |
| 3 | `BottomHeap` |
| 4 | `NonReferenceElement` |

<a id="details-decodeerrorcode"></a>
### Details: DecodeErrorCode

Reported with `ISOLINE_ERROR_DECODE`.

| Detail | Cause |
|---|---|
| 1 | `UnexpectedEnd` |
| 2 | `MagicHeaderNotDetected` |
| 3 | `UnknownBinaryVersion` |
| 4 | `IntegerTooLarge` |
| 5 | `IntegerRepresentationTooLong` |
| 6 | `MalformedSectionId` |
| 7 | `SectionSizeMismatch` |
| 8 | `JunkAfterLastSection` |
| 9 | `MalformedUtf8Encoding` |
| 10 | `TooManyLocals` |
| 11 | `LocalCountOverflow` |
| 12 | `IllegalOpcode` |
| 13 | `MalformedReferenceType` |
| 14 | `MalformedImportKind` |
| 15 | `MalformedMutability` |
| 16 | `IntegerTooLargeForLimits` |
| 17 | `LimitsFlagTooLarge` |
| 18 | `FunctionAndCodeSectionInconsistent` |
| 19 | `DataCountAndDataSectionInconsistent` |
| 20 | `DataCountSectionRequired` |
| 21 | `UnexpectedContentAfterEnd` |
| 22 | `MalformedElementSegmentKind` |
| 23 | `ZeroByteExpected` |
| 24 | `LengthOutOfBounds` |
| 25 | `ImplementationLimitExceeded` |
| 26 | `DuplicateSection` |
| 27 | `SectionOutOfOrder` |
| 28 | `MalformedExportKind` |
| 29 | `MalformedElementKind` |
| 30 | `MalformedDataSegmentKind` |
| 31 | `MalformedFunctionType` |
| 32 | `EndOpcodeExpected` |
| 33 | `MalformedMemopFlags` |
| 34 | `MissingFeaturePrerequisite` |
| 35 | `MalformedLimitsFlags` |
| 36 | `InvalidCustomPageSize` |
| 37 | `MalformedCatchClause` |
| 38 | `NameSubsectionSizeMismatch` |
| 39 | `NameIndexOutOfOrder` |

<a id="details-validationerrorcode"></a>
### Details: ValidationErrorCode

Reported with `ISOLINE_ERROR_VALIDATION`.

| Detail | Cause |
|---|---|
| 1 | `TypeMismatch` |
| 2 | `UnknownLocal` |
| 3 | `UnknownGlobal` |
| 4 | `UnknownFunction` |
| 5 | `UnknownTable` |
| 6 | `UnknownMemory` |
| 7 | `UnknownType` |
| 8 | `UnknownLabel` |
| 9 | `UnknownElemSegment` |
| 10 | `UnknownDataSegment` |
| 11 | `InvalidResultArity` |
| 12 | `ImmutableGlobal` |
| 13 | `ConstantExpressionRequired` |
| 14 | `DuplicateExportName` |
| 15 | `MultipleMemories` |
| 16 | `MultipleTables` |
| 17 | `AlignmentMustNotBeLarger` |
| 18 | `MemorySizeMustBeAtMost` |
| 19 | `SizeMinimumMustNotBeGreaterThanMaximum` |
| 20 | `StartFunction` |
| 21 | `UndeclaredFunctionReference` |
| 22 | `InvalidLaneIndex` |
| 23 | `UnknownTag` |
| 24 | `OffsetOutOfRange` |
| 25 | `TableSizeMustBeAtMost` |
| 26 | `InvalidCustomPageSize` |
| 27 | `UninitializedLocal` |
| 28 | `NonEmptyTagResultType` |
| 29 | `OperandStackLimitExceeded` |

<a id="details-limit"></a>
### Details: Limit

Reported with `ISOLINE_ERROR_LIMIT_EXCEEDED`.

| Detail | Cause |
|---|---|
| 1 | `CallDepth` |
| 2 | `StackValues` |
| 3 | `NestedInvocations` |
| 4 | `MemoryBytes` |
| 5 | `TableElements` |
| 6 | `Memories` |
| 7 | `Tables` |
| 8 | `Instances` |
| 9 | `Exceptions` |
| 10 | `ExceptionValues` |
| 11 | `Types` |
| 12 | `Tags` |
| 13 | `Steps` |

<a id="details-refusal"></a>
### Details: Refusal

Reported with `ISOLINE_ERROR_ALLOCATION_FAILURE`, `ISOLINE_ERROR_GROWTH_REFUSED`.

| Detail | Cause |
|---|---|
| 1 | `TypeMaximum` |
| 2 | `Limit` |
| 3 | `HostMemory` |
| 4 | `Guard` |
| 5 | `Embedder` |

<a id="details-callfailure"></a>
### Details: CallFailure

Reported with `ISOLINE_ERROR_CALL`.

| Detail | Cause |
|---|---|
| 1 | `Trap` |
| 2 | `Interrupted` |
| 3 | `HostFailure` |
| 4 | `UncaughtException` |
| 5 | `ArgumentMismatch` |
| 6 | `ForeignHandle` |
| 7 | `StoreDeleted` |

<a id="details-prepareerror"></a>
### Details: PrepareError

Reported with `ISOLINE_ERROR_THREAD_PREPARATION_UNAVAILABLE`.

| Detail | Cause |
|---|---|
| 1 | `Unavailable` |

<a id="details-mismatchkind"></a>
### Details: MismatchKind

Reported with `ISOLINE_ERROR_VALUE_MISMATCH`.

| Detail | Cause |
|---|---|
| 1 | `Count` |
| 2 | `Kind` |
| 3 | `Heap` |
| 4 | `NotNullable` |
| 5 | `ForeignReference` |

<!-- c-api-error-docs: end -->
