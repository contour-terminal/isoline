// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <utility>

#include <isoline/platform/Platform.hpp>
#include <isoline/platform/PlatformTable.hpp>
#include <isoline/platform/ThisPlatform.hpp>

using namespace isoline::platform;

namespace
{

/// Every OperatingSystem enumerator.
constexpr auto Systems = std::to_array(
    { OperatingSystem::Linux, OperatingSystem::MacOS, OperatingSystem::FreeBSD, OperatingSystem::Windows });

/// Every Architecture enumerator.
constexpr auto Architectures = std::to_array({ Architecture::X64, Architecture::Arm64, Architecture::Other });

static_assert(Systems.size() == OperatingSystemCount);
static_assert(Architectures.size() == ArchitectureCount);

// The compiler's own predefined macros: the independent source the configure's choice is checked
// against (the configure asks the same compiler, through CMake; this asks it directly). A build for
// several architectures at once compiles this once per architecture but has one platform row, Other.
#ifdef _WIN32
constexpr auto CompilerSystem = std::optional { OperatingSystem::Windows };
#elifdef __APPLE__
constexpr auto CompilerSystem = std::optional { OperatingSystem::MacOS };
#elifdef __linux__
constexpr auto CompilerSystem = std::optional { OperatingSystem::Linux };
#elifdef __FreeBSD__
constexpr auto CompilerSystem = std::optional { OperatingSystem::FreeBSD };
#else
constexpr auto CompilerSystem = std::optional<OperatingSystem> {};
#endif

#if ISOLINE_TEST_SEVERAL_ARCHITECTURES
constexpr auto CompilerArchitecture = Architecture::Other;
#elif (defined(__x86_64__) || defined(_M_X64)) && !defined(_M_ARM64EC)
constexpr auto CompilerArchitecture = Architecture::X64;
#elif defined(__aarch64__) || (defined(_M_ARM64) && !defined(_M_ARM64EC))
constexpr auto CompilerArchitecture = Architecture::Arm64;
#else
constexpr auto CompilerArchitecture = Architecture::Other;
#endif

} // namespace

TEST_CASE("PlatformTable.rowsAreCompleteAndOrdered")
{
    // One row per (system, architecture), systems outermost: platformRow() finds each pair's own row.
    auto expectedIndex = std::size_t { 0 };
    for (auto const system: Systems)
    {
        for (auto const architecture: Architectures)
        {
            INFO("system " << std::to_underlying(system) << ", architecture "
                           << std::to_underlying(architecture));
            auto const& row = PlatformTable[expectedIndex];
            CHECK(row.system == system);
            CHECK(row.architecture == architecture);
            CHECK(&platformRow(system, architecture) == &row);
            ++expectedIndex;
        }
    }
    CHECK(expectedIndex == PlatformTable.size());

    CHECK(platformRow(OperatingSystem::MacOS, Architecture::Arm64).codeMemory
          == CodeMemoryKind::MapJitWithThreadToggle);
    CHECK(platformRow(OperatingSystem::MacOS, Architecture::X64).codeMemory
          == CodeMemoryKind::MapJitWithProtection);
    CHECK(platformRow(OperatingSystem::Linux, Architecture::Arm64).branchTargets
          == BranchTargetProtectionKind::LinuxProtBti);
    CHECK(platformRow(OperatingSystem::Windows, Architecture::X64).unwind
          == UnwindRegistrationKind::WindowsFunctionTableX64);
    CHECK(platformRow(OperatingSystem::Windows, Architecture::Arm64).unwind
          == UnwindRegistrationKind::WindowsFunctionTableArm64);
    for (auto const& row: PlatformTable)
    {
        INFO("system " << std::to_underlying(row.system) << ", architecture "
                       << std::to_underlying(row.architecture));
        auto const isWindows = row.system == OperatingSystem::Windows;
        CHECK((row.faults == FaultMechanism::VectoredExceptions) == isWindows);
        // An architecture without a backend generates no code, so it needs no code-side seam.
        if (row.architecture == Architecture::Other)
        {
            CHECK(row.codeMemory == CodeMemoryKind::Unavailable);
            CHECK(row.controlFlowGuard == ControlFlowGuardKind::None);
            CHECK(row.unwind == UnwindRegistrationKind::None);
            continue;
        }
        CHECK(row.codeMemory != CodeMemoryKind::Unavailable);
        CHECK((row.controlFlowGuard == ControlFlowGuardKind::Windows) == isWindows);
        CHECK((row.unwind != UnwindRegistrationKind::None) == isWindows);
        // Only Linux arm64 enforces landing pads through its page protection.
        CHECK((row.branchTargets == BranchTargetProtectionKind::LinuxProtBti)
              == (row.system == OperatingSystem::Linux && row.architecture == Architecture::Arm64));
        // MAP_JIT is macOS's alone.
        CHECK((row.codeMemory == CodeMemoryKind::ReserveWriteThenExecute)
              == (row.system != OperatingSystem::MacOS));
    }
}

TEST_CASE("PlatformTable.thisPlatformMatchesTheBuild")
{
    REQUIRE(CompilerSystem.has_value());
    CHECK(ThisPlatform.system == *CompilerSystem);
    CHECK(ThisPlatform.architecture == CompilerArchitecture);
    CHECK(&platformRow(ThisPlatform.system, ThisPlatform.architecture)
          == &PlatformTable[platformIndex(ThisPlatform.system, ThisPlatform.architecture)]);
}

TEST_CASE("PlatformTable.decommitMechanismPerSystem")
{
    // Only Linux's MADV_DONTNEED discards a private mapping's pages; macOS's and FreeBSD's is a paging
    // hint that keeps them, so those rows map fresh pages over the range instead.
    for (auto const architecture: Architectures)
    {
        INFO("architecture " << std::to_underlying(architecture));
        CHECK(platformRow(OperatingSystem::Linux, architecture).decommit == DecommitKind::MadviseDontNeed);
        CHECK(platformRow(OperatingSystem::MacOS, architecture).decommit == DecommitKind::MapFixedAnonymous);
        CHECK(platformRow(OperatingSystem::FreeBSD, architecture).decommit
              == DecommitKind::MapFixedAnonymous);
        CHECK(platformRow(OperatingSystem::Windows, architecture).decommit
              == DecommitKind::VirtualFreeDecommit);
    }
}

TEST_CASE("PlatformTable.everyArm64RowNamesItsInstructionCache")
{
    CHECK(platformRow(OperatingSystem::Linux, Architecture::Arm64).instructionCache
          == InstructionCacheKind::LinuxArm64);
    CHECK(platformRow(OperatingSystem::FreeBSD, Architecture::Arm64).instructionCache
          == InstructionCacheKind::FreeBSDArm64);
    CHECK(platformRow(OperatingSystem::MacOS, Architecture::Arm64).instructionCache
          == InstructionCacheKind::MacOSArm64);
    CHECK(platformRow(OperatingSystem::Windows, Architecture::Arm64).instructionCache
          == InstructionCacheKind::WindowsArm64);
    for (auto const system: Systems)
    {
        INFO("system " << std::to_underlying(system));
        CHECK(platformRow(system, Architecture::X64).instructionCache == InstructionCacheKind::Coherent);
    }
}
