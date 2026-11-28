// SPDX-License-Identifier: Apache-2.0
#pragma once

// The code-to-runtime layout: the two records generated code reaches the runtime through, at fixed
// offsets from its two pinned registers, and a row table per record that names every field's offset
// and size. Generated code addresses a field by its row, never by a literal, so the tables are the
// one place the layout is spelled; every CI host compares them against one checked-in golden
// (tests/determinism/context-layout.golden), so that generated code is the same on every host and
// standard library (I9). Every field is a fixed-width scalar, a pointer, or a record of those.
//
// Pure data: the executor (P5 Task 15) fills the records, generated code reads and writes them.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/RawValue.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::compiler
{

/// What a store's execution has pending when generated code leaves through the trap exit.
enum class PendingKind : uint8_t
{
    None,       ///< Nothing: a normal return.
    Trap,       ///< Generated code trapped with PendingAbrupt::code.
    StubAbrupt, ///< A runtime stub failed; its full Abrupt is in the executor's per-store record.
};

/// Whether a pending trap names a subject (PendingAbrupt::subjectBits).
enum class SubjectPresence : uint8_t
{
    Absent,  ///< No subject.
    Present, ///< subjectBits holds it: the element index of an UndefinedElement or UninitializedElement.
};

/// How generated code leaves abruptly: what a trap site writes before it jumps to the trampoline's
/// trap exit, and what the executor turns into the invocation's Abrupt. Every field is fixed-width and
/// trivially copyable, so that the word is the same for every engine that writes it (P7's trap code).
struct PendingAbrupt
{
    PendingKind kind = PendingKind::None;                    ///< What is pending.
    runtime::TrapCode code = runtime::TrapCode::Unreachable; ///< The trap, for PendingKind::Trap.
    SubjectPresence subject = SubjectPresence::Absent; ///< Whether subjectBits names the trap's subject.
    uint64_t subjectBits = 0; ///< The subject, zero-extended: an element index, for one.
};

/// A store's execution as generated code sees it: one per store and executor, shared by the
/// invocations host functions nest, pointed at by a pinned register (r15 on x86-64).
struct ExecutionState
{
    /// The store's interrupt word (runtime::Interrupter::flagForGeneratedCode()), which every poll's
    /// fast path loads as a hint.
    std::atomic<uint8_t> const* interruptFlag = nullptr;
    uint64_t stepsLeft = 0;       ///< The steps the lease left (runtime::Lease::steps).
    uint64_t stackValuesLeft = 0; ///< The stack values the lease left (runtime::Lease::stackValues).
    uint32_t callDepthLeft = 0;   ///< The calls that may still nest, across every invocation in progress.
    std::uintptr_t nativeStackLimit = 0;             ///< The lowest stack address a prologue may reach.
    std::uintptr_t entryStackPointer = 0;            ///< The stack pointer the trap exit restores.
    void const* const* stubs = nullptr;              ///< The runtime stubs' entries, by their table's row.
    runtime::DispatchView const* dispatch = nullptr; ///< The store's dispatch rows.
    PendingAbrupt pending {};                        ///< What a trap exit leaves pending.
    /// The host-call scratch: max(params, results) of the widest host function called so far.
    runtime::RawValue* hostScratch = nullptr;
    uint32_t hostScratchCapacity = 0; ///< How many values hostScratch holds.
    /// The executor's per-store record: the store, the executor, the poll gate, a stub's full Abrupt,
    /// the lease and the per-invocation saves. Stubs reach the store only through it.
    void* executorState = nullptr;
};

/// One instance as generated code sees it, pointed at by a pinned register (r14 on x86-64) and by its
/// functions' dispatch rows. Every pointer leads into the store's own records, so every grow path and
/// every executor stays coherent with it.
struct InstanceContext
{
    runtime::MemoryView const* memory0 = nullptr;         ///< Memory 0's view; null without one.
    runtime::MemoryView const* const* memories = nullptr; ///< Every memory's view, by memory index.
    runtime::TableView const* const* tables = nullptr;    ///< Every table's view, by table index.
    runtime::RawValue* const* globals = nullptr;          ///< Every global's cell, by global index.
    runtime::TypeId const* typeIds = nullptr;             ///< Every type's TypeId, by type index.
    runtime::FuncAddr const* functions = nullptr;         ///< Every function's address, by function index.
    uint32_t moduleInstance = 0;                          ///< The instance's address in its store.
};

/// A runtime routine generated code calls (callStub's `stub` immediate), by its row in the execution
/// layer's RuntimeStubTable (rows filled in P5 Task 14), which reaches it through ExecutionState::stubs.
/// There is no trap stub: a trap is code that leaves through the trampoline's trap exit.
///
/// Translation passes a stub its instruction's indices first, each an I32 constant (a memory, a table, a
/// segment, in the instruction's immediate order), then its WebAssembly operands, every I32 among them
/// zero-extended to an I64, so that one signature serves 32- and 64-bit memories and tables alike.
/// memory.grow and table.grow return their old size, or -1, as an I64, which translation narrows for a
/// 32-bit memory or table; table.get returns the Ref's word. The bulk stubs poll their granules.
enum class RuntimeStub : uint8_t
{
    PollSlow,    ///< A poll's slow path: an interrupt, the step budget, or an observer.
    MemoryGrow,  ///< memory.grow: (memory, delta) → old size or -1.
    MemoryFill,  ///< memory.fill: (memory, destination, value, count).
    MemoryCopy,  ///< memory.copy: (destination memory, source memory, destination, source, count).
    MemoryInit,  ///< memory.init: (segment, memory, destination, source, count).
    DataDrop,    ///< data.drop: (segment).
    TableGrow,   ///< table.grow: (table, initial, delta) → old size or -1.
    TableFill,   ///< table.fill: (table, destination, value, count).
    TableCopy,   ///< table.copy: (destination table, source table, destination, source, count).
    TableInit,   ///< table.init: (segment, table, destination, source, count).
    ElementDrop, ///< elem.drop: (segment).
    TableGet,    ///< table.get of an imported table: (table, index) → the element.
    TableSet,    ///< table.set of an imported table: (table, index, value).
    CallHost,    ///< A call of a host function, through ExecutionState::hostScratch.
    FloatRound,  ///< A rounding the CPU cannot do inline (no SSE4.1): numerics:: on the host.
};

/// The last RuntimeStub enumerator, for checking RuntimeStubTable's size.
inline constexpr auto LastRuntimeStub = RuntimeStub::FloatRound;

/// What every runtime stub returns: its status, and its value for a stub that has one (memory.grow's old
/// size, table.get's element, FloatRound's bits); 0 when it has none. A stub is a non-member function
/// `StubResult stub(ExecutionState*, InstanceContext*, uint64_t...)`, its operands each one 64-bit word
/// (RuntimeStub's order), so that one host-call shape serves every stub (P5 Decision "Calling conventions
/// are rows", I8): two words, which System V returns in rax:rdx and Microsoft x64 through a hidden pointer.
/// A status other than StubSucceeded means the stub left PendingKind::StubAbrupt pending: generated code
/// then leaves through the trap exit without writing pending itself.
struct StubResult
{
    uint64_t status = 0; ///< StubSucceeded, or anything else when the stub left its Abrupt pending.
    uint64_t value = 0;  ///< The stub's value; 0 when it has none.
};

/// The status of a stub that succeeded.
inline constexpr auto StubSucceeded = uint64_t { 0 };

/// The rounding FloatRound performs: its first operand. FloatRound's second is the value's bits (an F32's
/// zero-extended), and its value the rounded bits, computed by numerics:: as both interpreters do.
enum class FloatRoundOperation : uint8_t
{
    F32Ceil,    ///< f32.ceil.
    F32Floor,   ///< f32.floor.
    F32Trunc,   ///< f32.trunc.
    F32Nearest, ///< f32.nearest.
    F64Ceil,    ///< f64.ceil.
    F64Floor,   ///< f64.floor.
    F64Trunc,   ///< f64.trunc.
    F64Nearest, ///< f64.nearest.
};

/// A field of PendingAbrupt, in declaration order.
enum class PendingAbruptField : uint8_t
{
    Kind,
    Code,
    Subject,
    SubjectBits,
};

/// A field of ExecutionState, in declaration order.
enum class ExecutionStateField : uint8_t
{
    InterruptFlag,
    StepsLeft,
    StackValuesLeft,
    CallDepthLeft,
    NativeStackLimit,
    EntryStackPointer,
    Stubs,
    Dispatch,
    Pending,
    HostScratch,
    HostScratchCapacity,
    ExecutorState,
};

/// A field of InstanceContext, in declaration order.
enum class InstanceContextField : uint8_t
{
    Memory0,
    Memories,
    Tables,
    Globals,
    TypeIds,
    Functions,
    ModuleInstance,
};

/// A field of runtime::MemoryView, in declaration order.
enum class MemoryViewField : uint8_t
{
    Base,
    Bytes,
};

/// A field of runtime::TableView, in declaration order.
enum class TableViewField : uint8_t
{
    Data,
    Size,
};

/// A field of runtime::FunctionDispatch, in declaration order.
enum class FunctionDispatchField : uint8_t
{
    Code,
    Context,
    TypeId,
};

/// A field of runtime::DispatchView, in declaration order.
enum class DispatchViewField : uint8_t
{
    Chunks,
    Rows,
};

/// A field of runtime::Ref as generated code reads and writes a table element in place (RefLayout), in the
/// order of its offsets.
enum class RefField : uint8_t
{
    Kind,
    HeapKind,
    HeapTypeIndex,
    Address,
};

/// Where a field of a record lies.
template <typename Field>
struct ContextLayoutRow
{
    Field field {};            ///< The field; equal to the row's index.
    std::string_view name;     ///< Its member's name, as the golden spells it.
    std::size_t offset = 0;    ///< offsetof its member.
    std::size_t size = 0;      ///< sizeof its member's type.
    std::size_t alignment = 0; ///< alignof its member's type.
};

/// PendingAbrupt's fields.
inline constexpr auto PendingAbruptLayout = std::to_array<ContextLayoutRow<PendingAbruptField>>({
    { .field = PendingAbruptField::Kind,
      .name = "kind",
      .offset = offsetof(PendingAbrupt, kind),
      .size = sizeof(decltype(PendingAbrupt::kind)),
      .alignment = alignof(decltype(PendingAbrupt::kind)) },
    { .field = PendingAbruptField::Code,
      .name = "code",
      .offset = offsetof(PendingAbrupt, code),
      .size = sizeof(decltype(PendingAbrupt::code)),
      .alignment = alignof(decltype(PendingAbrupt::code)) },
    { .field = PendingAbruptField::Subject,
      .name = "subject",
      .offset = offsetof(PendingAbrupt, subject),
      .size = sizeof(decltype(PendingAbrupt::subject)),
      .alignment = alignof(decltype(PendingAbrupt::subject)) },
    { .field = PendingAbruptField::SubjectBits,
      .name = "subjectBits",
      .offset = offsetof(PendingAbrupt, subjectBits),
      .size = sizeof(decltype(PendingAbrupt::subjectBits)),
      .alignment = alignof(decltype(PendingAbrupt::subjectBits)) },
});

/// ExecutionState's fields.
inline constexpr auto ExecutionStateLayout = std::to_array<ContextLayoutRow<ExecutionStateField>>({
    { .field = ExecutionStateField::InterruptFlag,
      .name = "interruptFlag",
      .offset = offsetof(ExecutionState, interruptFlag),
      .size = sizeof(decltype(ExecutionState::interruptFlag)),
      .alignment = alignof(decltype(ExecutionState::interruptFlag)) },
    { .field = ExecutionStateField::StepsLeft,
      .name = "stepsLeft",
      .offset = offsetof(ExecutionState, stepsLeft),
      .size = sizeof(decltype(ExecutionState::stepsLeft)),
      .alignment = alignof(decltype(ExecutionState::stepsLeft)) },
    { .field = ExecutionStateField::StackValuesLeft,
      .name = "stackValuesLeft",
      .offset = offsetof(ExecutionState, stackValuesLeft),
      .size = sizeof(decltype(ExecutionState::stackValuesLeft)),
      .alignment = alignof(decltype(ExecutionState::stackValuesLeft)) },
    { .field = ExecutionStateField::CallDepthLeft,
      .name = "callDepthLeft",
      .offset = offsetof(ExecutionState, callDepthLeft),
      .size = sizeof(decltype(ExecutionState::callDepthLeft)),
      .alignment = alignof(decltype(ExecutionState::callDepthLeft)) },
    { .field = ExecutionStateField::NativeStackLimit,
      .name = "nativeStackLimit",
      .offset = offsetof(ExecutionState, nativeStackLimit),
      .size = sizeof(decltype(ExecutionState::nativeStackLimit)),
      .alignment = alignof(decltype(ExecutionState::nativeStackLimit)) },
    { .field = ExecutionStateField::EntryStackPointer,
      .name = "entryStackPointer",
      .offset = offsetof(ExecutionState, entryStackPointer),
      .size = sizeof(decltype(ExecutionState::entryStackPointer)),
      .alignment = alignof(decltype(ExecutionState::entryStackPointer)) },
    { .field = ExecutionStateField::Stubs,
      .name = "stubs",
      .offset = offsetof(ExecutionState, stubs),
      .size = sizeof(decltype(ExecutionState::stubs)),
      .alignment = alignof(decltype(ExecutionState::stubs)) },
    { .field = ExecutionStateField::Dispatch,
      .name = "dispatch",
      .offset = offsetof(ExecutionState, dispatch),
      .size = sizeof(decltype(ExecutionState::dispatch)),
      .alignment = alignof(decltype(ExecutionState::dispatch)) },
    { .field = ExecutionStateField::Pending,
      .name = "pending",
      .offset = offsetof(ExecutionState, pending),
      .size = sizeof(decltype(ExecutionState::pending)),
      .alignment = alignof(decltype(ExecutionState::pending)) },
    { .field = ExecutionStateField::HostScratch,
      .name = "hostScratch",
      .offset = offsetof(ExecutionState, hostScratch),
      .size = sizeof(decltype(ExecutionState::hostScratch)),
      .alignment = alignof(decltype(ExecutionState::hostScratch)) },
    { .field = ExecutionStateField::HostScratchCapacity,
      .name = "hostScratchCapacity",
      .offset = offsetof(ExecutionState, hostScratchCapacity),
      .size = sizeof(decltype(ExecutionState::hostScratchCapacity)),
      .alignment = alignof(decltype(ExecutionState::hostScratchCapacity)) },
    { .field = ExecutionStateField::ExecutorState,
      .name = "executorState",
      .offset = offsetof(ExecutionState, executorState),
      .size = sizeof(decltype(ExecutionState::executorState)),
      .alignment = alignof(decltype(ExecutionState::executorState)) },
});

/// InstanceContext's fields.
inline constexpr auto InstanceContextLayout = std::to_array<ContextLayoutRow<InstanceContextField>>({
    { .field = InstanceContextField::Memory0,
      .name = "memory0",
      .offset = offsetof(InstanceContext, memory0),
      .size = sizeof(decltype(InstanceContext::memory0)),
      .alignment = alignof(decltype(InstanceContext::memory0)) },
    { .field = InstanceContextField::Memories,
      .name = "memories",
      .offset = offsetof(InstanceContext, memories),
      .size = sizeof(decltype(InstanceContext::memories)),
      .alignment = alignof(decltype(InstanceContext::memories)) },
    { .field = InstanceContextField::Tables,
      .name = "tables",
      .offset = offsetof(InstanceContext, tables),
      .size = sizeof(decltype(InstanceContext::tables)),
      .alignment = alignof(decltype(InstanceContext::tables)) },
    { .field = InstanceContextField::Globals,
      .name = "globals",
      .offset = offsetof(InstanceContext, globals),
      .size = sizeof(decltype(InstanceContext::globals)),
      .alignment = alignof(decltype(InstanceContext::globals)) },
    { .field = InstanceContextField::TypeIds,
      .name = "typeIds",
      .offset = offsetof(InstanceContext, typeIds),
      .size = sizeof(decltype(InstanceContext::typeIds)),
      .alignment = alignof(decltype(InstanceContext::typeIds)) },
    { .field = InstanceContextField::Functions,
      .name = "functions",
      .offset = offsetof(InstanceContext, functions),
      .size = sizeof(decltype(InstanceContext::functions)),
      .alignment = alignof(decltype(InstanceContext::functions)) },
    { .field = InstanceContextField::ModuleInstance,
      .name = "moduleInstance",
      .offset = offsetof(InstanceContext, moduleInstance),
      .size = sizeof(decltype(InstanceContext::moduleInstance)),
      .alignment = alignof(decltype(InstanceContext::moduleInstance)) },
});

/// runtime::MemoryView's fields: what memoryBase, memoryBound and memory 0's context values load.
inline constexpr auto MemoryViewLayout = std::to_array<ContextLayoutRow<MemoryViewField>>({
    { .field = MemoryViewField::Base,
      .name = "base",
      .offset = offsetof(runtime::MemoryView, base),
      .size = sizeof(decltype(runtime::MemoryView::base)),
      .alignment = alignof(decltype(runtime::MemoryView::base)) },
    { .field = MemoryViewField::Bytes,
      .name = "bytes",
      .offset = offsetof(runtime::MemoryView, bytes),
      .size = sizeof(decltype(runtime::MemoryView::bytes)),
      .alignment = alignof(decltype(runtime::MemoryView::bytes)) },
});

/// runtime::TableView's fields: what checkTable and tableSize load.
inline constexpr auto TableViewLayout = std::to_array<ContextLayoutRow<TableViewField>>({
    { .field = TableViewField::Data,
      .name = "data",
      .offset = offsetof(runtime::TableView, data),
      .size = sizeof(decltype(runtime::TableView::data)),
      .alignment = alignof(decltype(runtime::TableView::data)) },
    { .field = TableViewField::Size,
      .name = "size",
      .offset = offsetof(runtime::TableView, size),
      .size = sizeof(decltype(runtime::TableView::size)),
      .alignment = alignof(decltype(runtime::TableView::size)) },
});

/// runtime::FunctionDispatch's fields: what a call through a dispatch row and loadDispatchType load.
inline constexpr auto FunctionDispatchLayout = std::to_array<ContextLayoutRow<FunctionDispatchField>>({
    { .field = FunctionDispatchField::Code,
      .name = "code",
      .offset = offsetof(runtime::FunctionDispatch, code),
      .size = sizeof(decltype(runtime::FunctionDispatch::code)),
      .alignment = alignof(decltype(runtime::FunctionDispatch::code)) },
    { .field = FunctionDispatchField::Context,
      .name = "context",
      .offset = offsetof(runtime::FunctionDispatch, context),
      .size = sizeof(decltype(runtime::FunctionDispatch::context)),
      .alignment = alignof(decltype(runtime::FunctionDispatch::context)) },
    { .field = FunctionDispatchField::TypeId,
      .name = "typeId",
      .offset = offsetof(runtime::FunctionDispatch, typeId),
      .size = sizeof(decltype(runtime::FunctionDispatch::typeId)),
      .alignment = alignof(decltype(runtime::FunctionDispatch::typeId)) },
});

/// runtime::DispatchView's fields: what checkDispatch and every call through the dispatch rows load.
inline constexpr auto DispatchViewLayout = std::to_array<ContextLayoutRow<DispatchViewField>>({
    { .field = DispatchViewField::Chunks,
      .name = "chunks",
      .offset = offsetof(runtime::DispatchView, chunks),
      .size = sizeof(decltype(runtime::DispatchView::chunks)),
      .alignment = alignof(decltype(runtime::DispatchView::chunks)) },
    { .field = DispatchViewField::Rows,
      .name = "rows",
      .offset = offsetof(runtime::DispatchView, rows),
      .size = sizeof(decltype(runtime::DispatchView::rows)),
      .alignment = alignof(decltype(runtime::DispatchView::rows)) },
});

/// runtime::Ref's fields as runtime::RefLayoutOfRef places them: what a table element is read and written by,
/// field by field. Ref's heap is a record of its own, so these rows name the fields generated code touches
/// rather than Ref's members (rowsCoverEveryMember() does not apply); the golden pins them all the same.
inline constexpr auto RefFieldLayout = std::to_array<ContextLayoutRow<RefField>>({
    { .field = RefField::Kind,
      .name = "kind",
      .offset = runtime::RefLayoutOfRef.kindOffset,
      .size = sizeof(runtime::RefKind),
      .alignment = alignof(runtime::RefKind) },
    { .field = RefField::HeapKind,
      .name = "heap.kind",
      .offset = runtime::RefLayoutOfRef.heapKindOffset,
      .size = sizeof(binary::AbstractHeapType),
      .alignment = alignof(binary::AbstractHeapType) },
    { .field = RefField::HeapTypeIndex,
      .name = "heap.typeIndex",
      .offset = runtime::RefLayoutOfRef.heapTypeIndexOffset,
      .size = sizeof(decltype(binary::HeapType::typeIndex)),
      .alignment = alignof(decltype(binary::HeapType::typeIndex)) },
    { .field = RefField::Address,
      .name = "address",
      .offset = runtime::RefLayoutOfRef.addressOffset,
      .size = sizeof(decltype(runtime::Ref::address)),
      .alignment = alignof(decltype(runtime::Ref::address)) },
});

/// The size of a runtime::Ref: a table's stride, which generated code scales an index by.
inline constexpr auto RefBytes = runtime::RefLayoutOfRef.size;

namespace detail
{
    /// Converts to any member type, in unevaluated brace initialization only: what counts a record's
    /// members (memberCount()).
    struct AnyMember
    {
        template <typename Member>
        // Declared, never defined: only asked whether the conversion exists.
        constexpr operator Member() const noexcept;
    };

    /// AnyMember, once per index of a pack.
    template <std::size_t>
    using AnyMemberAt = AnyMember;

    /// Whether @p Record brace-initializes from as many values as @p Indices has.
    template <typename Record, std::size_t... Indices>
    [[nodiscard]] consteval bool initializesFrom(std::index_sequence<Indices...> /*indices*/) noexcept
    {
        return requires { Record { AnyMemberAt<Indices> {}... }; };
    }
} // namespace detail

/// How many members @p Record, an aggregate of pointers, scalars and records of those, has: the most
/// values it brace-initializes from, each initializing one member whole.
/// @return The count.
template <typename Record, std::size_t Known = 0>
[[nodiscard]] consteval std::size_t memberCount() noexcept
{
    if constexpr (detail::initializesFrom<Record>(std::make_index_sequence<Known + 1> {}))
        return memberCount<Record, Known + 1>();
    else
        return Known;
}

/// Whether @p rows name every member of @p Record exactly: one row per field in enumerator order and
/// one per member; sorted by offset and disjoint; every gap before a row smaller than its field's
/// alignment, so no member hides in one; and the last row ending where the record does, but for the
/// padding up to the record's own alignment. A member added without a row, a row out of place and a
/// row of the wrong size each fail it.
/// @param rows The record's layout table.
/// @return Whether they cover it.
template <typename Record, typename Field, std::size_t Count>
[[nodiscard]] constexpr bool rowsCoverEveryMember(
    std::array<ContextLayoutRow<Field>, Count> const& rows) noexcept
{
    if (Count != memberCount<Record>())
        return false;
    auto end = std::size_t { 0 };
    auto index = std::size_t { 0 };
    for (auto const& row: rows)
    {
        if (static_cast<std::size_t>(row.field) != index || row.offset < end
            || row.offset - end >= row.alignment)
            return false;
        end = row.offset + row.size;
        ++index;
    }
    return end <= sizeof(Record) && sizeof(Record) - end < alignof(Record);
}

static_assert(std::is_standard_layout_v<PendingAbrupt> && std::is_trivially_copyable_v<PendingAbrupt>);
static_assert(std::is_standard_layout_v<ExecutionState> && std::is_trivially_copyable_v<ExecutionState>);
static_assert(std::is_standard_layout_v<InstanceContext> && std::is_trivially_copyable_v<InstanceContext>);
static_assert(rowsCoverEveryMember<PendingAbrupt>(PendingAbruptLayout));
static_assert(rowsCoverEveryMember<ExecutionState>(ExecutionStateLayout));
static_assert(rowsCoverEveryMember<InstanceContext>(InstanceContextLayout));
static_assert(rowsCoverEveryMember<runtime::MemoryView>(MemoryViewLayout));
static_assert(rowsCoverEveryMember<runtime::TableView>(TableViewLayout));
static_assert(rowsCoverEveryMember<runtime::FunctionDispatch>(FunctionDispatchLayout));
static_assert(rowsCoverEveryMember<runtime::DispatchView>(DispatchViewLayout));
static_assert(
    [] {
        auto index = std::size_t { 0 };
        auto end = std::size_t { 0 };
        for (auto const& row: RefFieldLayout)
        {
            if (static_cast<std::size_t>(row.field) != index++ || row.offset < end)
                return false;
            end = row.offset + row.size;
        }
        return end <= RefBytes;
    }(),
    "RefFieldLayout must hold one row per RefField, in order of disjoint offsets within a Ref");

/// The row of @p field in its record's layout table.
/// @param field The field.
/// @return Its row: where generated code finds it.
[[nodiscard]] constexpr ContextLayoutRow<PendingAbruptField> const& layoutOf(
    PendingAbruptField field) noexcept
{
    return PendingAbruptLayout[static_cast<std::size_t>(field)];
}

/// @copydoc layoutOf(PendingAbruptField)
[[nodiscard]] constexpr ContextLayoutRow<ExecutionStateField> const& layoutOf(
    ExecutionStateField field) noexcept
{
    return ExecutionStateLayout[static_cast<std::size_t>(field)];
}

/// @copydoc layoutOf(PendingAbruptField)
[[nodiscard]] constexpr ContextLayoutRow<InstanceContextField> const& layoutOf(
    InstanceContextField field) noexcept
{
    return InstanceContextLayout[static_cast<std::size_t>(field)];
}

/// @copydoc layoutOf(PendingAbruptField)
[[nodiscard]] constexpr ContextLayoutRow<MemoryViewField> const& layoutOf(MemoryViewField field) noexcept
{
    return MemoryViewLayout[static_cast<std::size_t>(field)];
}

/// @copydoc layoutOf(PendingAbruptField)
[[nodiscard]] constexpr ContextLayoutRow<TableViewField> const& layoutOf(TableViewField field) noexcept
{
    return TableViewLayout[static_cast<std::size_t>(field)];
}

/// @copydoc layoutOf(PendingAbruptField)
[[nodiscard]] constexpr ContextLayoutRow<FunctionDispatchField> const& layoutOf(
    FunctionDispatchField field) noexcept
{
    return FunctionDispatchLayout[static_cast<std::size_t>(field)];
}

/// @copydoc layoutOf(PendingAbruptField)
[[nodiscard]] constexpr ContextLayoutRow<DispatchViewField> const& layoutOf(DispatchViewField field) noexcept
{
    return DispatchViewLayout[static_cast<std::size_t>(field)];
}

/// @copydoc layoutOf(PendingAbruptField)
[[nodiscard]] constexpr ContextLayoutRow<RefField> const& layoutOf(RefField field) noexcept
{
    return RefFieldLayout[static_cast<std::size_t>(field)];
}

} // namespace isoline::compiler
