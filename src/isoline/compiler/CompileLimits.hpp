// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <limits>

namespace isoline::compiler
{

/// The compiler's caps on one module's input: what a hostile module cannot push it past, each refused
/// with a named CompileError rather than paid for in memory or time. Per function unless stated; fixed
/// at construction (CompileConfig), and part of an artifact's key (P8). Memory is not capped here: the
/// whole compile draws on the load's LoadLimits::maxCompileBytes, one budget for every engine.
struct CompileLimits
{
    /// The most decoded body units a function may have (the fast interpreter's bodyUnits(): the
    /// instructions and their side-table entries), refused FunctionTooLarge. A unit takes at least one
    /// byte of input, so the default is the JS API's 7'654'321-byte body limit, or tighter.
    uint64_t maxFunctionBodyUnits = 7'654'321;

    /// The most IR instructions translation may produce for a function, refused TooManyInstructions:
    /// the compiled engine's own implementation limit.
    uint64_t maxFunctionInstructions = 4'000'000;

    /// The most parameters one block may take (a loop's assigned locals, a merge's values), refused
    /// TooManyBlockParameters.
    uint32_t maxBlockParameters = 100'000;

    /// The most loop-header parameters a function may take over all its loops (assignedLocalsPerLoop():
    /// each local a loop assigns, an inner loop's counted in every loop around it), refused
    /// TooManyBlockParameters.
    uint64_t maxTotalBlockParameters = 4'000'000;
    /// The most branch arguments a function may pass over all its branches, refused
    /// TooManyBranchArguments: each branch's results and locals, a loop's back edge its assigned
    /// locals, a forward edge the locals it modified, and the arguments a merge settles each edge to.
    /// The context values a branch also passes, at most three, are not counted: every branch is an IR
    /// instruction, so maxFunctionInstructions bounds them.
    uint64_t maxBranchArguments = 16'000'000;
    /// The most entries a function's IR operand pool, or its immediate pool, may hold (a call's
    /// arguments, a branch's, a br_table's targets), refused PoolTooLarge. The default is what a 32-bit
    /// pool index addresses (intermediate::MaxPoolEntries), and nothing larger is honoured.
    uint64_t maxPoolEntries = std::numeric_limits<uint32_t>::max();

    /// The most live-set bits register allocation's liveness may take for a function, refused
    /// LiveSetTooLarge before any of them is built: one bit per block for each value live across a block
    /// boundary (machine::computeLiveIntervals()). Liveness is the one analysis whose size is the product of
    /// two input dimensions, values × blocks, so it takes a cap of its own: 2^31 bits is 256 MiB of sets,
    /// which no function of a module within the default caps that is not built to reach it comes near.
    uint64_t maxLiveSetBits = uint64_t { 1 } << 31U;
    /// The artifact's cap per byte of the module (ValidatedModule's source bytes), checked after each
    /// function, refused ArtifactTooLarge: the module's code may not exceed
    /// max(artifactBytesFloor, artifactBytesPerModuleByte × source bytes).
    uint32_t artifactBytesPerModuleByte = 4;

    /// The artifact's cap for a small module, below which artifactBytesPerModuleByte does not reach.
    uint64_t artifactBytesFloor = uint64_t { 64 } * 1024;

    /// Whether both hold the same caps.
    [[nodiscard]] bool operator==(CompileLimits const&) const = default;
};

} // namespace isoline::compiler
