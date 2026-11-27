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
};

/// Every operation that returns a guarded Pointer, with its contract. Under SpectreGuard::Enabled each
/// selects 0 exactly where it traps (or, for guardAddress, where its operand is 0).
inline constexpr auto GuardTable = std::to_array<GuardRow>({
    { .operation = Operation::CheckMemory,
      .indexOperand = 0,
      .bound = GuardBound::MemoryBytes,
      .overflow = EndOverflow::Traps,
      .subject = GuardSubject::None },
    { .operation = Operation::GuardAddress,
      .indexOperand = 0,
      .bound = GuardBound::None,
      .overflow = EndOverflow::Impossible,
      .subject = GuardSubject::None },
    { .operation = Operation::CheckTable,
      .indexOperand = 0,
      .bound = GuardBound::TableSize,
      .overflow = EndOverflow::Impossible,
      .subject = GuardSubject::GuardedIndex },
    { .operation = Operation::CheckDispatch,
      .indexOperand = 0,
      .bound = GuardBound::DispatchRows,
      .overflow = EndOverflow::Impossible,
      .subject = GuardSubject::SecondOperand },
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
            auto const isGuard =
                row.result == ResultRule::Pointer
                && (row.effect == Effect::MayTrap || row.operation == Operation::GuardAddress);
            if (isGuard != guardRowOf(row.operation).has_value())
                return false;
        }
        return true;
    }(),
    "every operation that returns a guarded Pointer has a GuardTable row, and only those");

/// Which guards may produce the pointer one operand of an operation uses.
struct GuardedUseRow
{
    Operation operation;             ///< The operation that uses a pointer.
    uint8_t operand;                 ///< Which of its operands the pointer is.
    std::array<Operation, 2> guards; ///< The operations that may define it; a repeated one fills the pair.
};

/// Every use of a guarded pointer, with the guards its pointer must come from: what the verifier checks
/// (VerifierErrorCode::UnguardedAddress), so that no pass or translation feeds an access an address that
/// skipped its select or took an offset after it.
inline constexpr auto GuardedUseTable = std::to_array<GuardedUseRow>({
    { .operation = Operation::Load,
      .operand = 0,
      .guards = { Operation::CheckMemory, Operation::GuardAddress } },
    { .operation = Operation::Store,
      .operand = 0,
      .guards = { Operation::CheckMemory, Operation::GuardAddress } },
    { .operation = Operation::GuardAddress,
      .operand = 0,
      .guards = { Operation::CheckMemory, Operation::GuardAddress } },
    { .operation = Operation::LoadTableElement,
      .operand = 0,
      .guards = { Operation::CheckTable, Operation::CheckTable } },
    { .operation = Operation::StoreTableElement,
      .operand = 0,
      .guards = { Operation::CheckTable, Operation::CheckTable } },
    { .operation = Operation::LoadDispatchType,
      .operand = 0,
      .guards = { Operation::CheckDispatch, Operation::CheckDispatch } },
    { .operation = Operation::CallIndirect,
      .operand = 0,
      .guards = { Operation::CheckDispatch, Operation::CheckDispatch } },
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
            for (auto const guard: row.guards)
                if (!guardRowOf(guard).has_value())
                    return false;
        return true;
    }(),
    "every guard GuardedUseTable names has a GuardTable row");

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
