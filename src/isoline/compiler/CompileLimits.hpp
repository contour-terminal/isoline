// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

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

    /// The most block parameters a function may take over all its blocks, the inner loops' merges
    /// into their parents counted, refused TooManyBlockParameters.
    uint64_t maxTotalBlockParameters = 4'000'000;

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
