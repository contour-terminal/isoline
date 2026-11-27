// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory_resource>
#include <variant>
#include <vector>

#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/machine/CallingConvention.hpp>
#include <isoline/compiler/machine/TargetDescription.hpp>
#include <isoline/compiler/machine/testing/FakeTarget.hpp>

using isoline::compiler::intermediate::Type;
using isoline::compiler::machine::Architecture;
using isoline::compiler::machine::ArgumentSlots;
using isoline::compiler::machine::assignArguments;
using isoline::compiler::machine::assignResults;
using isoline::compiler::machine::Convention;
using isoline::compiler::machine::CpuFeatures;
using isoline::compiler::machine::hasHostConvention;
using isoline::compiler::machine::hostConvention;
using isoline::compiler::machine::OperatingSystemAbi;
using isoline::compiler::machine::PhysicalRegister;
using isoline::compiler::machine::RegisterClass;
using isoline::compiler::machine::StackOffset;
using isoline::compiler::machine::TargetDescription;
using isoline::compiler::machine::ValueLocation;
using isoline::compiler::machine::testing::FakeConvention;

namespace
{

/// General register @p number.
constexpr ValueLocation g(uint8_t number)
{
    return PhysicalRegister { .registerClass = RegisterClass::General, .number = number };
}

/// Float register @p number.
constexpr ValueLocation f(uint8_t number)
{
    return PhysicalRegister { .registerClass = RegisterClass::Float, .number = number };
}

/// The stack slot at @p bytes.
constexpr ValueLocation stack(uint32_t bytes)
{
    return StackOffset { .bytes = bytes };
}

/// The locations of an assignment, as a plain vector.
std::vector<ValueLocation> locationsOf(isoline::compiler::machine::Assignment const& assignment)
{
    return { assignment.locations.begin(), assignment.locations.end() };
}

} // namespace

TEST_CASE("CallingConvention.assignsInOrder")
{
    // FakeTarget's row: two argument registers per class, counted per class (R6: the x86-64 SystemV case
    // moves to Task 11, beside its rows). A Ref is integer class; the rest go to the stack in 8-byte slots,
    // and the area rounds up to the 16-byte stack alignment.
    std::pmr::monotonic_buffer_resource memory;
    auto const types =
        std::to_array({ Type::I32, Type::F64, Type::Ref, Type::F32, Type::I64, Type::F64, Type::I32 });
    auto const assignment = assignArguments(FakeConvention, types, memory);
    CHECK(locationsOf(assignment)
          == std::vector<ValueLocation> { g(0), f(0), g(1), f(1), stack(0), stack(8), stack(16) });
    CHECK(assignment.memoryBytes == 32);

    // Nothing on the stack: no area.
    auto const small = assignArguments(FakeConvention, std::to_array({ Type::Pointer, Type::F32 }), memory);
    CHECK(locationsOf(small) == std::vector<ValueLocation> { g(0), f(0) });
    CHECK(small.memoryBytes == 0);
}

TEST_CASE("CallingConvention.sharedSlotsCountPositions")
{
    // A Microsoft x64-shaped row (two registers per class, shared positions, 16 bytes of shadow space):
    // the n-th argument takes the n-th register of its class, and past them the stack above the shadow space.
    auto row = FakeConvention;
    row.slots = ArgumentSlots::Shared;
    row.shadowSpaceBytes = 16;
    std::pmr::monotonic_buffer_resource memory;
    auto const assignment = assignArguments(
        row, std::to_array({ Type::I32, Type::F64, Type::I64, Type::F32, Type::Ref }), memory);
    CHECK(locationsOf(assignment)
          == std::vector<ValueLocation> { g(0), f(1), stack(16), stack(24), stack(32) });
    CHECK(assignment.memoryBytes == 48);

    // Shadow space is reserved even when every argument is in a register.
    auto const registersOnly = assignArguments(row, std::to_array({ Type::F32 }), memory);
    CHECK(locationsOf(registersOnly) == std::vector<ValueLocation> { f(0) });
    CHECK(registersOnly.memoryBytes == 16);
}

TEST_CASE("CallingConvention.resultsSpillIntoTheResultArea")
{
    // FakeTarget returns one value per class in registers, the rest in the caller's result area.
    std::pmr::monotonic_buffer_resource memory;
    auto const assignment = assignResults(
        FakeConvention, std::to_array({ Type::I32, Type::I64, Type::F64, Type::F32, Type::Ref }), memory);
    CHECK(locationsOf(assignment)
          == std::vector<ValueLocation> { g(0), stack(0), f(0), stack(8), stack(16) });
    CHECK(assignment.memoryBytes == 24);

    auto const none = assignResults(FakeConvention, {}, memory);
    CHECK(none.locations.empty());
    CHECK(none.memoryBytes == 0);
}

TEST_CASE("CallingConvention.hostConventionFollowsTheAbiRow")
{
    // (amended for P6, 2026-11-26: F2) A row lookup per (architecture, ABI), whatever the building host is.
    auto const x64 = [](OperatingSystemAbi abi) {
        return TargetDescription { .architecture = Architecture::X64,
                                   .abi = abi,
                                   .features = CpuFeatures {} };
    };
    CHECK(hostConvention(x64(OperatingSystemAbi::SystemV)) == Convention::SystemV);
    CHECK(hostConvention(x64(OperatingSystemAbi::Windows)) == Convention::Windows64);
    CHECK(hostConvention(x64(OperatingSystemAbi::Apple)) == Convention::SystemV);
    // CPU features choose no convention.
    auto withFeatures = x64(OperatingSystemAbi::Windows);
    withFeatures.features = CpuFeatures {}.with(isoline::compiler::machine::CpuFeature::Avx2);
    CHECK(hostConvention(withFeatures) == Convention::Windows64);
    // arm64 has no backend yet: no row (P6 appends Aapcs64's).
    CHECK(hasHostConvention(x64(OperatingSystemAbi::Apple)));
    CHECK_FALSE(hasHostConvention(TargetDescription {
        .architecture = Architecture::Arm64, .abi = OperatingSystemAbi::SystemV, .features = {} }));
}
