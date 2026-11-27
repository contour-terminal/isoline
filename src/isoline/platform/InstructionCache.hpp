// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>

#include <isoline/meta/EnumRows.hpp>
#include <isoline/platform/Membarrier.hpp>
#include <isoline/platform/Platform.hpp>

namespace isoline::platform
{

/// How a platform makes bytes just written visible to every processor's instruction fetch: the ARM ARM
/// recipe (dc cvau; dsb ish; ic ivau; dsb ish, broadcast to the inner-shareable domain), through the
/// operating system's own call.
enum class InstructionCacheInvalidation : uint8_t
{
    None,               ///< Instruction fetch is coherent with stores (x86-64): nothing to do.
    CompilerClearCache, ///< __builtin___clear_cache (Linux, FreeBSD).
    AppleInvalidate,    ///< sys_icache_invalidate (macOS).
    WindowsFlush,       ///< FlushInstructionCache(GetCurrentProcess(), …) (Windows).
};

/// The last InstructionCacheInvalidation enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastInstructionCacheInvalidation = InstructionCacheInvalidation::WindowsFlush;

/// How a platform makes every other thread execute a context-synchronizing event after code was
/// invalidated, where the operating system's invalidation does not already.
enum class OtherThreadSynchronization : uint8_t
{
    /// Nothing more is done: the invalidation covers every processor (Windows documents it, macOS's is
    /// what Apple's own JITs rely on, FreeBSD has no command for it), or the hardware needs nothing
    /// (x86-64). Every thread still synchronizes itself before it runs the code (ThisThreadSynchronization).
    None,
    /// membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE), registered once when the cache is built.
    MembarrierSynchronizeCore,
};

/// The last OtherThreadSynchronization enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastOtherThreadSynchronization = OtherThreadSynchronization::MembarrierSynchronizeCore;

/// How a thread context-synchronizes itself before it executes code another thread synchronized.
enum class ThisThreadSynchronization : uint8_t
{
    None,                              ///< Nothing (x86-64).
    InstructionSynchronizationBarrier, ///< arm64 isb.
};

/// The last ThisThreadSynchronization enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastThisThreadSynchronization =
    ThisThreadSynchronization::InstructionSynchronizationBarrier;

/// What one InstructionCacheKind does: one row of InstructionCacheTable.
struct InstructionCacheRow
{
    InstructionCacheKind kind = InstructionCacheKind::Coherent; ///< The platform row's kind.
    /// How written code is invalidated.
    InstructionCacheInvalidation invalidation = InstructionCacheInvalidation::None;
    /// What then makes the other threads synchronize.
    OtherThreadSynchronization otherThreads = OtherThreadSynchronization::None;
    /// How a thread synchronizes itself (IInstructionCache::synchronizeThisThread()).
    ThisThreadSynchronization thisThread = ThisThreadSynchronization::None;
};

/// Every InstructionCacheKind, at its enumerator's index.
inline constexpr auto InstructionCacheTable = std::to_array<InstructionCacheRow>({
    { .kind = InstructionCacheKind::Coherent,
      .invalidation = InstructionCacheInvalidation::None,
      .otherThreads = OtherThreadSynchronization::None,
      .thisThread = ThisThreadSynchronization::None },
    { .kind = InstructionCacheKind::LinuxArm64,
      .invalidation = InstructionCacheInvalidation::CompilerClearCache,
      .otherThreads = OtherThreadSynchronization::MembarrierSynchronizeCore,
      .thisThread = ThisThreadSynchronization::InstructionSynchronizationBarrier },
    { .kind = InstructionCacheKind::FreeBSDArm64,
      .invalidation = InstructionCacheInvalidation::CompilerClearCache,
      .otherThreads = OtherThreadSynchronization::None,
      .thisThread = ThisThreadSynchronization::InstructionSynchronizationBarrier },
    { .kind = InstructionCacheKind::MacOSArm64,
      .invalidation = InstructionCacheInvalidation::AppleInvalidate,
      .otherThreads = OtherThreadSynchronization::None,
      .thisThread = ThisThreadSynchronization::InstructionSynchronizationBarrier },
    { .kind = InstructionCacheKind::WindowsArm64,
      .invalidation = InstructionCacheInvalidation::WindowsFlush,
      .otherThreads = OtherThreadSynchronization::None,
      .thisThread = ThisThreadSynchronization::InstructionSynchronizationBarrier },
});

static_assert(meta::firstMisplacedRow<LastInstructionCacheKind>(InstructionCacheTable,
                                                                &InstructionCacheRow::kind)
                  == meta::NoMisplacedRow,
              "InstructionCacheTable has one row per enumerator, at its index");

/// The row of @p kind.
/// @param kind The kind.
/// @return Its row of InstructionCacheTable; @p kind must be an enumerator (checked).
[[nodiscard]] constexpr InstructionCacheRow const& instructionCacheRow(InstructionCacheKind kind) noexcept
{
    Require(std::to_underlying(kind) < InstructionCacheTable.size());
    return InstructionCacheTable[std::to_underlying(kind)];
}

/// Why making code executable failed.
enum class InstructionCacheError : uint8_t
{
    /// The process could not register for the other threads' synchronization (membarrier's
    /// REGISTER_PRIVATE_EXPEDITED_SYNC_CORE refused): no cache is built, since none could keep its promise.
    RegistrationRefused,
    /// Invalidating the code, or synchronizing the other threads, was refused: the code may be stale for
    /// some thread, and must not run.
    SynchronizationRefused,
};

/// The last InstructionCacheError enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastInstructionCacheError = InstructionCacheError::SynchronizationRefused;

/// A failed instruction-cache call: why, and the system's own code for it.
struct InstructionCacheFailure
{
    InstructionCacheError error = InstructionCacheError::RegistrationRefused; ///< Why.
    /// The system's error code: errno on POSIX, GetLastError() on Windows.
    int systemCode = 0;

    /// Failures are equal when both their error and their system code are.
    [[nodiscard]] friend constexpr bool operator==(InstructionCacheFailure const&,
                                                   InstructionCacheFailure const&) noexcept = default;
};

/// The processor's cache maintenance, as the operating system offers it: the calls an
/// InstructionCacheRow names. Reached only through this interface, so that every row's sequence runs,
/// recorded, on any build.
class IInstructionCacheMaintenance
{
  public:
    virtual ~IInstructionCacheMaintenance() = default;

    /// Invalidates the instruction-cache lines of @p code for every processor, by this platform's
    /// InstructionCacheInvalidation.
    /// @param code The bytes just written.
    /// @return Nothing; or the system's error code.
    [[nodiscard]] virtual std::expected<void, int> invalidate(std::span<std::byte const> code) noexcept = 0;

    /// Context-synchronizes the calling thread (arm64 isb).
    virtual void synchronizeContext() noexcept = 0;
};

/// This build's cache maintenance, by its platform row (the CMake-selected
/// InstructionCache{Coherent,ClearCacheArm64,MacOSArm64,WindowsArm64}.cpp).
/// @return The maintenance.
[[nodiscard]] std::unique_ptr<IInstructionCacheMaintenance> makeSystemInstructionCacheMaintenance();

/// Makes freshly written code executable by every thread of the process: the one coherence seam of both
/// code generators.
class IInstructionCache
{
  public:
    virtual ~IInstructionCache() = default;

    /// Makes @p code, just written, executable by every thread of the process, as far as the platform
    /// can do it for them: each thread still synchronizes itself (synchronizeThisThread(), or the
    /// trampoline's isb) before it runs the code.
    /// @param code The bytes just written.
    /// @return Nothing; or SynchronizationRefused with the system's code, after which the code must not run.
    [[nodiscard]] virtual std::expected<void, InstructionCacheFailure> synchronize(
        std::span<std::byte const> code) noexcept = 0;

    /// Context-synchronizes the calling thread (arm64 isb; nothing on Coherent), so it may execute code
    /// another thread's synchronize() made executable.
    virtual void synchronizeThisThread() noexcept = 0;
};

/// The instruction cache of @p kind, interpreting its InstructionCacheTable row over @p maintenance and
/// @p membarrier. A row whose other threads synchronize by membarrier registers SYNC_CORE here, once per
/// cache built (never per synchronize()): the cache is built before any lock-down, so that the sandbox
/// never needs to allow the registration. The registration is the process's, and the kernel takes a
/// repeated one as a no-op, so a second cache costs one more system call and nothing else.
/// @param kind The platform row's kind.
/// @param maintenance The cache maintenance; owned by the cache.
/// @param membarrier The membarrier; must outlive the cache.
/// @return The cache; or RegistrationRefused with the system's code.
[[nodiscard]] std::expected<std::unique_ptr<IInstructionCache>, InstructionCacheFailure> makeInstructionCache(
    InstructionCacheKind kind,
    std::unique_ptr<IInstructionCacheMaintenance> maintenance,
    IMembarrier& membarrier);

/// This build's instruction cache: makeInstructionCache() of its platform row's kind, over the system's
/// cache maintenance.
/// @param membarrier The membarrier (makeSystemMembarrier() in production); must outlive the cache.
/// @return The cache; or RegistrationRefused with the system's code.
[[nodiscard]] std::expected<std::unique_ptr<IInstructionCache>, InstructionCacheFailure>
makeSystemInstructionCache(IMembarrier& membarrier);

} // namespace isoline::platform
