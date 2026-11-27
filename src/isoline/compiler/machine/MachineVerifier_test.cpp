// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <functional>
#include <memory_resource>
#include <tuple>

#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/MachineVerifier.hpp>
#include <isoline/compiler/machine/testing/FakeTarget.hpp>

using isoline::compiler::CompileErrorCode;
using isoline::compiler::machine::EdgeTo;
using isoline::compiler::machine::Hotness;
using isoline::compiler::machine::Immediate;
using isoline::compiler::machine::IndexExtension;
using isoline::compiler::machine::MachineFunction;
using isoline::compiler::machine::MachineOperand;
using isoline::compiler::machine::MemoryOperand;
using isoline::compiler::machine::OperandRole;
using isoline::compiler::machine::PhysicalRegister;
using isoline::compiler::machine::RegisterClass;
using isoline::compiler::machine::RegisterName;
using isoline::compiler::machine::RegisterOperand;
using isoline::compiler::machine::verifyMachineFunction;
using isoline::compiler::machine::testing::def;
using isoline::compiler::machine::testing::FakeOpcode;
using isoline::compiler::machine::testing::FakeTarget;
using isoline::compiler::machine::testing::opcodeOf;
using isoline::compiler::machine::testing::terminate;
using isoline::compiler::machine::testing::use;

namespace
{

/// Whether the verifier refuses the function @p build makes in its entry block, before a return.
bool refuses(std::function<void(MachineFunction&, isoline::compiler::machine::BlockId)> const& build)
{
    std::pmr::monotonic_buffer_resource memory;
    auto function = MachineFunction { memory };
    auto const entry = function.createBlock(Hotness::Normal);
    build(function, entry);
    auto const verified = verifyMachineFunction(function, FakeTarget {});
    if (verified)
        return false;
    CHECK(verified.error().code == CompileErrorCode::InternalVerificationFailed);
    CHECK(verified.error().pass == "machine-verifier");
    return true;
}

} // namespace

TEST_CASE("MachineVerifier.acceptsWellFormedFunctions")
{
    CHECK_FALSE(refuses([](MachineFunction& function, auto entry) {
        auto const base = function.createVirtualRegister(RegisterClass::General);
        auto const loaded = function.createVirtualRegister(RegisterClass::General);
        auto const next = function.createBlock(Hotness::Normal);
        auto const parameter = function.createVirtualRegister(RegisterClass::General);
        std::ignore =
            function.append(entry,
                            opcodeOf(FakeOpcode::Constant),
                            std::to_array<MachineOperand>({ def(function, base), Immediate { 1 } }));
        std::ignore = function.append(
            entry,
            opcodeOf(FakeOpcode::Load),
            std::to_array<MachineOperand>({ def(function, loaded),
                                            MemoryOperand { .base = function.name(base),
                                                            .index = function.name(base),
                                                            .scale = 4,
                                                            .displacement = 0,
                                                            .extension = IndexExtension::ZeroExtend32 } }));
        auto const arguments = std::array { loaded };
        terminate(function, entry, std::array { EdgeTo { .target = next, .arguments = arguments } });
        function.setParameters(next, std::array { parameter });
        // A physical register at a fixed position, the fixed one.
        auto const g0 = PhysicalRegister { .registerClass = RegisterClass::General, .number = 0 };
        // The selector's copy into it, in the block that reads it.
        std::ignore =
            function.append(next,
                            opcodeOf(FakeOpcode::Copy),
                            std::to_array<MachineOperand>(
                                { RegisterOperand { .name = RegisterName::of(g0), .role = OperandRole::Def },
                                  use(function, parameter) }));
        std::ignore = function.append(next,
                                      opcodeOf(FakeOpcode::Return),
                                      std::to_array<MachineOperand>({ RegisterOperand {
                                          .name = RegisterName::of(g0), .role = OperandRole::Use } }));
        function.setEdges(next, {});
    }));
}

TEST_CASE("MachineVerifier.refusesOperandsAgainstTheirRows")
{
    // A role the row does not give.
    CHECK(refuses([](MachineFunction& function, auto entry) {
        auto const v = function.createVirtualRegister(RegisterClass::General);
        std::ignore = function.append(entry,
                                      opcodeOf(FakeOpcode::Constant),
                                      std::to_array<MachineOperand>({ use(function, v), Immediate { 1 } }));
    }));
    // A class the row does not give.
    CHECK(refuses([](MachineFunction& function, auto entry) {
        auto const x = function.createVirtualRegister(RegisterClass::Float);
        std::ignore = function.append(entry,
                                      opcodeOf(FakeOpcode::Constant),
                                      std::to_array<MachineOperand>({ def(function, x), Immediate { 1 } }));
    }));
    // A physical register other than the fixed one.
    CHECK(refuses([](MachineFunction& function, auto entry) {
        auto const g1 = PhysicalRegister { .registerClass = RegisterClass::General, .number = 1 };
        std::ignore = function.append(entry,
                                      opcodeOf(FakeOpcode::Return),
                                      std::to_array<MachineOperand>({ RegisterOperand {
                                          .name = RegisterName::of(g1), .role = OperandRole::Use } }));
    }));
    // A memory operand where the row takes none.
    CHECK(refuses([](MachineFunction& function, auto entry) {
        auto const v = function.createVirtualRegister(RegisterClass::General);
        auto const w = function.createVirtualRegister(RegisterClass::General);
        std::ignore = function.append(
            entry,
            opcodeOf(FakeOpcode::Copy),
            std::to_array<MachineOperand>({ def(function, w), MemoryOperand { .base = function.name(v) } }));
    }));
}

TEST_CASE("MachineVerifier.refusesEdgesAgainstTheirTargets")
{
    // One argument too few.
    CHECK(refuses([](MachineFunction& function, auto entry) {
        auto const next = function.createBlock(Hotness::Normal);
        function.setParameters(next, std::array { function.createVirtualRegister(RegisterClass::General) });
        terminate(function, entry, std::array { EdgeTo { .target = next, .arguments = {} } });
        terminate(function, next, {});
    }));
    // An argument of the wrong class.
    CHECK(refuses([](MachineFunction& function, auto entry) {
        auto const next = function.createBlock(Hotness::Normal);
        auto const x = function.createVirtualRegister(RegisterClass::Float);
        function.setParameters(next, std::array { function.createVirtualRegister(RegisterClass::General) });
        auto const arguments = std::array { x };
        terminate(function, entry, std::array { EdgeTo { .target = next, .arguments = arguments } });
        terminate(function, next, {});
    }));
}

TEST_CASE("MachineVerifier.refusesARedefinitionInAnotherBlock")
{
    CHECK(refuses([](MachineFunction& function, auto entry) {
        auto const next = function.createBlock(Hotness::Normal);
        auto const v = function.createVirtualRegister(RegisterClass::General);
        std::ignore = function.append(entry,
                                      opcodeOf(FakeOpcode::Constant),
                                      std::to_array<MachineOperand>({ def(function, v), Immediate { 1 } }));
        terminate(function, entry, std::array { EdgeTo { .target = next, .arguments = {} } });
        std::ignore = function.append(next,
                                      opcodeOf(FakeOpcode::Constant),
                                      std::to_array<MachineOperand>({ def(function, v), Immediate { 2 } }));
        terminate(function, next, {});
    }));
}

TEST_CASE("MachineVerifier.refusesAPhysicalRegisterAcrossBlocks")
{
    auto const g0 = PhysicalRegister { .registerClass = RegisterClass::General, .number = 0 };
    auto const physical = [&](OperandRole role) {
        return MachineOperand { RegisterOperand { .name = RegisterName::of(g0), .role = role } };
    };
    // g0 written in the entry block and read in the next: no layer below selection carries it across the
    // edge.
    CHECK(refuses([&](MachineFunction& function, auto entry) {
        auto const v = function.createVirtualRegister(RegisterClass::General);
        auto const next = function.createBlock(Hotness::Normal);
        std::ignore = function.append(entry,
                                      opcodeOf(FakeOpcode::Constant),
                                      std::to_array<MachineOperand>({ def(function, v), Immediate { 1 } }));
        std::ignore =
            function.append(entry,
                            opcodeOf(FakeOpcode::Copy),
                            std::to_array<MachineOperand>({ physical(OperandRole::Def), use(function, v) }));
        terminate(function, entry, std::array { EdgeTo { .target = next } });
        std::ignore = function.append(next,
                                      opcodeOf(FakeOpcode::Return),
                                      std::to_array<MachineOperand>({ physical(OperandRole::Use) }));
        function.setEdges(next, {});
    }));
    // So is a memory operand's physical base.
    CHECK(refuses([&](MachineFunction& function, auto entry) {
        auto const v = function.createVirtualRegister(RegisterClass::General);
        auto const next = function.createBlock(Hotness::Normal);
        terminate(function, entry, std::array { EdgeTo { .target = next } });
        std::ignore =
            function.append(next,
                            opcodeOf(FakeOpcode::Load),
                            std::to_array<MachineOperand>(
                                { def(function, v), MemoryOperand { .base = RegisterName::of(g0) } }));
        terminate(function, next, {}, v);
    }));
}
