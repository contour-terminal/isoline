// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace isoline::binary
{

/// The implementation limits the decoder and the validator enforce (spec §9, Robustness), published so
/// embedders know them; exceeding one is ImplementationLimitExceeded, or TooManyLocals for maxLocals,
/// or the validator's OperandStackLimitExceeded for maxOperandsPerInstruction. Each of those codes
/// answers isImplementationLimit(), so an embedder tells "over a limit" from "malformed" or "invalid".
///
/// Every default but maxOperandsPerInstruction is one the JavaScript embedding fixes, which every
/// browser engine shares; maxOperandsPerInstruction is Isoline's own, as the JS embedding has none.
struct DecodeLimits
{
    uint32_t maxTypes = 1'000'000;            ///< Entries of the type section.
    uint32_t maxFunctions = 1'000'000;        ///< Entries of the function section.
    uint32_t maxImports = 100'000;            ///< Imports, each item of a compact entry one.
    uint32_t maxExports = 100'000;            ///< Entries of the export section.
    uint32_t maxGlobals = 1'000'000;          ///< Entries of the global section.
    uint32_t maxTables = 100'000;             ///< Tables, imported and defined alike, as JS allows.
    uint32_t maxMemories = 100;               ///< Memories, imported and defined alike.
    uint32_t maxTags = 1'000'000;             ///< Entries of the tag section.
    uint32_t maxDataSegments = 100'000;       ///< Entries of the data section.
    uint32_t maxElementSegments = 10'000'000; ///< Entries of the element section.
    uint32_t maxElementItems = 10'000'000;    ///< Items of one element segment.
    /// Declared locals of one function, over all its runs. Past 2^32 - 1 a count is malformed
    /// (LocalCountOverflow) whatever this limit, so it can be raised to the most a u32 holds.
    uint32_t maxLocals = 50'000;
    uint32_t maxParams = 1'000;               ///< Parameters of one function type.
    uint32_t maxResults = 1'000;              ///< Results of one function type.
    uint32_t maxFunctionBodySize = 7'654'321; ///< Bytes of one function body, its locals included.
    uint32_t maxBrTableSize = 65'520;         ///< Labels of one br_table.
    /// Catch clauses of one try_table: as many as br_table may name labels, as each clause names one.
    uint32_t maxCatchClauses = 65'520;
    /// Operands one function's operand stack may hold at once, per instruction of its body, beyond the
    /// max(maxParams, maxResults) one instruction may push: a taller stack is refused
    /// (OperandStackLimitExceeded) at the instruction that grows it, so validating a body holds memory
    /// linear in its size. What counts is the stack's peak, so results consumed later count too:
    /// `call $f; call $f; call $sink; call $sink`, of two 1000-result producers and two 1000-param
    /// consumers, peaks at 2000 operands in 5 instructions, past the cap of 4 x 5 + 1000 = 1020, and
    /// is refused although valid. Only bodies that pile up many multi-value results before consuming
    /// them approach the cap; compiled code stays far below it.
    uint32_t maxOperandsPerInstruction = 4;
};

} // namespace isoline::binary
