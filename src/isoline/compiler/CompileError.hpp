// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <isoline/Features.hpp>
#include <isoline/compiler/intermediate/Verifier.hpp>

namespace isoline::compiler
{

/// Why a compile failed.
enum class CompileErrorCode : uint8_t
{
    UnsupportedFeature,     ///< The module uses a feature the compiler defers (CompilerFeatureTable).
    FunctionTooLarge,       ///< A body has more units than CompileLimits::maxFunctionBodyUnits.
    TooManyInstructions,    ///< Translation exceeded CompileLimits::maxFunctionInstructions.
    TooManyBlockParameters, ///< A block or function exceeded the block-parameter caps.
    PoolTooLarge, ///< A function's operand or immediate pool exceeded CompileLimits::maxPoolEntries.
    TooManyBranchArguments, ///< A function's branches pass more than CompileLimits::maxBranchArguments.
    BudgetExceeded,         ///< The compile's arena refused: LoadLimits::maxCompileBytes is spent.
    HostMemoryExhausted,    ///< The compile's arena refused: the host is out of memory.
    FrameTooLarge,          ///< A native frame exceeds the target's frame rule.
    ArtifactTooLarge,       ///< The module's code exceeds its artifact cap (CompileLimits).
    BranchOutOfRange,       ///< A branch's displacement does not fit its encoding.
    LiveSetTooLarge, ///< Liveness would take more than CompileLimits::maxLiveSetBits (values × blocks).
    Cancelled,       ///< The compile was cancelled (CompileCancellation).
    /// A target was asked for code it does not generate: another architecture, or an ABI without its host
    /// convention (X64Target::create()).
    UnsupportedTarget,
    /// A pass left a function ill-formed (intermediate::verify()): a compiler bug, caught before any of the
    /// function's code is generated, so the module is refused rather than miscompiled.
    InternalVerificationFailed,
};

/// The last CompileErrorCode enumerator.
inline constexpr auto LastCompileErrorCode = CompileErrorCode::InternalVerificationFailed;

/// How many CompileErrorCode enumerators there are.
inline constexpr auto CompileErrorCodeCount = static_cast<std::size_t>(LastCompileErrorCode) + 1;

/// Whether a refusal is an implementation limit: valid input the compiler cannot take, which another
/// engine (or a larger limit) runs, as opposed to the module's use of a deferred feature, a spent
/// budget or a cancellation. The api reports it through isImplementationLimit().
enum class ImplementationLimit : uint8_t
{
    No,  ///< Not a limit of the compiler's.
    Yes, ///< A limit of the compiler's implementation.
};

/// What a CompileErrorCode means.
struct CompileErrorRow
{
    CompileErrorCode code;     ///< The code; equal to the row's index.
    std::string_view text;     ///< What describe() says of it.
    ImplementationLimit limit; ///< Whether it is an implementation limit.
};

/// Every CompileErrorCode's row, in enumerator order.
inline constexpr auto CompileErrorTable = std::to_array<CompileErrorRow>({
    { .code = CompileErrorCode::UnsupportedFeature,
      .text = "feature not supported by the compiler",
      .limit = ImplementationLimit::No },
    { .code = CompileErrorCode::FunctionTooLarge,
      .text = "function too large",
      .limit = ImplementationLimit::Yes },
    { .code = CompileErrorCode::TooManyInstructions,
      .text = "too many instructions",
      .limit = ImplementationLimit::Yes },
    { .code = CompileErrorCode::TooManyBlockParameters,
      .text = "too many block parameters",
      .limit = ImplementationLimit::Yes },
    { .code = CompileErrorCode::PoolTooLarge,
      .text = "operand pool too large",
      .limit = ImplementationLimit::Yes },
    { .code = CompileErrorCode::TooManyBranchArguments,
      .text = "too many branch arguments",
      .limit = ImplementationLimit::Yes },
    { .code = CompileErrorCode::BudgetExceeded,
      .text = "compile memory budget exceeded",
      .limit = ImplementationLimit::No },
    { .code = CompileErrorCode::HostMemoryExhausted,
      .text = "host memory exhausted",
      .limit = ImplementationLimit::No },
    { .code = CompileErrorCode::FrameTooLarge, .text = "frame too large", .limit = ImplementationLimit::Yes },
    { .code = CompileErrorCode::ArtifactTooLarge,
      .text = "compiled code too large",
      .limit = ImplementationLimit::Yes },
    { .code = CompileErrorCode::BranchOutOfRange,
      .text = "branch out of range",
      .limit = ImplementationLimit::Yes },
    { .code = CompileErrorCode::LiveSetTooLarge,
      .text = "too many values live across blocks",
      .limit = ImplementationLimit::Yes },
    { .code = CompileErrorCode::Cancelled, .text = "compile cancelled", .limit = ImplementationLimit::No },
    { .code = CompileErrorCode::UnsupportedTarget,
      .text = "target not supported by this backend",
      .limit = ImplementationLimit::No },
    { .code = CompileErrorCode::InternalVerificationFailed,
      .text = "internal compiler error: ill-formed intermediate code",
      .limit = ImplementationLimit::No },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: CompileErrorTable)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return index == CompileErrorCodeCount;
    }(),
    "CompileErrorTable must hold one row per CompileErrorCode, at the index of its enumerator");

/// The row of @p code.
/// @param code The code.
/// @return Its row of CompileErrorTable.
[[nodiscard]] constexpr CompileErrorRow const& compileErrorRow(CompileErrorCode code) noexcept
{
    return CompileErrorTable[static_cast<std::size_t>(code)];
}

/// Why a compile failed, and where.
struct CompileError
{
    CompileErrorCode code = CompileErrorCode::UnsupportedFeature; ///< Why.
    std::optional<Feature> feature {}; ///< UnsupportedFeature: the feature the module uses.
    /// The function it failed in, by its index in the code section (imports not counted); none for a
    /// failure outside any body (a type, a table, a global, a segment, the module's artifact).
    std::optional<uint32_t> function {};
    uint64_t requested = 0; ///< For a cap: what the input needed of it; 0 otherwise.
    uint64_t limit = 0;     ///< For a cap: what it allows; 0 otherwise.
    /// InternalVerificationFailed: the pass after which the function failed to verify (its PassTable name,
    /// static text), or "end" when only the pipeline's final verification ran; empty otherwise.
    std::string_view pass {};
    /// InternalVerificationFailed: the verifier's rule the function broke; none otherwise.
    std::optional<intermediate::VerifierErrorCode> rule {};

    /// Whether both describe the same failure.
    [[nodiscard]] bool operator==(CompileError const&) const = default;
};

/// A one-line description of @p error: its row's text, then the feature, the function, what was requested of
/// which limit, and the pass and the verifier's rule, each where the error names it.
/// @param error The error.
/// @return The description.
[[nodiscard]] std::string describe(CompileError const& error);

} // namespace isoline::compiler
