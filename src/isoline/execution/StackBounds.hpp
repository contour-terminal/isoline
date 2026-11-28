// SPDX-License-Identifier: Apache-2.0
#pragma once

// The native stack compiled code runs on (P5 Decision "Native stack limit from the real stack"): where the
// calling thread's stack ends, injected as IStackBounds so that a test can stand in another, and the
// published arithmetic that turns a store's limits into the stack its invocations need and the limit every
// prologue checks. The platform's half of ThreadStackBounds is CMake-selected (StackBoundsPosix.cpp,
// StackBoundsMacOS.cpp, StackBoundsWindows.cpp).

#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>

#include <isoline/compiler/machine/FrameRule.hpp>
#include <isoline/runtime/Limits.hpp>

namespace isoline::execution
{

/// Why the calling thread's stack bounds are not known.
enum class StackBoundsError : uint8_t
{
    Unavailable, ///< The system did not report them (a sandbox without /proc, a failing pthread query).
};

/// Where the calling thread's stack ends: the DI seam for the platform's thread stack.
class IStackBounds
{
  public:
    virtual ~IStackBounds() = default;

    /// The lowest address the calling thread's stack may use. Without it no native stack limit can be set, so
    /// an entry into compiled code must be refused (with the executor's error, never an abort).
    /// @return The address; or Unavailable when the system does not tell.
    [[nodiscard]] virtual std::expected<std::uintptr_t, StackBoundsError> lowestUsableAddress()
        const noexcept = 0;

  protected:
    IStackBounds() = default;
    IStackBounds(IStackBounds const&) = default;
    IStackBounds(IStackBounds&&) = default;
    IStackBounds& operator=(IStackBounds const&) = default;
    IStackBounds& operator=(IStackBounds&&) = default;
};

/// The calling thread's own stack, as the system reports it: pthread_getattr_np on Linux, pthread_attr_get_np
/// on FreeBSD, pthread_get_stackaddr_np and pthread_get_stacksize_np on macOS, GetCurrentThreadStackLimits on
/// Windows. Each keeps the guard below the stack out of it. Stateless: it asks for the thread that calls it.
class ThreadStackBounds final: public IStackBounds
{
  public:
    [[nodiscard]] std::expected<std::uintptr_t, StackBoundsError> lowestUsableAddress()
        const noexcept override;
};

/// An address in the frame of this out-of-line function, on the real host stack: where the caller's stack is,
/// to within the caller's own frame (a local's address would not do: a sanitizer may move locals to a stack
/// of its own). What an entry into compiled code measures its room above the native stack limit from. The
/// compiler's half is CMake-selected (StackAddressGnu.cpp, StackAddressMsvc.cpp).
/// @return The address.
[[nodiscard]] std::uintptr_t currentStackAddress() noexcept;

/// The stack every prologue keeps free below its frame for a callee's entry and the deepest runtime stub that
/// is not a host call (published; StackBounds.stubReserveCoversEveryStub measures every stub against
/// compiler::machine::StubStackBudgetBytes, within it).
inline constexpr uint64_t StubReserve = compiler::machine::StubReserveBytes;

static_assert(StubReserve == uint64_t { 16 } * 1024, "the published stub reserve");

/// The host stack one nested invocation of compiled code takes, at the most: what a host function that
/// invokes the guest again adds below its caller's frames (published; P3 measured about 35 KB of the
/// reference's under ASan, and the compiled executor's nesting is no deeper).
inline constexpr uint64_t NestedInvocationBytes = uint64_t { 64 } * 1024;

/// The stack an entry takes before its function's frame, at the most, beyond its adapter's area: the entry
/// trampoline's saves (rbp, the host's preserved General registers, Microsoft x64's ten XMM registers and the
/// alignment) and the entry adapter's own fixed frame (its saved rbp, its two slots and a return address
/// each) (Trampoline.entryOverheadCoversTheEntry measures it). The adapter's stack arguments and result area,
/// 8 bytes per value of the entry's type past the registers (machine::ITarget::entryAdapterAreaBytes()), come
/// on top: every entry checks the two together, with the stub reserve, against the native limit before it
/// calls the trampoline. requiredNativeStack() counts them within each invocation's NestedInvocationBytes: at
/// the decode limits' 1'000 params and 1'000 results an area is 16 KiB at the most.
inline constexpr uint64_t EntryOverheadBytes = 512;

namespace detail
{
    /// @p a × @p b + @p c, saturated at the largest uint64_t.
    [[nodiscard]] constexpr uint64_t multiplyAdd(uint64_t a, uint64_t b, uint64_t c) noexcept
    {
        constexpr auto Largest = std::numeric_limits<uint64_t>::max();
        if (a != 0 && b > (Largest - c) / a)
            return Largest;
        return (a * b) + c;
    }
} // namespace detail

/// The native stack an outermost invocation needs so that a store's static limits always fire before the
/// native one (P5 Decision "Native stack limit"): bytesPerStackValue × maxStackValues + frameOverhead ×
/// maxCallDepth + maxNestedInvocations × NestedInvocationBytes + StubReserve + EntryOverheadBytes, saturated
/// at the largest uint64_t. A host function gets whatever stack is left beyond it.
/// @param rule The target's frame rule.
/// @param limits The store's limits.
/// @return The bytes.
[[nodiscard]] constexpr uint64_t requiredNativeStack(compiler::machine::FrameRule rule,
                                                     runtime::StoreLimits const& limits) noexcept
{
    auto bytes = StubReserve + EntryOverheadBytes;
    bytes = detail::multiplyAdd(rule.bytesPerStackValue, limits.maxStackValues, bytes);
    bytes = detail::multiplyAdd(rule.frameOverhead, limits.maxCallDepth, bytes);
    return detail::multiplyAdd(limits.maxNestedInvocations, NestedInvocationBytes, bytes);
}

/// The limit an outermost invocation entering at @p entry sets for every prologue
/// (ExecutionState::nativeStackLimit): max(@p entry − @p grant, @p lowest + StubReserve), so that it takes no
/// more than its grant and never runs into the thread's end; the subtraction saturates at 0.
/// @param entry The stack pointer at the entry.
/// @param grant The native stack the invocation is granted (CompiledExecutorConfig::nativeStackBytes).
/// @param lowest The thread's lowest usable address (IStackBounds).
/// @return The limit.
[[nodiscard]] constexpr std::uintptr_t nativeStackLimitOf(std::uintptr_t entry,
                                                          uint64_t grant,
                                                          std::uintptr_t lowest) noexcept
{
    auto const granted = entry > grant ? entry - static_cast<std::uintptr_t>(grant) : std::uintptr_t { 0 };
    return std::max(granted, lowest + static_cast<std::uintptr_t>(StubReserve));
}

} // namespace isoline::execution
