// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace isoline::platform
{

/// An operating system Isoline has a platform row for (PlatformTable). An OS without one fails the
/// configure (cmake/IsolinePlatform.cmake), so no build falls back to another's row silently.
enum class OperatingSystem : uint8_t
{
    Linux,   ///< Linux, with glibc or another libc.
    MacOS,   ///< macOS (any Apple target the compiler names __APPLE__).
    FreeBSD, ///< FreeBSD.
    Windows, ///< Windows.
};

/// The last OperatingSystem enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastOperatingSystem = OperatingSystem::Windows;

/// The number of OperatingSystem enumerators, as LastOperatingSystem counts them.
inline constexpr auto OperatingSystemCount = static_cast<std::size_t>(LastOperatingSystem) + 1;

/// The architecture a build's code runs on. Spelled as compiler::machine::Architecture spells it (X64:
/// the naming rule's CamelCase has no room for x86_64's underscore).
enum class Architecture : uint8_t
{
    X64,   ///< x86-64 (AMD64, Intel 64).
    Arm64, ///< AArch64.
    /// Any other: an architecture no backend generates code for (riscv64, ppc64le, …), ARM64EC, or a
    /// build for several architectures at once, where no one row is the host's.
    Other,
};

/// The last Architecture enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastArchitecture = Architecture::Other;

/// The number of Architecture enumerators, as LastArchitecture counts them.
inline constexpr auto ArchitectureCount = static_cast<std::size_t>(LastArchitecture) + 1;

/// How a platform maps generated code: the sealing sequence its code memory follows.
enum class CodeMemoryKind : uint8_t
{
    /// Reserve inaccessible, make writable, copy, make executable: never writable and executable at once.
    ReserveWriteThenExecute,
    /// macOS x86-64: map with MAP_JIT read-write, copy, make read-execute.
    MapJitWithProtection,
    /// macOS arm64: map with MAP_JIT read-write-execute, behind the per-thread write-protection toggle.
    MapJitWithThreadToggle,
    /// No code can be generated: an architecture without a backend.
    Unavailable,
};

/// The last CodeMemoryKind enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastCodeMemoryKind = CodeMemoryKind::Unavailable;

/// How a platform reports a fault to the process.
enum class FaultMechanism : uint8_t
{
    PosixSignals,       ///< SIGSEGV/SIGBUS, through sigaction.
    VectoredExceptions, ///< Windows vectored exception handlers.
};

/// The last FaultMechanism enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastFaultMechanism = FaultMechanism::VectoredExceptions;

/// How a platform makes freshly written code visible to instruction fetch.
enum class InstructionCacheKind : uint8_t
{
    Coherent,     ///< The hardware keeps instruction fetch coherent with stores (x86-64): nothing to do.
    LinuxArm64,   ///< __builtin___clear_cache, then membarrier(SYNC_CORE).
    FreeBSDArm64, ///< __builtin___clear_cache.
    MacOSArm64,   ///< sys_icache_invalidate.
    WindowsArm64, ///< FlushInstructionCache.
};

/// The last InstructionCacheKind enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastInstructionCacheKind = InstructionCacheKind::WindowsArm64;

/// Which control-flow integrity check of indirect calls a platform has.
enum class ControlFlowGuardKind : uint8_t
{
    None,    ///< No check of call targets.
    Windows, ///< Windows Control Flow Guard (SetProcessValidCallTargets).
};

/// The last ControlFlowGuardKind enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastControlFlowGuardKind = ControlFlowGuardKind::Windows;

/// How a platform learns the unwind information of generated code.
enum class UnwindRegistrationKind : uint8_t
{
    None,                      ///< Nothing is registered: frame-pointer chains serve every walker.
    WindowsFunctionTableX64,   ///< RtlAddFunctionTable with x64 RUNTIME_FUNCTIONs.
    WindowsFunctionTableArm64, ///< RtlAddFunctionTable with arm64 RUNTIME_FUNCTIONs.
};

/// The last UnwindRegistrationKind enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastUnwindRegistrationKind = UnwindRegistrationKind::WindowsFunctionTableArm64;

/// Whether, and how, a platform enforces branch-target landing pads on generated code.
enum class BranchTargetProtectionKind : uint8_t
{
    None,         ///< Landing pads, where emitted, are unenforced.
    LinuxProtBti, ///< Linux arm64: pages mapped with PROT_BTI.
};

/// The last BranchTargetProtectionKind enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastBranchTargetProtectionKind = BranchTargetProtectionKind::LinuxProtBti;

/// How a platform decommits pages (Protection::None): returns their memory to the system so that the
/// next commit reads zeroes, never the previous owner's bytes.
enum class DecommitKind : uint8_t
{
    /// Linux: mprotect(PROT_NONE), then madvise(MADV_DONTNEED), which drops a private anonymous
    /// mapping's pages.
    MadviseDontNeed,
    /// macOS, FreeBSD: mmap(PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED) over the range, a fresh
    /// mapping in place (their MADV_DONTNEED is only a paging hint that keeps the contents), as Wasmtime
    /// does. On older Linux kernels, where only a test selects it (SystemVirtualMemory(DecommitKind)), a
    /// refused MAP_FIXED may already have unmapped the range: it leaves a hole in the reservation, which a
    /// later mapping could take.
    MapFixedAnonymous,
    /// Windows: VirtualFree(MEM_DECOMMIT).
    VirtualFreeDecommit,
};

/// The last DecommitKind enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastDecommitKind = DecommitKind::VirtualFreeDecommit;

} // namespace isoline::platform
