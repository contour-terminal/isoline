// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory_resource>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/machine/CallingConvention.hpp>
#include <isoline/compiler/machine/FrameRule.hpp>
#include <isoline/compiler/machine/Registers.hpp>
#include <isoline/compiler/machine/Target.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>

using isoline::compiler::intermediate::Type;
using isoline::compiler::machine::assignArguments;
using isoline::compiler::machine::assignCall;
using isoline::compiler::machine::Convention;
using isoline::compiler::machine::PhysicalRegister;
using isoline::compiler::machine::RegisterClass;
using isoline::compiler::machine::ResultArea;
using isoline::compiler::machine::StackOffset;
using isoline::compiler::machine::ValueLocation;
using isoline::compiler::x86_64::AllocatableGeneral;
using isoline::compiler::x86_64::conventionRow;
using isoline::compiler::x86_64::Gpr;
using isoline::compiler::x86_64::HostBoundaryFunction;
using isoline::compiler::x86_64::physical;
using isoline::compiler::x86_64::RegisterRole;
using isoline::compiler::x86_64::registerRow;
using isoline::compiler::x86_64::RegisterTable;
using isoline::compiler::x86_64::X64FrameRule;
using isoline::compiler::x86_64::X64StackRule;
using isoline::compiler::x86_64::Xmm;

namespace
{

ValueLocation at(Gpr reg)
{
    return physical(reg);
}

ValueLocation at(Xmm reg)
{
    return physical(reg);
}

ValueLocation stack(uint32_t bytes)
{
    return StackOffset { .bytes = bytes };
}

std::vector<ValueLocation> locationsOf(isoline::compiler::machine::Assignment const& assignment)
{
    return { assignment.locations.begin(), assignment.locations.end() };
}

/// Shapes of functions the host-call boundary allows or refuses.
struct TwoWords
{
    uint64_t first;
    uint64_t second;
};

struct OneDouble
{
    double value;
};

/// A member function's pointer: Microsoft x64 passes its hidden result pointer after `this`.
using MemberStub = TwoWords (TwoWords::*)(uint64_t) noexcept;

using TwoWordsStub = TwoWords (*)(void*, uint32_t) noexcept;
using StatusStub = uint32_t (*)(void*, uint64_t, int64_t) noexcept;
using OneDoubleStub = OneDouble (*)(void*) noexcept;
using ScalarDoubleStub = double (*)(void*) noexcept;
using TakesDoubleStub = uint32_t (*)(void*, double) noexcept;
using TakesByteStub = uint32_t (*)(void*, uint8_t) noexcept;

} // namespace

TEST_CASE("Registers.pinsTheContextRegisters")
{
    // rsp and rbp are the stack and frame pointers, r15 the ExecutionState*, r14 the InstanceContext*; the 12
    // others are allocatable, and no pinned register is in the allocation order.
    CHECK(registerRow(physical(Gpr::Rsp)).role == RegisterRole::StackPointer);
    CHECK(registerRow(physical(Gpr::Rbp)).role == RegisterRole::FramePointer);
    CHECK(registerRow(physical(Gpr::R15)).role == RegisterRole::ExecutionState);
    CHECK(registerRow(physical(Gpr::R14)).role == RegisterRole::InstanceContext);
    CHECK(registerRow(physical(Gpr::R9)).name == "r9");
    CHECK(registerRow(physical(Xmm::Xmm13)).name == "xmm13");
    auto allocatable = 0U;
    for (auto const& row: RegisterTable)
        if (row.role == RegisterRole::Allocatable)
            ++allocatable;
    CHECK(allocatable == 12 + 16);
    for (auto const reg: AllocatableGeneral)
        CHECK(registerRow(reg).role == RegisterRole::Allocatable);
    // The internal convention preserves the pinned registers: a callee keeps its caller's context.
    for (auto const reg: { Gpr::Rbp, Gpr::R14, Gpr::R15 })
        CHECK(conventionRow(Convention::WasmInternal).calleeSaved.contains(physical(reg)));
}

TEST_CASE("Registers.frameAndStackRules")
{
    CHECK(X64FrameRule
          == isoline::compiler::machine::FrameRule { .bytesPerStackValue = 32, .frameOverhead = 256 });
    CHECK(X64StackRule.alignment == 16);
    CHECK(X64StackRule.returnAddressBytes == 8);
    CHECK(X64StackRule.framePointerBytes == 8);
    CHECK(X64StackRule.savedRegisterBytes[static_cast<std::size_t>(RegisterClass::Float)] == 16);
}

TEST_CASE("CallingConvention.assignsInOrderOnX64")
{
    // System V (R6: moved beside the rows): each class counts on its own, rdi, xmm0, rsi, …; past six
    // integer and eight float registers, the stack in 8-byte slots, its area rounded to 16 bytes.
    std::pmr::monotonic_buffer_resource memory;
    auto const systemV = assignArguments(conventionRow(Convention::SystemV),
                                         std::to_array({ Type::I32, Type::F64, Type::Ref, Type::F32 }),
                                         memory);
    CHECK(locationsOf(systemV)
          == std::vector<ValueLocation> { at(Gpr::Rdi), at(Xmm::Xmm0), at(Gpr::Rsi), at(Xmm::Xmm1) });
    CHECK(systemV.memoryBytes == 0);

    auto const many =
        assignArguments(conventionRow(Convention::SystemV), std::vector<Type>(7, Type::I64), memory);
    CHECK(locationsOf(many)
          == std::vector<ValueLocation> {
              at(Gpr::Rdi), at(Gpr::Rsi), at(Gpr::Rdx), at(Gpr::Rcx), at(Gpr::R8), at(Gpr::R9), stack(0) });
    CHECK(many.memoryBytes == 16);

    // Microsoft x64: shared positions, rcx, xmm1, r8, xmm3, then the stack above 32 bytes of shadow space.
    auto const windows =
        assignArguments(conventionRow(Convention::Windows64),
                        std::to_array({ Type::I32, Type::F64, Type::I64, Type::F32, Type::Ref }),
                        memory);
    CHECK(
        locationsOf(windows)
        == std::vector<ValueLocation> { at(Gpr::Rcx), at(Xmm::Xmm1), at(Gpr::R8), at(Xmm::Xmm3), stack(32) });
    CHECK(windows.memoryBytes == 48);

    // The internal convention passes as System V does.
    auto const internal = assignArguments(
        conventionRow(Convention::WasmInternal), std::to_array({ Type::I64, Type::F32, Type::I32 }), memory);
    CHECK(locationsOf(internal) == std::vector<ValueLocation> { at(Gpr::Rdi), at(Xmm::Xmm0), at(Gpr::Rsi) });
}

TEST_CASE("CallingConvention.aggregateResultsOnTheHostRows")
{
    std::pmr::monotonic_buffer_resource memory;
    auto const twoWordTypes = std::to_array({ Type::I64, Type::I64 });
    // System V returns two words in rax and rdx.
    auto const systemV = assignCall(conventionRow(Convention::SystemV), {}, twoWordTypes, memory);
    CHECK(locationsOf(systemV.results) == std::vector<ValueLocation> { at(Gpr::Rax), at(Gpr::Rdx) });
    CHECK(systemV.results.area == ResultArea::None);
    // Microsoft x64 returns them through a hidden pointer in rcx, given back in rax; arguments shift by one.
    auto const windows =
        assignCall(conventionRow(Convention::Windows64), std::to_array({ Type::I64 }), twoWordTypes, memory);
    CHECK(windows.results.area == ResultArea::HiddenPointer);
    REQUIRE(windows.resultPointer.has_value());
    CHECK(*windows.resultPointer == at(Gpr::Rcx));
    REQUIRE(windows.returnedPointer.has_value());
    CHECK(*windows.returnedPointer == physical(Gpr::Rax));
    CHECK(locationsOf(windows.arguments) == std::vector<ValueLocation> { at(Gpr::Rcx), at(Gpr::Rdx) });
    // The internal convention splits: two words in rax and rdx, the third in the caller's area.
    auto const internal = assignCall(conventionRow(Convention::WasmInternal),
                                     {},
                                     std::to_array({ Type::I64, Type::I32, Type::Ref }),
                                     memory);
    CHECK(locationsOf(internal.results)
          == std::vector<ValueLocation> { at(Gpr::Rax), at(Gpr::Rdx), stack(0) });
    CHECK(internal.results.area == ResultArea::CallerArea);
}

TEST_CASE("Registers.hostBoundaryFunctions")
{
    // Non-member functions of 32- and 64-bit integers and pointers, returning such a word or a struct of
    // 64-bit words: what AggregateResults::HiddenPointerBeyond describes (ledger carry to T11).
    STATIC_CHECK(HostBoundaryFunction<TwoWordsStub>);
    STATIC_CHECK(HostBoundaryFunction<StatusStub>);
    // A member function (Microsoft x64 passes its hidden pointer after `this`), a float result or argument, a
    // struct of a double (returned in rax by Microsoft x64), a narrow argument: refused.
    STATIC_CHECK_FALSE(HostBoundaryFunction<MemberStub>);
    STATIC_CHECK_FALSE(HostBoundaryFunction<OneDoubleStub>);
    STATIC_CHECK_FALSE(HostBoundaryFunction<ScalarDoubleStub>);
    STATIC_CHECK_FALSE(HostBoundaryFunction<TakesDoubleStub>);
    STATIC_CHECK_FALSE(HostBoundaryFunction<TakesByteStub>);
}

TEST_CASE("Registers.allocationOrderAndConventionsAtRunTime")
{
    // The static_asserts' predicates, at run time, and on orders that break them.
    using isoline::compiler::x86_64::AllocatableFloat;
    using isoline::compiler::x86_64::isAllocationOrderOf;
    CHECK(isAllocationOrderOf(AllocatableGeneral, RegisterClass::General));
    CHECK(isAllocationOrderOf(AllocatableFloat, RegisterClass::Float));
    auto const withRsp = std::to_array({ physical(Gpr::Rax), physical(Gpr::Rsp) });
    CHECK_FALSE(isAllocationOrderOf(withRsp, RegisterClass::General)); // a pinned register
    auto const twice = std::to_array({ physical(Gpr::Rax), physical(Gpr::Rax) });
    CHECK_FALSE(isAllocationOrderOf(twice, RegisterClass::General));
    CHECK_FALSE(isAllocationOrderOf(std::span { AllocatableGeneral }.first(11), RegisterClass::General));
    CHECK_FALSE(isAllocationOrderOf(AllocatableFloat, RegisterClass::General)); // the other class
    // A number past the register file, refused before its row is looked up.
    auto const pastTheFile = std::to_array({ physical(Gpr::Rax),
                                             isoline::compiler::machine::PhysicalRegister {
                                                 .registerClass = RegisterClass::General, .number = 16 } });
    CHECK_FALSE(isAllocationOrderOf(pastTheFile, RegisterClass::General));
    for (auto const convention: { Convention::WasmInternal, Convention::SystemV, Convention::Windows64 })
        CHECK(conventionRow(convention).convention == convention);
    CHECK(registerRow(physical(Xmm::Xmm0)).role == RegisterRole::Allocatable);
}

TEST_CASE("Registers.wordAggregatesAtRunTime")
{
    // HostBoundaryResult's aggregate probe, at run time: two uint64_t words, and not one double.
    using isoline::compiler::x86_64::detail::isMadeOfWords;
    CHECK(isMadeOfWords<TwoWords>(std::make_index_sequence<2> {}));
    CHECK_FALSE(isMadeOfWords<OneDouble>(std::make_index_sequence<1> {}));
}

TEST_CASE("Registers.callersSaveExactlyWhatTheirConventionClobbers")
{
    // Each convention's clobbers, as callerSavedRegisters() derives them over the allocatable files: every
    // register it does not preserve, General then Float, in the allocator's order.
    using isoline::compiler::machine::forEachCallerSaved;
    using isoline::compiler::x86_64::AllocatableFloat;
    auto const clobbersOf = [](Convention convention) {
        auto registers = std::vector<PhysicalRegister> {};
        forEachCallerSaved(conventionRow(convention),
                           AllocatableGeneral,
                           AllocatableFloat,
                           [&](PhysicalRegister reg) { registers.push_back(reg); });
        return registers;
    };
    auto const xmm = [](uint8_t count) {
        auto registers = std::vector<PhysicalRegister> {};
        for (auto const number: std::views::iota(uint8_t { 0 }, count))
            registers.push_back(physical(static_cast<Xmm>(number)));
        return registers;
    };
    auto systemV =
        std::vector<PhysicalRegister> { physical(Gpr::Rax), physical(Gpr::Rcx), physical(Gpr::Rdx),
                                        physical(Gpr::Rsi), physical(Gpr::Rdi), physical(Gpr::R8),
                                        physical(Gpr::R9),  physical(Gpr::R10), physical(Gpr::R11) };
    std::ranges::copy(xmm(16), std::back_inserter(systemV));
    CHECK(clobbersOf(Convention::SystemV) == systemV);
    CHECK(clobbersOf(Convention::WasmInternal) == systemV);
    auto windows = std::vector<PhysicalRegister> { physical(Gpr::Rax), physical(Gpr::Rcx), physical(Gpr::Rdx),
                                                   physical(Gpr::R8),  physical(Gpr::R9),  physical(Gpr::R10),
                                                   physical(Gpr::R11) };
    std::ranges::copy(xmm(6), std::back_inserter(windows));
    CHECK(clobbersOf(Convention::Windows64) == windows);
}

TEST_CASE("Contract.Registers.registerRowOfARegister", "[.contract]")
{
    // A number past the file is no register: registerRow() refuses it rather than wrapping to another one.
    isoline::runtime::testing::endAtContractViolation();
    std::ignore = registerRow(PhysicalRegister { .registerClass = RegisterClass::General, .number = 16 });
    FAIL("SURVIVED: a row of General register 16");
}
