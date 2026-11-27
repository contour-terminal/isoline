// SPDX-License-Identifier: Apache-2.0
#pragma once

// The published bound on a native frame: one row per target (ITarget::frameRule()). A function's frame is
// at most bytesPerStackValue bytes per Wasm stack value it charges (its frame cost) plus a fixed overhead,
// so that the native stack a store needs follows from its limits alone (P5 Decision "Native frames are
// bounded by a published per-target rule"). x86-64's row is {32, 256}: per stack value at most 8 B of
// spill, 8 B of outgoing argument, 8 B of result area and 8 B of slack; the overhead covers the saved
// registers, alignment, the return address, the frame pointer and the three context values.

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <limits>

#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/machine/CallingConvention.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>

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

/// A target's stack rule: the stack pointer's alignment every call leaves with, and what a call pushes.
struct StackRule
{
    uint32_t alignment;          ///< The stack pointer's alignment at every call (x86-64 and arm64: 16).
    uint32_t returnAddressBytes; ///< What a call pushes on the stack (x86-64: 8; arm64, with its link
                                 ///< register: 0).
    /// The bytes a prologue's save of one callee-saved register takes, per RegisterClass: a set names
    /// registers, not widths (ConventionRow::calleeSaved). x86-64: 8 and 16 (Microsoft x64 preserves xmm6–15
    /// whole); arm64: 8 and 8 (AAPCS64 preserves the low 64 bits of v8–v15).
    std::array<uint32_t, RegisterClassCount> savedRegisterBytes;

    /// Whether both rules are alike.
    [[nodiscard]] constexpr bool operator==(StackRule const&) const noexcept = default;
};

/// Whether a frame of @p totalBytes keeps @p rule's size bound for a function of @p frameCost stack values.
/// @param rule The target's rule.
/// @param frameCost The Wasm stack values the function charges.
/// @param totalBytes The frame's size.
/// @return Nothing; or FrameTooLarge, with what the frame needs and what the rule allows, a refusal of
///         the function (an implementation limit, never a Require: guest input reaches it).
[[nodiscard]] constexpr std::expected<void, CompileError> checkFrameSize(FrameRule rule,
                                                                         uint64_t frameCost,
                                                                         uint64_t totalBytes) noexcept
{
    auto const limit = maximumFrameBytes(rule, frameCost);
    if (totalBytes > limit)
        return std::unexpected { CompileError {
            .code = CompileErrorCode::FrameTooLarge, .requested = totalBytes, .limit = limit } };
    return {};
}

/// Whether @p frame keeps the target's rules: its size bound, and the stack at every call.
///
/// Every call leaves from the frame's bottom: calls to Wasm functions, the runtime stubs and the poll's slow
/// path (PollSlow) alike, as no instruction of the body moves the stack pointer. So one alignment covers
/// every call site: the frame's size, what the call into it pushed included, is a multiple of the stack
/// alignment. And every function calls the host convention (its entry poll's slow path), so its outgoing area
/// holds that convention's shadow space, which the callee may write, and the spill slots lie above it.
/// @param rule The target's frame rule.
/// @param stack The target's stack rule.
/// @param host The host convention's row (the stubs').
/// @param frameCost The Wasm stack values the function charges.
/// @param frame The frame.
/// @return Nothing; FrameTooLarge for a frame past the size bound (guest input reaches it); or
///         InternalVerificationFailed (pass "frame") for one that misaligns a call, lacks the shadow space or
///         lays its spills into the outgoing area: an allocator bug refused, never run.
[[nodiscard]] constexpr std::expected<void, CompileError> checkFrame(FrameRule rule,
                                                                     StackRule stack,
                                                                     ConventionRow const& host,
                                                                     uint64_t frameCost,
                                                                     FrameLayout const& frame) noexcept
{
    if (auto const sized = checkFrameSize(rule, frameCost, frame.totalBytes); !sized)
        return sized;
    auto const alignment = std::max(stack.alignment, host.stackAlignment);
    auto const top =
        frame.totalBytes >= stack.returnAddressBytes ? frame.totalBytes - stack.returnAddressBytes : 0;
    auto const aligned = alignment != 0 && frame.totalBytes % alignment == 0;
    auto const shadowed = frame.outgoingBytes >= host.shadowSpaceBytes;
    auto const spillsAbove = frame.spillOffset >= frame.outgoingBytes && frame.spillBytes <= top
                             && frame.spillOffset <= top - frame.spillBytes;
    if (!aligned || !shadowed || !spillsAbove || frame.totalBytes < stack.returnAddressBytes)
        return std::unexpected { CompileError { .code = CompileErrorCode::InternalVerificationFailed,
                                                .pass = "frame" } };
    return {};
}

/// Whether @p frame's outgoing area covers every call site of @p function: each call's stack arguments and
/// result area, as the selector recorded them (MachineFunction::addCallSite()).
/// @return Nothing; or InternalVerificationFailed (pass "frame") for an area too small for a call: an
///         allocator bug refused, never run.
[[nodiscard]] inline std::expected<void, CompileError> checkOutgoingArea(MachineFunction const& function,
                                                                         FrameLayout const& frame) noexcept
{
    for (auto const& site: function.callSites())
        if (site.outgoingBytes > frame.outgoingBytes)
            return std::unexpected { CompileError { .code = CompileErrorCode::InternalVerificationFailed,
                                                    .pass = "frame" } };
    return {};
}

} // namespace isoline::compiler::machine
