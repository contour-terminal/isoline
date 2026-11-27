// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <array>
#include <cstddef>
#include <ranges>
#include <utility>

#include <isoline/meta/EnumRows.hpp>
#include <isoline/platform/Platform.hpp>

namespace isoline::platform
{

/// What Isoline does on one (operating system, architecture): one row of PlatformTable. Each column
/// names the CMake-selected implementation of one seam, so behaviour per platform is data here and a
/// file per enumerator there, never #ifdef in logic.
struct PlatformRow
{
    OperatingSystem system = OperatingSystem::Linux;                        ///< The operating system.
    Architecture architecture = Architecture::X64;                          ///< The architecture.
    CodeMemoryKind codeMemory = CodeMemoryKind::Unavailable;                ///< How code is mapped.
    FaultMechanism faults = FaultMechanism::PosixSignals;                   ///< How faults arrive.
    InstructionCacheKind instructionCache = InstructionCacheKind::Coherent; ///< How code becomes visible.
    ControlFlowGuardKind controlFlowGuard = ControlFlowGuardKind::None;     ///< Which call-target check.
    UnwindRegistrationKind unwind = UnwindRegistrationKind::None;           ///< How unwind info registers.
    BranchTargetProtectionKind branchTargets = BranchTargetProtectionKind::None; ///< Landing-pad enforcement.
    DecommitKind decommit = DecommitKind::MadviseDontNeed; ///< How pages are decommitted.
};

/// The index of (@p system, @p architecture)'s row in PlatformTable: systems in enumerator order, and
/// within each its architectures in enumerator order.
/// @param system The operating system.
/// @param architecture The architecture.
/// @return The row's index.
[[nodiscard]] constexpr std::size_t platformIndex(OperatingSystem system, Architecture architecture) noexcept
{
    return (static_cast<std::size_t>(std::to_underlying(system)) * ArchitectureCount)
           + static_cast<std::size_t>(std::to_underlying(architecture));
}

/// Every platform, one row per (OperatingSystem, Architecture), at platformIndex(): an architecture
/// without a backend (Other) maps no code on any system.
inline constexpr auto PlatformTable = std::to_array<PlatformRow>({
    { .system = OperatingSystem::Linux,
      .architecture = Architecture::X64,
      .codeMemory = CodeMemoryKind::ReserveWriteThenExecute,
      .faults = FaultMechanism::PosixSignals,
      .instructionCache = InstructionCacheKind::Coherent,
      .controlFlowGuard = ControlFlowGuardKind::None,
      .unwind = UnwindRegistrationKind::None,
      .branchTargets = BranchTargetProtectionKind::None,
      .decommit = DecommitKind::MadviseDontNeed },
    { .system = OperatingSystem::Linux,
      .architecture = Architecture::Arm64,
      .codeMemory = CodeMemoryKind::ReserveWriteThenExecute,
      .faults = FaultMechanism::PosixSignals,
      .instructionCache = InstructionCacheKind::LinuxArm64,
      .controlFlowGuard = ControlFlowGuardKind::None,
      .unwind = UnwindRegistrationKind::None,
      .branchTargets = BranchTargetProtectionKind::LinuxProtBti,
      .decommit = DecommitKind::MadviseDontNeed },
    { .system = OperatingSystem::Linux,
      .architecture = Architecture::Other,
      .codeMemory = CodeMemoryKind::Unavailable,
      .faults = FaultMechanism::PosixSignals,
      .instructionCache = InstructionCacheKind::Coherent,
      .controlFlowGuard = ControlFlowGuardKind::None,
      .unwind = UnwindRegistrationKind::None,
      .branchTargets = BranchTargetProtectionKind::None,
      .decommit = DecommitKind::MadviseDontNeed },
    { .system = OperatingSystem::MacOS,
      .architecture = Architecture::X64,
      .codeMemory = CodeMemoryKind::MapJitWithProtection,
      .faults = FaultMechanism::PosixSignals,
      .instructionCache = InstructionCacheKind::Coherent,
      .controlFlowGuard = ControlFlowGuardKind::None,
      .unwind = UnwindRegistrationKind::None,
      .branchTargets = BranchTargetProtectionKind::None,
      .decommit = DecommitKind::MapFixedAnonymous },
    { .system = OperatingSystem::MacOS,
      .architecture = Architecture::Arm64,
      .codeMemory = CodeMemoryKind::MapJitWithThreadToggle,
      .faults = FaultMechanism::PosixSignals,
      .instructionCache = InstructionCacheKind::MacOSArm64,
      .controlFlowGuard = ControlFlowGuardKind::None,
      .unwind = UnwindRegistrationKind::None,
      .branchTargets = BranchTargetProtectionKind::None,
      .decommit = DecommitKind::MapFixedAnonymous },
    { .system = OperatingSystem::MacOS,
      .architecture = Architecture::Other,
      .codeMemory = CodeMemoryKind::Unavailable,
      .faults = FaultMechanism::PosixSignals,
      .instructionCache = InstructionCacheKind::Coherent,
      .controlFlowGuard = ControlFlowGuardKind::None,
      .unwind = UnwindRegistrationKind::None,
      .branchTargets = BranchTargetProtectionKind::None,
      .decommit = DecommitKind::MapFixedAnonymous },
    { .system = OperatingSystem::FreeBSD,
      .architecture = Architecture::X64,
      .codeMemory = CodeMemoryKind::ReserveWriteThenExecute,
      .faults = FaultMechanism::PosixSignals,
      .instructionCache = InstructionCacheKind::Coherent,
      .controlFlowGuard = ControlFlowGuardKind::None,
      .unwind = UnwindRegistrationKind::None,
      .branchTargets = BranchTargetProtectionKind::None,
      .decommit = DecommitKind::MapFixedAnonymous },
    { .system = OperatingSystem::FreeBSD,
      .architecture = Architecture::Arm64,
      .codeMemory = CodeMemoryKind::ReserveWriteThenExecute,
      .faults = FaultMechanism::PosixSignals,
      .instructionCache = InstructionCacheKind::FreeBSDArm64,
      .controlFlowGuard = ControlFlowGuardKind::None,
      .unwind = UnwindRegistrationKind::None,
      .branchTargets = BranchTargetProtectionKind::None,
      .decommit = DecommitKind::MapFixedAnonymous },
    { .system = OperatingSystem::FreeBSD,
      .architecture = Architecture::Other,
      .codeMemory = CodeMemoryKind::Unavailable,
      .faults = FaultMechanism::PosixSignals,
      .instructionCache = InstructionCacheKind::Coherent,
      .controlFlowGuard = ControlFlowGuardKind::None,
      .unwind = UnwindRegistrationKind::None,
      .branchTargets = BranchTargetProtectionKind::None,
      .decommit = DecommitKind::MapFixedAnonymous },
    { .system = OperatingSystem::Windows,
      .architecture = Architecture::X64,
      .codeMemory = CodeMemoryKind::ReserveWriteThenExecute,
      .faults = FaultMechanism::VectoredExceptions,
      .instructionCache = InstructionCacheKind::Coherent,
      .controlFlowGuard = ControlFlowGuardKind::Windows,
      .unwind = UnwindRegistrationKind::WindowsFunctionTableX64,
      .branchTargets = BranchTargetProtectionKind::None,
      .decommit = DecommitKind::VirtualFreeDecommit },
    { .system = OperatingSystem::Windows,
      .architecture = Architecture::Arm64,
      .codeMemory = CodeMemoryKind::ReserveWriteThenExecute,
      .faults = FaultMechanism::VectoredExceptions,
      .instructionCache = InstructionCacheKind::WindowsArm64,
      .controlFlowGuard = ControlFlowGuardKind::Windows,
      .unwind = UnwindRegistrationKind::WindowsFunctionTableArm64,
      .branchTargets = BranchTargetProtectionKind::None,
      .decommit = DecommitKind::VirtualFreeDecommit },
    { .system = OperatingSystem::Windows,
      .architecture = Architecture::Other,
      .codeMemory = CodeMemoryKind::Unavailable,
      .faults = FaultMechanism::VectoredExceptions,
      .instructionCache = InstructionCacheKind::Coherent,
      .controlFlowGuard = ControlFlowGuardKind::None,
      .unwind = UnwindRegistrationKind::None,
      .branchTargets = BranchTargetProtectionKind::None,
      .decommit = DecommitKind::VirtualFreeDecommit },
});

static_assert(PlatformTable.size() == OperatingSystemCount * ArchitectureCount,
              "PlatformTable has one row per (OperatingSystem, Architecture)");
static_assert(
    [] {
        for (auto const index: std::views::iota(std::size_t { 0 }, PlatformTable.size()))
        {
            auto const& row = PlatformTable[index];
            if (platformIndex(row.system, row.architecture) != index)
                return false;
        }
        return true;
    }(),
    "every PlatformTable row sits at its platformIndex()");

/// The row of (@p system, @p architecture).
/// @param system The operating system.
/// @param architecture The architecture.
/// @return Its row of PlatformTable; both enumerators must be in range (checked).
[[nodiscard]] constexpr PlatformRow const& platformRow(OperatingSystem system,
                                                       Architecture architecture) noexcept
{
    Require(std::to_underlying(system) <= std::to_underlying(LastOperatingSystem));
    Require(std::to_underlying(architecture) <= std::to_underlying(LastArchitecture));
    return PlatformTable[platformIndex(system, architecture)];
}

} // namespace isoline::platform
