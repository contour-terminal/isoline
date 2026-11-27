// SPDX-License-Identifier: Apache-2.0
#pragma once

// The intermediate representation's verifier: whether a function is well-formed SSA. It reads nothing it
// has not first checked to lie within the function, so it may be given any function a pass or a hostile
// test built, and it walks with explicit worklists only.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <isoline/compiler/intermediate/Function.hpp>

namespace isoline::compiler::intermediate
{

/// Why a function is not well-formed: the rows of VerifierErrorTable, by the phase verify() checks them in:
/// the structure of each block, then each instruction against its row, then dominance.
enum class VerifierErrorCode : uint8_t
{
    NoBlocks,               ///< The function has no entry block.
    UnknownType,            ///< A value's type is no Type enumerator.
    RangeOutOfBounds,       ///< A block's or instruction's range leaves its pool.
    UnknownHotness,         ///< A block's hotness is no Hotness enumerator.
    InstructionOutOfBounds, ///< A block lists an instruction index past the function's.
    InstructionListedTwice, ///< An instruction is listed twice, in one block or two.
    UnknownOperation,       ///< An instruction's operation is no Operations.def row.
    UndefinedValue,         ///< A value is used, or defined, that the function does not define.
    RedefinedValue,         ///< A value is defined twice.
    MissingTerminator,      ///< A block does not end with a terminator.
    TerminatorNotLast,      ///< A terminator is followed by another instruction.
    ImmediateCount,         ///< An instruction's immediates do not fit its row's schema.
    ImmediateValue,         ///< An immediate lies outside its field's values.
    TargetOutOfBounds,      ///< A branch target is past the function's blocks.
    TargetIsEntry,          ///< A branch targets the entry block, whose parameters are the function's.
    OperandCount,           ///< An instruction's operands do not number what its row and targets say.
    OperandType,            ///< An operand's type is not its row's.
    ResultCount,            ///< An instruction's results do not number what its row says.
    ResultType,             ///< A result's type, or the instruction's type, is not its row's.
    ArgumentCount,          ///< A target is passed more or fewer arguments than it has parameters.
    ArgumentType,           ///< An argument's type is not its parameter's.
    ReturnsDisagree,        ///< Two returns differ in their values' count or types.
    UseNotDominated,        ///< A value is used where its definition does not dominate the use.
    UnguardedAddress,       ///< A pointer is used that its GuardedUseTable row's guards did not define.
};

/// The last VerifierErrorCode enumerator.
inline constexpr auto LastVerifierErrorCode = VerifierErrorCode::UnguardedAddress;

/// How many VerifierErrorCode enumerators there are.
inline constexpr auto VerifierErrorCodeCount = std::size_t { std::to_underlying(LastVerifierErrorCode) } + 1;

/// One VerifierErrorCode: what describe() says of it.
struct VerifierErrorRow
{
    VerifierErrorCode code; ///< The code; equal to the row's index.
    std::string_view text;  ///< What describe() says of it.
};

/// Every VerifierErrorCode's row, in enumerator order.
inline constexpr auto VerifierErrorTable = std::to_array<VerifierErrorRow>({
    { .code = VerifierErrorCode::NoBlocks, .text = "function has no blocks" },
    { .code = VerifierErrorCode::UnknownType, .text = "value of unknown type" },
    { .code = VerifierErrorCode::RangeOutOfBounds, .text = "range out of its pool" },
    { .code = VerifierErrorCode::UnknownHotness, .text = "block of unknown hotness" },
    { .code = VerifierErrorCode::InstructionOutOfBounds, .text = "instruction index out of bounds" },
    { .code = VerifierErrorCode::InstructionListedTwice, .text = "instruction listed twice" },
    { .code = VerifierErrorCode::UnknownOperation, .text = "unknown operation" },
    { .code = VerifierErrorCode::UndefinedValue, .text = "undefined value" },
    { .code = VerifierErrorCode::RedefinedValue, .text = "value defined twice" },
    { .code = VerifierErrorCode::MissingTerminator, .text = "block does not end with a terminator" },
    { .code = VerifierErrorCode::TerminatorNotLast, .text = "terminator before the end of its block" },
    { .code = VerifierErrorCode::ImmediateCount, .text = "immediate count mismatch" },
    { .code = VerifierErrorCode::ImmediateValue, .text = "immediate out of range" },
    { .code = VerifierErrorCode::TargetOutOfBounds, .text = "branch target out of bounds" },
    { .code = VerifierErrorCode::TargetIsEntry, .text = "branch to the entry block" },
    { .code = VerifierErrorCode::OperandCount, .text = "operand count mismatch" },
    { .code = VerifierErrorCode::OperandType, .text = "operand type mismatch" },
    { .code = VerifierErrorCode::ResultCount, .text = "result count mismatch" },
    { .code = VerifierErrorCode::ResultType, .text = "result type mismatch" },
    { .code = VerifierErrorCode::ArgumentCount, .text = "branch argument count mismatch" },
    { .code = VerifierErrorCode::ArgumentType, .text = "branch argument type mismatch" },
    { .code = VerifierErrorCode::ReturnsDisagree, .text = "returns disagree" },
    { .code = VerifierErrorCode::UseNotDominated, .text = "use not dominated by its definition" },
    { .code = VerifierErrorCode::UnguardedAddress, .text = "address not defined by its guard" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: VerifierErrorTable)
            if (std::to_underlying(row.code) != index++)
                return false;
        return index == VerifierErrorCodeCount;
    }(),
    "VerifierErrorTable must hold one row per VerifierErrorCode, at the index of its enumerator");

/// Why a function is not well-formed, and where.
struct VerifierError
{
    VerifierErrorCode code = VerifierErrorCode::NoBlocks; ///< Why.
    std::optional<BlockId> block {};        ///< The block it is in; none for the function as a whole.
    std::optional<uint32_t> instruction {}; ///< The instruction's index; none for a block's own fault.

    /// Whether both describe the same fault at the same place.
    [[nodiscard]] bool operator==(VerifierError const&) const = default;
};

/// A one-line description of @p error: its row's text, then the block and instruction it names.
[[nodiscard]] std::string describe(VerifierError const& error);

/// Whether @p function is well-formed: every range within its pool and every index within the function;
/// each value defined once and every definition dominating its uses (a block parameter at its block's
/// entry, a result just after its instruction); each block of a known hotness and ending in one
/// terminator; each instruction's operands, results and immediates as many and of the types and values
/// its row says, a `type` immediate within its schema's types and a `field` immediate's ContextValue
/// typed as its row says, an access's `size` fitting its type (at most as wide, a float exactly as wide,
/// a sign extension only when narrower); each target a block other than the entry, passed as many
/// arguments as it has parameters, of their types; every return of the same values' types. Removed
/// instructions are skipped.
/// @param function The function, however it was built.
/// @return Nothing; or the first fault: every block's structure is checked before any instruction's row,
///         every row before the returns' agreement, and that before dominance.
[[nodiscard]] std::expected<void, VerifierError> verify(Function const& function);

} // namespace isoline::compiler::intermediate
