// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory_resource>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/machine/FrameRule.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/Registers.hpp>
#include <isoline/compiler/machine/testing/FakeTarget.hpp>

using isoline::compiler::CompileErrorCode;
using isoline::compiler::machine::checkFrame;
using isoline::compiler::machine::checkFrameSize;
using isoline::compiler::machine::ConstantOperand;
using isoline::compiler::machine::controlFlowGraphOf;
using isoline::compiler::machine::EdgeTo;
using isoline::compiler::machine::FrameLayout;
using isoline::compiler::machine::FrameRule;
using isoline::compiler::machine::Hotness;
using isoline::compiler::machine::Immediate;
using isoline::compiler::machine::MachineFunction;
using isoline::compiler::machine::MachineOperand;
using isoline::compiler::machine::maximumFrameBytes;
using isoline::compiler::machine::MemoryOperand;
using isoline::compiler::machine::OperandRole;
using isoline::compiler::machine::PhysicalRegister;
using isoline::compiler::machine::RegisterClass;
using isoline::compiler::machine::RegisterKind;
using isoline::compiler::machine::RegisterName;
using isoline::compiler::machine::RegisterOperand;
using isoline::compiler::machine::RegisterSet;
using isoline::compiler::machine::testing::def;
using isoline::compiler::machine::testing::FakeConstraintTable;
using isoline::compiler::machine::testing::FakeOpcode;
using isoline::compiler::machine::testing::FakeTarget;
using isoline::compiler::machine::testing::label;
using isoline::compiler::machine::testing::opcodeOf;
using isoline::compiler::machine::testing::use;

TEST_CASE("MachineFunction.blocksInstructionsAndEdges")
{
    std::pmr::monotonic_buffer_resource memory;
    auto function = MachineFunction { memory };
    auto const entry = function.createBlock(Hotness::Normal);
    auto const join = function.createBlock(Hotness::Cold);
    auto const a = function.createVirtualRegister(RegisterClass::General);
    auto const x = function.createVirtualRegister(RegisterClass::Float);
    auto const p = function.createVirtualRegister(RegisterClass::General);
    CHECK(function.virtualRegisterCount() == 3);
    CHECK(function.registerClass(x) == RegisterClass::Float);
    CHECK(function.name(a) == RegisterName::of(a, RegisterClass::General));

    auto const constant = function.addConstant(0x4009'21FB'5444'2D18); // pi
    auto const first = function.append(entry,
                                       opcodeOf(FakeOpcode::Constant),
                                       std::to_array<MachineOperand>({ def(function, a), Immediate { 7 } }));
    auto const load = function.append(
        entry,
        opcodeOf(FakeOpcode::Load),
        std::to_array<MachineOperand>({
            def(function, p),
            MemoryOperand { .base = function.name(a), .index = {}, .scale = 1, .displacement = -16 },
        }));
    auto const jump = function.append(
        entry, opcodeOf(FakeOpcode::Jump), std::to_array<MachineOperand>({ label(join), constant }));
    auto const arguments = std::to_array({ a, p });
    function.setEdges(entry, std::to_array({ EdgeTo { .target = join, .arguments = arguments } }));
    auto const parameters = std::to_array({ function.createVirtualRegister(RegisterClass::General),
                                            function.createVirtualRegister(RegisterClass::General) });
    function.setParameters(join, parameters);
    function.append(
        join, opcodeOf(FakeOpcode::Return), std::to_array<MachineOperand>({ use(function, parameters[0]) }));

    CHECK(function.blockCount() == 2);
    CHECK(function.block(join).hotness == Hotness::Cold);
    CHECK(std::vector(function.instructions(entry).begin(), function.instructions(entry).end())
          == std::vector { first, load, jump });
    CHECK(function.instructionCount() == 4);
    CHECK(function.instruction(load).opcode == opcodeOf(FakeOpcode::Load));
    CHECK(function.operands(first).size() == 2);
    CHECK(std::get<Immediate>(function.operands(first)[1]).value == 7);
    CHECK(std::get<MemoryOperand>(function.operands(load)[1]).displacement == -16);
    CHECK(std::get<ConstantOperand>(function.operands(jump)[1]) == constant);
    CHECK(function.constants().size() == 1);
    CHECK(function.constants()[constant.slot] == 0x4009'21FB'5444'2D18);
    REQUIRE(function.edges(entry).size() == 1);
    CHECK(function.edges(entry)[0].target == join);
    CHECK(std::vector(function.arguments(function.edges(entry)[0]).begin(),
                      function.arguments(function.edges(entry)[0]).end())
          == std::vector { a, p });
    CHECK(std::vector(function.parameters(join).begin(), function.parameters(join).end())
          == std::vector(parameters.begin(), parameters.end()));
    CHECK(function.edges(join).empty());
    CHECK(function.parameters(entry).empty());

    // The allocator rewrites operands in place.
    auto const rax = PhysicalRegister { .registerClass = RegisterClass::General, .number = 0 };
    function.operands(first)[0] = RegisterOperand { .name = RegisterName::of(rax), .role = OperandRole::Def };
    auto const& rewritten = std::get<RegisterOperand>(function.operands(first)[0]);
    CHECK(rewritten.name.kind == RegisterKind::Physical);
    CHECK(rewritten.name.physicalRegister() == rax);

    // A function moves with its arena.
    auto moved = std::move(function);
    CHECK(&moved.memory() == &memory);
    CHECK(moved.blockCount() == 2);

    // The control-flow graph follows the edges.
    auto const graph = controlFlowGraphOf(moved, memory);
    CHECK(graph.blockCount() == 2);
    REQUIRE(graph.successors(entry).size() == 1);
    CHECK(graph.successors(entry)[0] == join);
    REQUIRE(graph.predecessors(join).size() == 1);
    CHECK(graph.predecessors(join)[0] == entry);
}

TEST_CASE("MachineFunction.registerSets")
{
    auto const rbx = PhysicalRegister { .registerClass = RegisterClass::General, .number = 3 };
    auto const xmm6 = PhysicalRegister { .registerClass = RegisterClass::Float, .number = 6 };
    auto const set = RegisterSet::of({ rbx, xmm6, rbx });
    CHECK(set.contains(rbx));
    CHECK(set.contains(xmm6));
    CHECK_FALSE(set.contains(PhysicalRegister { .registerClass = RegisterClass::Float, .number = 3 }));
    CHECK(set.countIn(RegisterClass::General) == 1);
    CHECK(set.countIn(RegisterClass::Float) == 1);
    CHECK(RegisterSet {}.with(rbx).with(xmm6) == set);
    CHECK(RegisterSet {}.countIn(RegisterClass::General) == 0);
    auto const highest = PhysicalRegister { .registerClass = RegisterClass::Float, .number = 63 };
    CHECK(RegisterSet {}.with(highest).contains(highest));
    // Usable in the targets' constant rows.
    STATIC_CHECK(
        RegisterSet::of({ PhysicalRegister { .registerClass = RegisterClass::General, .number = 3 } })
            .countIn(RegisterClass::General)
        == 1);
}

TEST_CASE("MachineFunction.frameRuleBoundsTheFrame")
{
    // x86-64's row, {32, 256}: 32 bytes per stack value plus 256.
    constexpr auto Rule = FrameRule { .bytesPerStackValue = 32, .frameOverhead = 256 };
    STATIC_CHECK(maximumFrameBytes(Rule, 0) == 256);
    STATIC_CHECK(maximumFrameBytes(Rule, 1'000) == 32'256);
    CHECK(checkFrameSize(Rule, 1'000, 32'256).has_value());
    auto const refused = checkFrameSize(Rule, 1'000, 32'257);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == CompileErrorCode::FrameTooLarge);
    CHECK(refused.error().requested == 32'257);
    CHECK(refused.error().limit == 32'256);

    // A guest-sized cost saturates rather than wraps, so no frame passes by overflow.
    constexpr auto Largest = std::numeric_limits<uint64_t>::max();
    STATIC_CHECK(maximumFrameBytes(Rule, Largest / 16) == Largest);
    CHECK(checkFrameSize(Rule, Largest / 16, Largest).has_value());
    // A tight test-only rule, {1, 0}, refuses any frame beyond one byte per value.
    CHECK_FALSE(checkFrameSize(FrameRule { .bytesPerStackValue = 1, .frameOverhead = 0 }, 8, 9).has_value());
}

TEST_CASE("MachineFunction.framesKeepTheStackAtEveryCall")
{
    // The frame's size, return address included, is a multiple of 16: every call (a Wasm callee, a stub,
    // PollSlow) leaves from the frame's bottom aligned. The outgoing area holds the host convention's shadow
    // space, and the spills lie above it.
    constexpr auto Rule = FrameRule { .bytesPerStackValue = 32, .frameOverhead = 256 };
    auto const& systemV = isoline::compiler::machine::testing::FakeHostConvention;
    auto const stack = isoline::compiler::machine::testing::FakeStackRule;
    auto const frame = [](uint64_t total, uint64_t outgoing, uint64_t spillOffset, uint64_t spills) {
        return FrameLayout { .totalBytes = total,
                             .calleeSaved = {},
                             .outgoingBytes = outgoing,
                             .spillOffset = spillOffset,
                             .spillBytes = spills };
    };
    auto const isBroken = [](std::expected<void, isoline::compiler::CompileError> const& checked) {
        return !checked.has_value() && checked.error().code == CompileErrorCode::InternalVerificationFailed
               && checked.error().pass == "frame";
    };
    CHECK(checkFrame(Rule, stack, systemV, 10, frame(32, 0, 0, 16)).has_value());
    // A frame of 8 mod 16 misaligns every call on System V.
    CHECK(isBroken(checkFrame(Rule, stack, systemV, 10, frame(24, 0, 0, 8))));
    // Spills that reach into the return address, or are larger than the frame below it.
    CHECK(isBroken(checkFrame(Rule, stack, systemV, 10, frame(32, 0, 8, 24))));
    CHECK(isBroken(checkFrame(Rule, stack, systemV, 10, frame(32, 0, 0, 32))));
    // A frame smaller than what the call pushed.
    CHECK(isBroken(checkFrame(Rule, stack, systemV, 10, frame(0, 0, 0, 0))));
    // A target without an alignment rule names none.
    auto unaligned = systemV;
    unaligned.stackAlignment = 0;
    CHECK(
        isBroken(checkFrame(Rule,
                            isoline::compiler::machine::StackRule { .alignment = 0, .returnAddressBytes = 8 },
                            unaligned,
                            10,
                            frame(32, 0, 0, 16))));
    // The size bound still comes first, as FrameTooLarge.
    auto const tooLarge = checkFrame(Rule, stack, systemV, 0, frame(272, 0, 0, 16));
    REQUIRE_FALSE(tooLarge.has_value());
    CHECK(tooLarge.error().code == CompileErrorCode::FrameTooLarge);

    // Microsoft x64-shaped: 32 bytes of shadow space, which the callee may write.
    auto windows = systemV;
    windows.shadowSpaceBytes = 32;
    CHECK(isBroken(checkFrame(Rule, stack, windows, 10, frame(48, 0, 0, 16))));   // no shadow space
    CHECK(isBroken(checkFrame(Rule, stack, windows, 10, frame(64, 32, 16, 16)))); // spills inside it
    CHECK(checkFrame(Rule, stack, windows, 10, frame(64, 32, 32, 16)).has_value());
}

TEST_CASE("MachineFunction.fakeTargetRows")
{
    // FakeTarget is what Task 10's allocator tests run on: two registers per class, and rows of every
    // constraint kind.
    auto const target = FakeTarget {};
    CHECK(target.allocatable(RegisterClass::General).size() == 2);
    CHECK(target.allocatable(RegisterClass::Float).size() == 2);
    CHECK(target.frameRule() == isoline::compiler::machine::testing::FakeFrameRule);
    CHECK(target.stackRule() == isoline::compiler::machine::testing::FakeStackRule);
    CHECK(target.conventionRow(isoline::compiler::machine::Convention::WasmInternal).convention
          == isoline::compiler::machine::Convention::WasmInternal);
    CHECK(target.conventionRow(isoline::compiler::machine::Convention::SystemV).shadowSpaceBytes == 0);
    CHECK(FakeTarget { FrameRule { .bytesPerStackValue = 1, .frameOverhead = 0 } }.frameRule().frameOverhead
          == 0);
    auto const divide = target.constraints(opcodeOf(FakeOpcode::Divide));
    REQUIRE(divide.size() == 3);
    CHECK(divide[0].fixed == 0);
    CHECK(divide[0].role == OperandRole::Def);
    auto const shift = target.constraints(opcodeOf(FakeOpcode::Shift));
    CHECK(shift[0].role == OperandRole::UseDef);
    CHECK(shift[1].fixed == 1);
    CHECK(target.constraints(opcodeOf(FakeOpcode::EarlyClobber))[0].role == OperandRole::Early);
    CHECK(target.constraints(opcodeOf(FakeOpcode::Add))[2].memory
          == isoline::compiler::machine::AllowsMemory::Yes);
    CHECK(target.constraints(opcodeOf(FakeOpcode::Call)).size() == 4);
    CHECK(FakeConstraintTable.size() == static_cast<std::size_t>(FakeOpcode::Return) + 1);
}
