// SPDX-License-Identifier: Apache-2.0
#pragma once

// What translation needs to know of the memory, table and bulk instructions, as data: per load and
// store what it moves, and per instruction that runs as a runtime stub which stub it calls with what.
// Translator.cpp interprets these rows; the guards they translate to are intermediate/Guards.hpp's.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>

#include <isoline/binary/Opcode.hpp>
#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>

namespace isoline::compiler::translation
{

/// Whether a memory access reads or writes.
enum class AccessKind : uint8_t
{
    Load,  ///< Reads: load of the guarded address.
    Store, ///< Writes: store to it.
};

/// One load or store instruction: what it moves.
struct MemoryAccessRow
{
    binary::Opcode opcode;             ///< The instruction.
    AccessKind kind;                   ///< Whether it loads or stores.
    intermediate::Type type;           ///< The value's type: a load's result, a store's operand.
    uint8_t size;                      ///< How many bytes it moves: 1, 2, 4 or 8.
    intermediate::Extension extension; ///< How a narrower load widens; Zero for every store.
};

namespace detail
{
    using enum binary::Opcode;
    using intermediate::Extension;
    using intermediate::Type;
} // namespace detail

/// Every scalar load and store, in Opcode order, from i32.load to i64.store32.
inline constexpr auto MemoryAccessTable = std::to_array<MemoryAccessRow>({
    { detail::I32Load, AccessKind::Load, detail::Type::I32, 4, detail::Extension::Zero },
    { detail::I64Load, AccessKind::Load, detail::Type::I64, 8, detail::Extension::Zero },
    { detail::F32Load, AccessKind::Load, detail::Type::F32, 4, detail::Extension::Zero },
    { detail::F64Load, AccessKind::Load, detail::Type::F64, 8, detail::Extension::Zero },
    { detail::I32Load8S, AccessKind::Load, detail::Type::I32, 1, detail::Extension::Sign },
    { detail::I32Load8U, AccessKind::Load, detail::Type::I32, 1, detail::Extension::Zero },
    { detail::I32Load16S, AccessKind::Load, detail::Type::I32, 2, detail::Extension::Sign },
    { detail::I32Load16U, AccessKind::Load, detail::Type::I32, 2, detail::Extension::Zero },
    { detail::I64Load8S, AccessKind::Load, detail::Type::I64, 1, detail::Extension::Sign },
    { detail::I64Load8U, AccessKind::Load, detail::Type::I64, 1, detail::Extension::Zero },
    { detail::I64Load16S, AccessKind::Load, detail::Type::I64, 2, detail::Extension::Sign },
    { detail::I64Load16U, AccessKind::Load, detail::Type::I64, 2, detail::Extension::Zero },
    { detail::I64Load32S, AccessKind::Load, detail::Type::I64, 4, detail::Extension::Sign },
    { detail::I64Load32U, AccessKind::Load, detail::Type::I64, 4, detail::Extension::Zero },
    { detail::I32Store, AccessKind::Store, detail::Type::I32, 4, detail::Extension::Zero },
    { detail::I64Store, AccessKind::Store, detail::Type::I64, 8, detail::Extension::Zero },
    { detail::F32Store, AccessKind::Store, detail::Type::F32, 4, detail::Extension::Zero },
    { detail::F64Store, AccessKind::Store, detail::Type::F64, 8, detail::Extension::Zero },
    { detail::I32Store8, AccessKind::Store, detail::Type::I32, 1, detail::Extension::Zero },
    { detail::I32Store16, AccessKind::Store, detail::Type::I32, 2, detail::Extension::Zero },
    { detail::I64Store8, AccessKind::Store, detail::Type::I64, 1, detail::Extension::Zero },
    { detail::I64Store16, AccessKind::Store, detail::Type::I64, 2, detail::Extension::Zero },
    { detail::I64Store32, AccessKind::Store, detail::Type::I64, 4, detail::Extension::Zero },
});

static_assert(
    [] {
        auto expected = std::to_underlying(binary::Opcode::I32Load);
        for (auto const& row: MemoryAccessTable)
            if (std::to_underlying(row.opcode) != expected++)
                return false;
        return MemoryAccessTable.back().opcode == binary::Opcode::I64Store32;
    }(),
    "MemoryAccessTable must hold i32.load to i64.store32, one row per opcode, in Opcode order");

/// Which of an instruction's index immediates a stub takes first, each as an I32 constant.
enum class StubIndices : uint8_t
{
    None,  ///< None.
    First, ///< Its first (binary::Instruction::a): the memory, table or segment.
    Both,  ///< Its first, then its second (b): the destination then the source, the segment then the target.
};

/// How many index immediates @p indices names.
[[nodiscard]] constexpr std::size_t indexCountOf(StubIndices indices) noexcept
{
    switch (indices)
    {
        case StubIndices::None: return 0;
        case StubIndices::First: return 1;
        case StubIndices::Both: return 2;
    }
    return 0; // uncovered(stub-indices-without-case)
}

/// What a stub's WebAssembly result is.
enum class StubResult : uint8_t
{
    None,          ///< Nothing.
    MemoryAddress, ///< An I64 in the address type of the memory its first index names: narrowed for 32 bits.
    TableAddress,  ///< An I64 in the address type of the table its first index names: narrowed for 32 bits.
    Reference,     ///< A Ref.
};

/// Whether a stub row applies to every table or only to an imported one.
enum class StubTables : uint8_t
{
    Every,    ///< Every instruction of its opcode is a stub call.
    Imported, ///< Only one on an imported table: a defined table's is inline.
};

/// One instruction that runs as a runtime stub.
struct StubCallRow
{
    binary::Opcode opcode; ///< The instruction.
    RuntimeStub stub;      ///< The stub it calls.
    StubIndices indices;   ///< Which index immediates the stub takes first.
    uint8_t operands;      ///< How many WebAssembly operands it pops, passed after the indices.
    StubResult result;     ///< What it pushes.
    StubTables tables;     ///< Which of its instructions call the stub.
};

/// Every instruction that runs as a runtime stub, in Opcode order. The bulk operations, the grows and
/// the drops always do; table.get and table.set only on an imported table.
inline constexpr auto StubCallTable = std::to_array<StubCallRow>({
    { detail::TableGet,
      RuntimeStub::TableGet,
      StubIndices::First,
      1,
      StubResult::Reference,
      StubTables::Imported },
    { detail::TableSet,
      RuntimeStub::TableSet,
      StubIndices::First,
      2,
      StubResult::None,
      StubTables::Imported },
    { detail::MemoryGrow,
      RuntimeStub::MemoryGrow,
      StubIndices::First,
      1,
      StubResult::MemoryAddress,
      StubTables::Every },
    { detail::MemoryInit,
      RuntimeStub::MemoryInit,
      StubIndices::Both,
      3,
      StubResult::None,
      StubTables::Every },
    { detail::DataDrop, RuntimeStub::DataDrop, StubIndices::First, 0, StubResult::None, StubTables::Every },
    { detail::MemoryCopy,
      RuntimeStub::MemoryCopy,
      StubIndices::Both,
      3,
      StubResult::None,
      StubTables::Every },
    { detail::MemoryFill,
      RuntimeStub::MemoryFill,
      StubIndices::First,
      3,
      StubResult::None,
      StubTables::Every },
    { detail::TableInit, RuntimeStub::TableInit, StubIndices::Both, 3, StubResult::None, StubTables::Every },
    { detail::ElemDrop,
      RuntimeStub::ElementDrop,
      StubIndices::First,
      0,
      StubResult::None,
      StubTables::Every },
    { detail::TableCopy, RuntimeStub::TableCopy, StubIndices::Both, 3, StubResult::None, StubTables::Every },
    { detail::TableGrow,
      RuntimeStub::TableGrow,
      StubIndices::First,
      2,
      StubResult::TableAddress,
      StubTables::Every },
    { detail::TableFill, RuntimeStub::TableFill, StubIndices::First, 3, StubResult::None, StubTables::Every },
});

static_assert(
    [] {
        for (auto const index: std::views::iota(std::size_t { 1 }, StubCallTable.size()))
            if (std::to_underlying(StubCallTable[index - 1].opcode)
                >= std::to_underlying(StubCallTable[index].opcode))
                return false;
        return true;
    }(),
    "StubCallTable must hold one row per opcode, in Opcode order");

/// The most WebAssembly operands a stub takes (StubCallRow::operands).
inline constexpr auto MaxStubOperands = std::size_t { 3 };
/// The most index immediates a stub takes (StubIndices::Both).
inline constexpr auto MaxStubIndices = std::size_t { 2 };

/// The row of @p opcode in MemoryAccessTable.
/// @return It; none for every instruction but a scalar load or store.
[[nodiscard]] std::optional<MemoryAccessRow> memoryAccessOf(binary::Opcode opcode) noexcept;

/// The row of @p opcode in StubCallTable.
/// @return It; none for an instruction that never runs as a stub.
[[nodiscard]] std::optional<StubCallRow> stubCallOf(binary::Opcode opcode) noexcept;

} // namespace isoline::compiler::translation
