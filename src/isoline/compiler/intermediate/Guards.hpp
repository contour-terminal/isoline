// SPDX-License-Identifier: Apache-2.0
#pragma once

// The lowering contract of the operations that guard an access: what checkMemory, checkTable,
// checkDispatch and guardAddress compute, exactly, as data and as constexpr functions. Selection (P5
// Task 13) lowers each of them to a compare, a branch to the trap and, under SpectreGuard::Enabled, a
// select that replaces the guarded pointer by 0. The select's condition must be the trap's, bit for bit,
// so that code running ahead of a mispredicted branch reads through a null pointer and never through
// one the guest chose. The functions here are that condition and that pointer: Task 13's lowerings are
// tested against them, and so is anything that later folds or elides a guard (Task 8).
//
// Two rules are easy to lose in a lowering, and the fast interpreter once lost both:
//  - An I32 index is its low 32 bits, zero-extended, whatever its register's high half holds. A 32-bit
//    memory's or table's index is an I32 value, and a lowering that adds the whole register can form an
//    address past the end that its 64-bit comparison never sees.
//  - The sum index + offset + size is taken without wrapping: an access whose end overflows 64 bits (a
//    64-bit memory's index near 2^64, an offset past 2^63) traps, and its select leaves 0 as for any other
//    access past the end. Comparing a wrapped end, or comparing the index against bound - (offset + size)
//    without treating a negative difference as past the end, lets a memory smaller than its access, or an
//    offset that wraps, through.
//
// Nothing but fixed field offsets is ever added to a guarded pointer after its select: a table element's
// RefLayout offsets and a dispatch row's FunctionDispatch offsets, which stay within the element or row the
// select chose or within the first bytes past 0. A load or store has no offset of its own, and every use of
// a pointer is held to come from its guard (GuardedUseTable), so an offset the guest chose never reaches an
// address after its select, where it would make 0 + offset a guest-chosen absolute address under
// misspeculation. Only guardAddress adds a variable offset, and it is itself a guard: 0 stays 0.
//
// Bounds-check elimination (P5 Task 8) removes a check two ways, and only these, each a row here or a
// function below that the verifier and the pass share:
//  - A constant index whose access ends within the memory's declared minimum (guardMemoryAccess() against
//    minimumBytesOf()) becomes inBoundsAddress, which compares nothing and selects nothing: no index the
//    guest chooses reaches it, and the verifier holds its offset + size to the minimum (GuardBound::
//    MemoryMinimum), as memories never shrink and an import's minimum bounds the memory bound to it.
//  - A check whose bytes a dominating check of the same memory and SSA index covered (guardCovers()), with
//    nothing that may grow or move a memory on any path between (MemoryMotionTable), takes that check's
//    address, through guardAddress when its offset is further on. The clamp is the dominating check's
//    select, which guardAddress keeps (0 stays 0): an elided check never means an elided clamp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include <isoline/compiler/intermediate/Operation.hpp>

namespace isoline::compiler::intermediate
{

/// The bit at which a Ref's word holds its heap's kind: the word is kind | heap.kind << 8 | address << 32.
inline constexpr auto RefWordHeapKindShift = 8U;
/// The bit at which a Ref's word holds its address: the word is kind | heap.kind << 8 | address << 32.
inline constexpr auto RefWordAddressShift = 32U;

/// What a guard's index is compared against.
enum class GuardBound : uint8_t
{
    None,         ///< Nothing: guardAddress compares nothing, its dominating check did.
    MemoryBytes,  ///< The memory's bound operand: its size in bytes.
    TableSize,    ///< The table's size (runtime::TableView::size).
    DispatchRows, ///< The store's dispatch rows (runtime::DispatchView::rows).
    /// The memory's declared minimum in bytes, statically: inBoundsAddress compares nothing at run time, and
    /// the verifier holds its offset + size to the minimum it is given.
    MemoryMinimum,
};

/// Whether a guard's end can overflow, and what an overflow does.
enum class EndOverflow : uint8_t
{
    Impossible, ///< The guard adds nothing to its index.
    Traps,      ///< index + offset + size is computed without wrapping; past 2^64 - 1 it traps and selects 0.
};

/// What a guard's trap names as its subject (runtime::Trap::subject), zero-extended, when its trap code is
/// UndefinedElement: the element index.
enum class GuardSubject : uint8_t
{
    None,          ///< Nothing.
    GuardedIndex,  ///< Its guarded index operand: checkTable's.
    SecondOperand, ///< Its second operand: checkDispatch's, the element index the reference came from.
};

/// One guarding operation's lowering contract.
struct GuardRow
{
    Operation operation;  ///< The operation.
    uint8_t indexOperand; ///< Which of its operands the guarded index is.
    GuardBound bound;     ///< What the index is compared against.
    EndOverflow overflow; ///< Whether its end can overflow.
    GuardSubject subject; ///< What its trap names.
    /// The root guard the uses of its pointer name in GuardedUseTable: itself, or the check it stands in for.
    Operation serves;
    /// Which of its operands is its `memory` immediate's base (memory 0's context value, or memoryBase of
    /// it), which the verifier holds it to; NoBaseOperand when it takes none.
    uint8_t baseOperand;
};

/// GuardRow::baseOperand of a guard that takes no memory base.
inline constexpr auto NoBaseOperand = uint8_t { 0xFF };

/// GuardRow::indexOperand of a guard that has no index operand: inBoundsAddress, whose index was a constant.
inline constexpr auto NoIndexOperand = uint8_t { 0xFF };

/// Every operation that returns a guarded Pointer, with its contract. Under SpectreGuard::Enabled each
/// selects 0 exactly where it traps (or, for guardAddress, where its operand is 0; inBoundsAddress traps
/// nowhere and selects nothing).
inline constexpr auto GuardTable = std::to_array<GuardRow>({
    { .operation = Operation::CheckMemory,
      .indexOperand = 0,
      .bound = GuardBound::MemoryBytes,
      .overflow = EndOverflow::Traps,
      .subject = GuardSubject::None,
      .serves = Operation::CheckMemory,
      .baseOperand = 1 },
    { .operation = Operation::GuardAddress,
      .indexOperand = 0,
      .bound = GuardBound::None,
      .overflow = EndOverflow::Impossible,
      .subject = GuardSubject::None,
      .serves = Operation::CheckMemory,
      .baseOperand = NoBaseOperand },
    { .operation = Operation::InBoundsAddress,
      .indexOperand = NoIndexOperand,
      .bound = GuardBound::MemoryMinimum,
      .overflow = EndOverflow::Impossible,
      .subject = GuardSubject::None,
      .serves = Operation::CheckMemory,
      .baseOperand = 0 },
    { .operation = Operation::CheckTable,
      .indexOperand = 0,
      .bound = GuardBound::TableSize,
      .overflow = EndOverflow::Impossible,
      .subject = GuardSubject::GuardedIndex,
      .serves = Operation::CheckTable,
      .baseOperand = NoBaseOperand },
    { .operation = Operation::CheckDispatch,
      .indexOperand = 0,
      .bound = GuardBound::DispatchRows,
      .overflow = EndOverflow::Impossible,
      .subject = GuardSubject::SecondOperand,
      .serves = Operation::CheckDispatch,
      .baseOperand = NoBaseOperand },
});

/// The row of @p operation in GuardTable; none when it guards nothing.
[[nodiscard]] constexpr std::optional<GuardRow> guardRowOf(Operation operation) noexcept
{
    for (auto const& row: GuardTable)
        if (row.operation == operation)
            return row;
    return std::nullopt;
}

static_assert(
    [] {
        for (auto const& row: OperationTable)
        {
            auto const isGuard = row.result == ResultRule::Pointer
                                 && (row.effect == Effect::MayTrap || row.operation == Operation::GuardAddress
                                     || row.operation == Operation::InBoundsAddress);
            if (isGuard != guardRowOf(row.operation).has_value())
                return false;
        }
        return true;
    }(),
    "every operation that returns a guarded Pointer has a GuardTable row, and only those");

static_assert(
    [] {
        for (auto const& row: GuardTable)
        {
            auto const served = guardRowOf(row.serves);
            if (!served || served->serves != row.serves || served->operation == Operation::GuardAddress)
                return false;
        }
        return true;
    }(),
    "every guard serves a root guard that serves itself");

/// The root guard the uses of @p operation's pointer name in GuardedUseTable (GuardRow::serves); itself
/// when it is no guard.
[[nodiscard]] constexpr Operation rootGuardOf(Operation operation) noexcept
{
    auto const row = guardRowOf(operation);
    return row ? row->serves : operation;
}

/// Which immediate of a use must name what its guard named.
enum class GuardScope : uint8_t
{
    None,   ///< None: a dispatch row belongs to the store, not to a table or memory.
    Memory, ///< The `memory` immediate: an access of memory m takes an address checkMemory of m guarded.
    Table,  ///< The `table` immediate: an element of table t comes from checkTable of t.
};

/// How far past its guarded address a use reaches.
enum class UseExtent : uint8_t
{
    Element, ///< The element or row its guard chose, at the guard's own address: no offset on the way.
    Size,    ///< Its `size` immediate's bytes past the address: a load's or store's width.
    Offset,  ///< Its `offset` immediate past the address: guardAddress, which moves it.
};

/// One use of a guarded pointer: where the pointer is, the guard at the root of its guardAddress chain,
/// and what the use must agree on with that guard.
struct GuardedUseRow
{
    Operation operation; ///< The operation that uses a pointer.
    uint8_t operand;     ///< Which of its operands the pointer is.
    Operation guard;     ///< The guard its pointer must come from, through guardAddress only.
    GuardScope scope;    ///< Which immediate must equal the guard's.
    UseExtent extent;    ///< How far past the address it reaches, which the guard must cover.
};

/// Every use of a guarded pointer: what the verifier checks (VerifierErrorCode::UnguardedAddress). A
/// pointer must come from its row's guard, directly or through guardAddress; a memory use must name the
/// guard's memory, and the offsets of its guardAddress chain plus its own extent must stay within the
/// `size` bytes checkMemory covered, summed in 64 bits with an overflow refused. So no pass or translation
/// feeds an access an address that skipped its select, belongs to another memory, or reaches past what was
/// checked.
inline constexpr auto GuardedUseTable = std::to_array<GuardedUseRow>({
    { .operation = Operation::GuardAddress,
      .operand = 0,
      .guard = Operation::CheckMemory,
      .scope = GuardScope::None,
      .extent = UseExtent::Offset },
    { .operation = Operation::Load,
      .operand = 0,
      .guard = Operation::CheckMemory,
      .scope = GuardScope::Memory,
      .extent = UseExtent::Size },
    { .operation = Operation::Store,
      .operand = 0,
      .guard = Operation::CheckMemory,
      .scope = GuardScope::Memory,
      .extent = UseExtent::Size },
    { .operation = Operation::LoadTableElement,
      .operand = 0,
      .guard = Operation::CheckTable,
      .scope = GuardScope::Table,
      .extent = UseExtent::Element },
    { .operation = Operation::StoreTableElement,
      .operand = 0,
      .guard = Operation::CheckTable,
      .scope = GuardScope::Table,
      .extent = UseExtent::Element },
    { .operation = Operation::LoadDispatchType,
      .operand = 0,
      .guard = Operation::CheckDispatch,
      .scope = GuardScope::None,
      .extent = UseExtent::Element },
    { .operation = Operation::CallIndirect,
      .operand = 0,
      .guard = Operation::CheckDispatch,
      .scope = GuardScope::None,
      .extent = UseExtent::Element },
});

/// The row of @p operation in GuardedUseTable; none when it uses no guarded pointer.
[[nodiscard]] constexpr std::optional<GuardedUseRow> guardedUseOf(Operation operation) noexcept
{
    for (auto const& row: GuardedUseTable)
        if (row.operation == operation)
            return row;
    return std::nullopt;
}

static_assert(
    [] {
        for (auto const& row: GuardedUseTable)
            if (!guardRowOf(row.guard).has_value() || row.guard == Operation::GuardAddress)
                return false;
        return true;
    }(),
    "every guard GuardedUseTable names is a root guard of GuardTable: guardAddress is a link of a chain");

static_assert(
    [] {
        for (auto const& row: OperationTable)
        {
            auto const takesPointer = row.operands == OperandSignature::Pointer
                                      || row.operands == OperandSignature::Store
                                      || row.operands == OperandSignature::StoreReference
                                      || row.operands == OperandSignature::PointerVariadic;
            if (takesPointer && !guardedUseOf(row.operation).has_value())
                return false;
        }
        return true;
    }(),
    "every operation that takes a Pointer operand has a GuardedUseTable row");

/// The index a guard reads from an operand of @p type holding @p bits: an I32's low 32 bits,
/// zero-extended; an I64's 64.
[[nodiscard]] constexpr uint64_t guardedIndexOf(Type type, uint64_t bits) noexcept
{
    return type == Type::I32 ? bits & std::numeric_limits<uint32_t>::max() : bits;
}

/// What checkMemory computes: the offset into the memory its guarded address points at; none where it
/// traps, which is where its select leaves 0.
/// @param indexType The index operand's type: I32 for a 32-bit memory, I64 for a 64-bit one.
/// @param indexBits The index register's bits; an I32's high half is ignored.
/// @param offset The access's static offset.
/// @param size The access's width in bytes.
/// @param bound The memory's size in bytes.
/// @return index + offset when index + offset + size, computed without wrapping, is at most bound.
[[nodiscard]] constexpr std::optional<uint64_t> guardMemoryAccess(
    Type indexType, uint64_t indexBits, uint64_t offset, uint64_t size, uint64_t bound) noexcept
{
    auto const index = guardedIndexOf(indexType, indexBits);
    auto const effective = index + offset;
    auto const end = effective + size;
    if (effective < index || end < effective || end > bound)
        return std::nullopt;
    return effective;
}

/// What checkTable computes: the element its guarded address points at; none where it traps, which is
/// where its select leaves 0.
/// @param indexType The index operand's type: I32 for a 32-bit table, I64 for a 64-bit one.
/// @param indexBits The index register's bits; an I32's high half is ignored.
/// @param size The table's size.
/// @return The index when it is below size.
[[nodiscard]] constexpr std::optional<uint64_t> guardTableIndex(Type indexType,
                                                                uint64_t indexBits,
                                                                uint64_t size) noexcept
{
    auto const index = guardedIndexOf(indexType, indexBits);
    if (index >= size)
        return std::nullopt;
    return index;
}

/// What checkDispatch computes: the function address whose row its guarded address points at; none where
/// it traps, which is where its select leaves 0.
/// @param reference The non-null Ref's word: its address is bits 32 to 63.
/// @param rows The store's dispatch rows.
/// @return The address when it is below rows.
[[nodiscard]] constexpr std::optional<uint64_t> guardFunctionAddress(uint64_t reference,
                                                                     uint64_t rows) noexcept
{
    auto const address = reference >> RefWordAddressShift;
    if (address >= rows)
        return std::nullopt;
    return address;
}

/// A memory's declared minimum in bytes: @p pages pages of 2^@p pageSizeLog2 bytes, saturated at 2^64 - 1
/// (a memory64 of 2^48 64 KiB pages would be 2^64 bytes, which no memory reaches). What inBoundsAddress is
/// held to: every size the memory can have reaches it.
[[nodiscard]] constexpr uint64_t minimumBytesOf(uint64_t pages, uint8_t pageSizeLog2) noexcept
{
    if (pageSizeLog2 >= 64 || pages > (std::numeric_limits<uint64_t>::max() >> pageSizeLog2))
        return std::numeric_limits<uint64_t>::max();
    return pages << pageSizeLog2;
}

/// Whether bytes [@p offset, @p offset + @p size) lie within the first @p bytes, computed without wrapping:
/// what the verifier holds an inBoundsAddress to, against its memory's minimum.
[[nodiscard]] constexpr bool liesWithin(uint64_t offset, uint64_t size, uint64_t bytes) noexcept
{
    return offset <= bytes && size <= bytes - offset;
}

/// How far a dominated access lies past the address a dominating check of the same memory and index returned,
/// when the bytes it reaches lie within those the dominating check covered: offset' >= offset and
/// offset' + width' <= offset + width, computed without wrapping. guardAddress moves an address forward only,
/// so an access that starts before the dominating one is not covered, even inside the bytes its check proved.
/// @param checkedOffset The dominating check's static offset.
/// @param checkedWidth Its width.
/// @param accessOffset The dominated check's static offset.
/// @param accessWidth Its width.
/// @return accessOffset - checkedOffset; none when not covered.
[[nodiscard]] constexpr std::optional<uint64_t> guardCovers(uint64_t checkedOffset,
                                                            uint64_t checkedWidth,
                                                            uint64_t accessOffset,
                                                            uint64_t accessWidth) noexcept
{
    if (accessOffset < checkedOffset)
        return std::nullopt;
    auto const delta = accessOffset - checkedOffset;
    if (!liesWithin(delta, accessWidth, checkedWidth))
        return std::nullopt;
    return delta;
}

/// Whether an operation may grow or move a memory: what ends the proof a dominating check gives.
enum class MemoryMotion : uint8_t
{
    None,    ///< Never: no guest or host code runs, no memory changes.
    MayMove, ///< May: it runs code that may grow any memory, and a grow may move it (its base changes).
};

/// One operation whose MemoryMotion its effect does not give.
struct MemoryMotionRow
{
    Operation operation; ///< The operation.
    MemoryMotion motion; ///< Whether it may grow or move a memory.
};

/// The operations whose MemoryMotion is not their effect's. Every Call operation (a call, an import, a stub:
/// memory.grow is one) and every Poll one (a poll's slow path may run the store's observer, host code) may
/// move a memory; of those, storeContext only writes a field of the execution state.
inline constexpr auto MemoryMotionTable = std::to_array<MemoryMotionRow>({
    { .operation = Operation::StoreContext, .motion = MemoryMotion::None },
});

/// Whether @p operation may grow or move a memory.
[[nodiscard]] constexpr MemoryMotion memoryMotionOf(Operation operation) noexcept
{
    for (auto const& row: MemoryMotionTable)
        if (row.operation == operation)
            return row.motion;
    switch (effectOf(operation))
    {
        case Effect::Call:
        case Effect::Poll: return MemoryMotion::MayMove;
        default: return MemoryMotion::None;
    }
}

/// Whether a memory64 access's static part alone overflows: offset + size past 2^64 - 1, which no index
/// fits. Translation makes such an access an unconditional trap after its index is evaluated, never a
/// checkMemory, so a checkMemory's offset + size never wraps by itself.
/// @param offset The access's static offset.
/// @param size Its width in bytes.
[[nodiscard]] constexpr bool staticEndOverflows(uint64_t offset, uint64_t size) noexcept
{
    return offset > std::numeric_limits<uint64_t>::max() - size;
}

} // namespace isoline::compiler::intermediate
