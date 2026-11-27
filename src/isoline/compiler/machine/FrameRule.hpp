// SPDX-License-Identifier: Apache-2.0
#pragma once

// The published bound on a native frame: one row per target (ITarget::frameRule()). A function's frame is
// at most bytesPerStackValue bytes per Wasm stack value it charges (its frame cost) plus a fixed overhead,
// so that the native stack a store needs follows from its limits alone (P5 Decision "Native frames are
// bounded by a published per-target rule"). x86-64's row is {32, 256}: per stack value at most 8 B of
// spill, 8 B of outgoing argument, 8 B of result area and 8 B of slack; the overhead covers the saved
// registers, alignment, the return address, the frame pointer and the three context values.

#include <cstdint>
#include <expected>
#include <limits>

#include <isoline/compiler/CompileError.hpp>

namespace isoline::compiler::machine
{

/// A target's bound on a native frame.
struct FrameRule
{
    uint32_t bytesPerStackValue = 0; ///< The most frame bytes per Wasm stack value a function charges.
    uint32_t frameOverhead = 0;      ///< The frame bytes every function may take beyond those.

    /// Whether both rules bound frames alike.
    [[nodiscard]] constexpr bool operator==(FrameRule const&) const noexcept = default;
};

/// The most bytes a frame of @p frameCost stack values may take under @p rule:
/// bytesPerStackValue × frameCost + frameOverhead, saturated at the largest uint64_t.
/// @param rule The target's rule.
/// @param frameCost The Wasm stack values the function charges.
/// @return The bound.
[[nodiscard]] constexpr uint64_t maximumFrameBytes(FrameRule rule, uint64_t frameCost) noexcept
{
    constexpr auto Largest = std::numeric_limits<uint64_t>::max();
    if (rule.bytesPerStackValue != 0 && frameCost > (Largest - rule.frameOverhead) / rule.bytesPerStackValue)
        return Largest;
    return (rule.bytesPerStackValue * frameCost) + rule.frameOverhead;
}

/// Whether a frame of @p totalBytes keeps @p rule for a function of @p frameCost stack values.
/// @param rule The target's rule.
/// @param frameCost The Wasm stack values the function charges.
/// @param totalBytes The frame's size.
/// @return Nothing; or FrameTooLarge, with what the frame needs and what the rule allows, a refusal of
///         the function (an implementation limit, never a Require: guest input reaches it).
[[nodiscard]] constexpr std::expected<void, CompileError> checkFrame(FrameRule rule,
                                                                     uint64_t frameCost,
                                                                     uint64_t totalBytes) noexcept
{
    auto const limit = maximumFrameBytes(rule, frameCost);
    if (totalBytes > limit)
        return std::unexpected { CompileError {
            .code = CompileErrorCode::FrameTooLarge, .requested = totalBytes, .limit = limit } };
    return {};
}

} // namespace isoline::compiler::machine
