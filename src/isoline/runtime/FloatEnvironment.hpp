// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace isoline::runtime
{

/// The host target's floating-point control register, widened to 64 bits: MXCSR on x86-64, FPCR on
/// arm64, nothing (always 0) on the portable fallback.
using FloatControlWord = uint64_t;

/// One target's row: which bits of its control register the IEEE 754 defaults Wasm assumes
/// (round-to-nearest-even, subnormals neither flushed nor treated as zero, no float traps) clear or
/// set. The rounding mode the C library reports (std::fegetround()) is kept apart from it, so that
/// the portable fallback, which has no register, is a row of zeros.
struct FloatControlLayout
{
    FloatControlWord cleared = 0; ///< The bits the defaults clear: rounding mode, flushes, trap enables.
    FloatControlWord set = 0;     ///< The bits the defaults set: x86-64's exception masks.
    /// The bits that flush subnormal results or operands to zero (x86-64 FTZ and DAZ, arm64 FZ and
    /// FZ16), a subset of cleared; 0 where the target cannot control them.
    FloatControlWord flushes = 0;
    /// The bits that, flipped from the defaults, let a float exception trap (x86-64's exception masks,
    /// arm64's trap enables), a subset of cleared or set; 0 where the target has none.
    FloatControlWord traps = 0;
};

/// The control bits of @p word that @p layout governs, the sticky exception flags and every other bit
/// aside: what FloatEnvironmentScope restores.
/// @param layout The target's row.
/// @param word A value of the target's control register.
/// @return The bits of @p word in layout.cleared or layout.set.
[[nodiscard]] constexpr FloatControlWord governedBits(FloatControlLayout const& layout,
                                                      FloatControlWord word) noexcept
{
    return word & (layout.cleared | layout.set);
}

/// @p current with the bits @p layout governs taken from @p saved: the control state of @p saved,
/// with the sticky exception flags and every other bit of @p current kept.
/// @param layout The target's row.
/// @param current The register's value now.
/// @param saved The register's value whose control state to restore.
/// @return The register's value to write.
[[nodiscard]] constexpr FloatControlWord withControlOf(FloatControlLayout const& layout,
                                                       FloatControlWord current,
                                                       FloatControlWord saved) noexcept
{
    return governedBits(layout, saved) | (current & ~(layout.cleared | layout.set));
}

/// @p word with the IEEE defaults of @p layout established, every other bit as it was.
/// @param layout The target's row.
/// @param word A value of the target's control register.
/// @return The register's value under the defaults.
[[nodiscard]] constexpr FloatControlWord ieeeDefaults(FloatControlLayout const& layout,
                                                      FloatControlWord word) noexcept
{
    return (word & ~layout.cleared) | layout.set;
}

/// The row of the target Isoline was built for. CMake compiles exactly one of
/// FloatControlX86_64.cpp, FloatControlArm64.cpp and FloatControlPortable.cpp, from the compiler's
/// target architecture. MSVC on arm64, and a build for several architectures at once (a universal
/// macOS build), take the portable row: there only the rounding mode is pinned.
/// @return The row.
[[nodiscard]] FloatControlLayout const& hostFloatControlLayout() noexcept;

/// Reads the host's control register.
/// @return Its value; 0 on the portable fallback.
[[nodiscard]] FloatControlWord readFloatControl() noexcept;

/// Writes the host's control register; does nothing on the portable fallback.
/// @param word The value to write.
void writeFloatControl(FloatControlWord word) noexcept;

/// Establishes the IEEE defaults on the calling thread: round-to-nearest-even as the C library
/// sees it (std::fesetround()), and the control register's row (ieeeDefaults()). Writes only what
/// differs. What an executor does when a host function returns (HostCallScope), so that a host that
/// changed the environment cannot change the results of the guest code that called it.
void establishFloatDefaults() noexcept;

/// Pins the floating-point environment of the calling thread to the IEEE defaults for as long as it
/// lives, and gives the host its own back at the end: what every invocation of guest code opens
/// (InvocationScope), nested ones included, because a host function may change the environment and
/// then re-enter. A host that rounds upward or flushes subnormals therefore cannot change a Wasm
/// result.
///
/// What it restores is the host's control state: its rounding mode and its row's bits (cleared and
/// set). The sticky exception flags are status, not control: the guest's float operations may raise
/// them, as any of the host's own would, and they stay raised after the scope. Costs one read of the
/// rounding mode and one of the control register at entry; writes happen only where the host's
/// environment differs from the defaults. On the portable row, flushing to zero cannot be controlled
/// and only the rounding mode is pinned.
///
/// No `#pragma STDC FENV_ACCESS` is used. What keeps the compiler from moving a guest's float
/// operation across the scope is the executor boundary: guest code runs only inside the virtual
/// ICodeExecutor::invoke(), which the entry calls after the scope opened and returns from before it
/// closes, and whose operands come from the guest's stacks; the compiler cannot see through the
/// virtual call to move what it does, link-time optimization included.
class FloatEnvironmentScope
{
  public:
    /// Saves the host's environment and establishes the defaults.
    FloatEnvironmentScope() noexcept;

    /// Restores the host's environment saved at construction.
    ~FloatEnvironmentScope();

    FloatEnvironmentScope(FloatEnvironmentScope const&) = delete;
    FloatEnvironmentScope(FloatEnvironmentScope&&) = delete;
    FloatEnvironmentScope& operator=(FloatEnvironmentScope const&) = delete;
    FloatEnvironmentScope& operator=(FloatEnvironmentScope&&) = delete;

  private:
    FloatControlWord _hostControl; ///< The host's control register.
    int _hostRounding;             ///< The host's rounding mode (std::fegetround()).
};

} // namespace isoline::runtime
