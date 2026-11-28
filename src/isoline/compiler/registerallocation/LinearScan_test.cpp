// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/CompileLimits.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/machine/CallingConvention.hpp>
#include <isoline/compiler/machine/FrameRule.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/testing/FakeTarget.hpp>
#include <isoline/compiler/registerallocation/LinearScan.hpp>
#include <isoline/compiler/registerallocation/testing/AssignmentChecker.hpp>
#include <isoline/compiler/x86_64/testing/X64FakeTarget.hpp>

using isoline::compiler::CompileCancellation;
using isoline::compiler::CompileError;
using isoline::compiler::CompileErrorCode;
using isoline::compiler::CompileLimits;
using isoline::compiler::machine::BlockId;
using isoline::compiler::machine::Convention;
using isoline::compiler::machine::EdgeTo;
using isoline::compiler::machine::FrameLayout;
using isoline::compiler::machine::FrameRule;
using isoline::compiler::machine::Hotness;
using isoline::compiler::machine::Immediate;
using isoline::compiler::machine::LocationKind;
using isoline::compiler::machine::MachineFunction;
using isoline::compiler::machine::MachineOperand;
using isoline::compiler::machine::MemoryOperand;
using isoline::compiler::machine::Move;
using isoline::compiler::machine::OperandRole;
using isoline::compiler::machine::PhysicalRegister;
using isoline::compiler::machine::RegisterClass;
using isoline::compiler::machine::RegisterKind;
using isoline::compiler::machine::RegisterName;
using isoline::compiler::machine::RegisterOperand;
using isoline::compiler::machine::VirtualRegister;
using isoline::compiler::machine::testing::def;
using isoline::compiler::machine::testing::early;
using isoline::compiler::machine::testing::FakeFrameRule;
using isoline::compiler::machine::testing::FakeOpcode;
using isoline::compiler::machine::testing::FakeSaves;
using isoline::compiler::machine::testing::FakeTarget;
using isoline::compiler::machine::testing::opcodeOf;
using isoline::compiler::machine::testing::terminate;
using isoline::compiler::machine::testing::use;
using isoline::compiler::machine::testing::useDef;
using isoline::compiler::registerallocation::LinearScanAllocator;
using isoline::compiler::registerallocation::ScanWork;
using isoline::compiler::registerallocation::testing::AssignmentFaultKind;
using isoline::compiler::registerallocation::testing::checkAssignment;
using isoline::compiler::x86_64::testing::X64FakeTarget;
using isoline::compiler::x86_64::testing::X64WindowsFakeTarget;

namespace
{

/// General register @p number, as an operand's name.
RegisterName generalRegister(uint8_t number)
{
    return RegisterName::of(PhysicalRegister { .registerClass = RegisterClass::General, .number = number });
}

/// A physical operand of @p role naming @p reg.
MachineOperand physical(PhysicalRegister reg, OperandRole role)
{
    return RegisterOperand { .name = RegisterName::of(reg), .role = role };
}

/// The operands of a call under @p convention on @p target: a physical definition of every register it
/// clobbers, derived from the convention row (callerSavedRegisters()), never listed by hand.
std::vector<MachineOperand> clobbers(isoline::compiler::machine::ITarget const& target,
                                     Convention convention = Convention::WasmInternal)
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto operands = std::vector<MachineOperand> {};
    for (auto const reg: isoline::compiler::machine::callerSavedRegisters(target, convention, memory))
        operands.push_back(physical(reg, OperandRole::Def));
    return operands;
}

/// The target a Builder derives its calls' clobbers from when a test names none.
FakeTarget const defaultTarget {};

/// A builder of machine functions of the fake opcodes, on @p target's register file.
class Builder
{
  public:
    explicit Builder(MachineFunction& function,
                     isoline::compiler::machine::ITarget const& target = defaultTarget):
        _function { function }, _target { target }
    {
    }

    VirtualRegister general() { return _function.createVirtualRegister(RegisterClass::General); }
    VirtualRegister floating() { return _function.createVirtualRegister(RegisterClass::Float); }

    uint32_t constant(BlockId block, VirtualRegister reg, int64_t value = 0)
    {
        return append(block, FakeOpcode::Constant, { def(_function, reg), Immediate { value } });
    }

    uint32_t add(BlockId block, VirtualRegister result, VirtualRegister left, VirtualRegister right)
    {
        return append(
            block, FakeOpcode::Add, { def(_function, result), use(_function, left), use(_function, right) });
    }

    uint32_t constantFloat(BlockId block, VirtualRegister result, int64_t bits = 0)
    {
        return append(block, FakeOpcode::ConstantFloat, { def(_function, result), Immediate { bits } });
    }

    uint32_t copyFloat(BlockId block, VirtualRegister result, VirtualRegister source)
    {
        return append(block, FakeOpcode::CopyFloat, { def(_function, result), use(_function, source) });
    }

    /// Stores @p value at [@p displacement]: a stack argument, or the host call's scratch.
    uint32_t store(BlockId block, VirtualRegister value, int32_t displacement = 0)
    {
        return append(block,
                      FakeOpcode::Store,
                      { use(_function, value), MemoryOperand { .displacement = displacement } });
    }

    /// Loads @p result from [@p displacement].
    uint32_t load(BlockId block, VirtualRegister result, int32_t displacement = 0)
    {
        return append(block,
                      FakeOpcode::Load,
                      { def(_function, result), MemoryOperand { .displacement = displacement } });
    }

    /// A call that clobbers every register.
    uint32_t call(BlockId block)
    {
        auto const operands = clobbers(_target);
        return _function.append(block, opcodeOf(FakeOpcode::Call), operands);
    }

    /// An indirect call through @p target, its result @p result in g0, clobbering every other register.
    uint32_t callIndirect(BlockId block, VirtualRegister target, VirtualRegister result)
    {
        auto operands = clobbers(_target);
        operands[0] = def(_function, result);
        operands.insert(operands.begin(), use(_function, target));
        return _function.append(block, opcodeOf(FakeOpcode::CallIndirect), operands);
    }

    /// A poll, whose slow path calls the runtime: it clobbers the host convention's caller-saved registers.
    uint32_t poll(BlockId block)
    {
        auto const operands =
            clobbers(_target, isoline::compiler::machine::hostConvention(_target.description()));
        return _function.append(block, opcodeOf(FakeOpcode::Poll), operands);
    }

    uint32_t append(BlockId block, FakeOpcode opcode, std::initializer_list<MachineOperand> operands)
    {
        return _function.append(block, opcodeOf(opcode), std::span { operands.begin(), operands.size() });
    }

    /// Returns: Return of @p value.
    void ret(BlockId block, VirtualRegister value) { terminate(_function, block, {}, value); }

    void jump(BlockId block, BlockId target, std::initializer_list<VirtualRegister> arguments = {})
    {
        auto const edge = std::to_array<EdgeTo>(
            { EdgeTo { .target = target, .arguments = std::span { arguments.begin(), arguments.size() } } });
        terminate(_function, block, edge);
    }

    void jumpWith(BlockId block, BlockId target, std::span<VirtualRegister const> arguments)
    {
        auto const edge = std::to_array<EdgeTo>({ EdgeTo { .target = target, .arguments = arguments } });
        terminate(_function, block, edge);
    }

    void branch(BlockId block, VirtualRegister condition, BlockId taken, BlockId other)
    {
        auto const edges = std::to_array<EdgeTo>({ EdgeTo { .target = taken }, EdgeTo { .target = other } });
        terminate(_function, block, edges, condition);
    }

    /// Adds every value of @p values into one, in order, and stores each running sum: keeps them all live up
    /// to their use.
    VirtualRegister sum(BlockId block, std::span<VirtualRegister const> values)
    {
        auto total = values.front();
        for (auto const value: values.subspan(1))
        {
            auto const next = general();
            add(block, next, total, value);
            total = next;
        }
        return total;
    }

    MachineFunction& function() { return _function; }

    /// The target the function is built for.
    [[nodiscard]] isoline::compiler::machine::ITarget const& target() const { return _target; }

  private:
    MachineFunction& _function;
    isoline::compiler::machine::ITarget const& _target;
};

/// A function built by a test: built twice, once to keep as selected and once to allocate.
using Build = std::function<void(Builder&)>;

/// An allocation of a built function, and its check.
struct Allocation
{
    MachineFunction before;
    MachineFunction after;
    std::expected<FrameLayout, CompileError> frame;
};

Allocation allocateBuilt(Build const& build,
                         isoline::compiler::machine::ITarget const& target,
                         uint64_t frameCost,
                         std::pmr::memory_resource& memory,
                         CompileCancellation const& cancellation = CompileCancellation {})
{
    auto result =
        Allocation { .before = MachineFunction { memory }, .after = MachineFunction { memory }, .frame = {} };
    auto beforeBuilder = Builder { result.before, target };
    build(beforeBuilder);
    auto afterBuilder = Builder { result.after, target };
    build(afterBuilder);
    result.frame = LinearScanAllocator { CompileLimits {} }.allocate(
        result.after, target, frameCost, cancellation, memory);
    return result;
}

/// Allocates @p build on @p target and checks the result with the AssignmentChecker.
Allocation allocateAndCheck(Build const& build,
                            std::pmr::memory_resource& memory,
                            isoline::compiler::machine::ITarget const& target = defaultTarget,
                            uint64_t frameCost = 64)
{
    auto result = allocateBuilt(build, target, frameCost, memory);
    if (!result.frame)
        UNSCOPED_INFO(isoline::compiler::describe(result.frame.error()));
    REQUIRE(result.frame.has_value());
    auto const checked =
        checkAssignment(result.before, result.after, target, *result.frame, frameCost, memory);
    if (!checked)
    {
        INFO("fault " << static_cast<int>(checked.error().kind) << " at instruction "
                      << checked.error().instruction.value_or(~0U) << " operand "
                      << checked.error().operand.value_or(~0U));
        CHECK(checked.has_value());
    }
    return result;
}

/// How many spill slots a frame has.
uint64_t spillSlots(FrameLayout const& frame)
{
    return frame.spillBytes / 8;
}

/// The register an operand of an allocated instruction names (Require: a register operand).
RegisterName nameOf(MachineFunction const& function, uint32_t instruction, std::size_t operand)
{
    return std::get<RegisterOperand>(function.operands(instruction)[operand]).name;
}

/// Every move of an allocated function, in instruction order.
std::vector<Move> allMoves(MachineFunction const& function)
{
    auto moves = std::vector<Move> {};
    for (auto const instruction: std::views::iota(uint32_t { 0 }, function.instructionCount()))
        std::ranges::copy(function.movesBefore(instruction), std::back_inserter(moves));
    return moves;
}

/// A loop: entry → header(i, acc) → body → header, header → exit; @p pressure values live across the loop,
/// and a call in the body when @p withCall.
Build loopWith(uint32_t pressure, bool withCall)
{
    return [=](Builder& b) {
        auto& f = b.function();
        auto const entry = f.createBlock(Hotness::Normal);
        auto const header = f.createBlock(Hotness::Normal);
        auto const body = f.createBlock(Hotness::Normal);
        auto const exit = f.createBlock(Hotness::Normal);
        auto live = std::vector<VirtualRegister> {};
        for (auto const index: std::views::iota(uint32_t { 0 }, pressure))
        {
            live.push_back(b.general());
            b.constant(entry, live.back(), index);
        }
        auto const i0 = b.general();
        auto const acc0 = b.general();
        b.constant(entry, i0, 10);
        b.constant(entry, acc0, 0);
        b.jump(entry, header, { i0, acc0 });
        auto const i = b.general();
        auto const acc = b.general();
        auto const parameters = std::to_array({ i, acc });
        f.setParameters(header, parameters);
        b.branch(header, i, body, exit);
        auto const next = b.general();
        auto const accNext = b.general();
        b.add(body, next, i, acc);
        if (withCall)
            b.call(body);
        b.add(body, accNext, acc, next);
        b.jump(body, header, { next, accNext });
        live.push_back(acc);
        b.ret(exit, b.sum(exit, live));
    };
}

/// Runs @p test on FakeTarget's two registers per class and on x86-64's register file (Task 10's allocator
/// tests on the file they allocate for, ledger carry to T11), each in a section of its own. The test takes
/// the target as an ITarget: MSVC 19.52 fails with an internal compiler error on these bodies as generic
/// lambdas.
template <typename Test>
void onEachRegisterFile(Test const& test)
{
    SECTION("two registers per class")
    {
        test(FakeTarget {});
    }
    SECTION("x86-64's register file")
    {
        test(X64FakeTarget {});
    }
    SECTION("x86-64's register file under a Microsoft x64 host")
    {
        test(X64WindowsFakeTarget {});
    }
}

/// The shadow space of @p target's host convention: what every frame's outgoing area holds at least, as its
/// entry poll's slow path calls the host.
uint32_t hostShadowOf(isoline::compiler::machine::ITarget const& target)
{
    return target.conventionRow(isoline::compiler::machine::hostConvention(target.description()))
        .shadowSpaceBytes;
}

/// How many registers of @p registerClass @p target allocates.
uint32_t registerCount(isoline::compiler::machine::ITarget const& target, RegisterClass registerClass)
{
    return static_cast<uint32_t>(target.allocatable(registerClass).size());
}

} // namespace

TEST_CASE("LinearScan.noSpillsUnderPressure")
{
    onEachRegisterFile([&](isoline::compiler::machine::ITarget const& target) {
        auto memory = std::pmr::monotonic_buffer_resource {};
        // As many live values of each class as registers: none is spilled.
        auto const generals = registerCount(target, RegisterClass::General);
        auto const floats = registerCount(target, RegisterClass::Float);
        auto const allocation = allocateAndCheck(
            [&](Builder& b) {
                auto& f = b.function();
                auto const block = f.createBlock(Hotness::Normal);
                auto values = std::vector<VirtualRegister> {};
                for (auto const index: std::views::iota(0U, generals))
                {
                    values.push_back(b.general());
                    b.constant(block, values.back(), index);
                }
                auto floating = std::vector<VirtualRegister> {};
                for (auto const index: std::views::iota(0U, floats))
                {
                    floating.push_back(b.floating());
                    b.constantFloat(block, floating.back(), index);
                }
                for (auto const value: floating)
                    b.copyFloat(block, b.floating(), value);
                b.ret(block, b.sum(block, values));
            },
            memory,
            target);
        auto const& frame = *allocation.frame;
        CHECK(spillSlots(frame) == 0);
        CHECK(frame.outgoingBytes == hostShadowOf(target));
        // The host's shadow space, the frame pointer's save and the return address, and the saves of the
        // preserved registers taken.
        auto const stack = target.stackRule();
        auto const fixed = frame.outgoingBytes + stack.returnAddressBytes + stack.framePointerBytes
                           + stack.savedBytes(frame.calleeSaved);
        CHECK(frame.totalBytes == ((fixed + stack.alignment - 1) / stack.alignment) * stack.alignment);
    });
}

TEST_CASE("LinearScan.spillsBeyondPressure")
{
    onEachRegisterFile([&](isoline::compiler::machine::ITarget const& target) {
        auto memory = std::pmr::monotonic_buffer_resource {};
        auto const registers = registerCount(target, RegisterClass::General);
        for (auto const count: { registers + 1, registers + 6, 40U })
        {
            INFO(count << " live values");
            auto const allocation = allocateAndCheck(
                [count](Builder& b) {
                    auto& f = b.function();
                    auto const block = f.createBlock(Hotness::Normal);
                    auto values = std::vector<VirtualRegister> {};
                    for (auto const index: std::views::iota(0U, count))
                    {
                        values.push_back(b.general());
                        b.constant(block, values.back(), index);
                    }
                    b.ret(block, b.sum(block, values));
                },
                memory,
                target);
            CHECK(spillSlots(*allocation.frame) >= count - registers);
            CHECK(spillSlots(*allocation.frame) <= count);
        }
    });
}

TEST_CASE("LinearScan.honoursFixedAndTiedOperands")
{
    onEachRegisterFile([&](isoline::compiler::machine::ITarget const& target) {
        auto memory = std::pmr::monotonic_buffer_resource {};
        auto divide = uint32_t { 0 };
        auto shift = uint32_t { 0 };
        auto earlyClobber = uint32_t { 0 };
        auto tied = uint32_t { 0 };
        auto selectorCopy = uint32_t { 0 };
        auto const allocation = allocateAndCheck(
            [&](Builder& b) {
                auto& f = b.function();
                auto const block = f.createBlock(Hotness::Normal);
                auto const dividend = b.general();
                auto const divisor = b.general();
                auto const quotient = b.general();
                b.constant(block, dividend, 100);
                b.constant(block, divisor, 7);
                // quotient (g0) ← dividend (g0) / divisor; both inputs live on, so they are copied into g0.
                divide = b.append(
                    block, FakeOpcode::Divide, { def(f, quotient), use(f, dividend), use(f, divisor) });
                auto const count = b.general();
                b.constant(block, count, 3);
                // quotient <<= count (g1): tied, the count fixed.
                shift = b.append(block, FakeOpcode::Shift, { useDef(f, quotient), use(f, count) });
                auto const clobbered = b.general();
                earlyClobber =
                    b.append(block, FakeOpcode::EarlyClobber, { early(f, clobbered), use(f, dividend) });
                tied = b.append(block, FakeOpcode::AddTied, { useDef(f, clobbered), use(f, divisor) });
                // The selector's own copy into g1, read two instructions later: nothing may take g1 between.
                auto const pinned = b.general();
                b.add(block, pinned, quotient, clobbered);
                selectorCopy = b.append(
                    block,
                    FakeOpcode::Copy,
                    { physical({ .registerClass = RegisterClass::General, .number = 1 }, OperandRole::Def),
                      use(f, pinned) });
                auto const late = b.general();
                b.add(block, late, count, count);
                auto const fromPinned = b.general();
                b.append(
                    block,
                    FakeOpcode::Add,
                    { def(f, fromPinned),
                      physical({ .registerClass = RegisterClass::General, .number = 1 }, OperandRole::Use),
                      use(f, late) });
                b.ret(block, fromPinned);
            },
            memory,
            target);
        auto const& f = allocation.after;
        CHECK(nameOf(f, divide, 0) == generalRegister(0));
        CHECK(nameOf(f, divide, 1) == generalRegister(0));
        CHECK(nameOf(f, divide, 2) != generalRegister(0)); // not g0, which the fixed operands take
        CHECK(nameOf(f, shift, 1) == generalRegister(1));
        CHECK(nameOf(f, shift, 0) != generalRegister(1));
        CHECK(nameOf(f, earlyClobber, 0) != nameOf(f, earlyClobber, 1));
        CHECK(nameOf(f, tied, 0).kind == RegisterKind::Physical);
        CHECK(nameOf(f, selectorCopy, 0) == generalRegister(1));
        // The copies into and out of g0 around the divide.
        CHECK(std::ranges::any_of(f.movesBefore(divide), [](Move const& move) {
            return move.to.index == 0 && move.to.kind == LocationKind::Register;
        }));
    });
}

TEST_CASE("LinearScan.isDeterministic")
{
    onEachRegisterFile([&](isoline::compiler::machine::ITarget const& target) {
        auto build = loopWith(5, true);
        auto first = std::pmr::monotonic_buffer_resource {};
        auto second = std::pmr::monotonic_buffer_resource {};
        auto const one = allocateAndCheck(build, first, target);
        auto const two = allocateAndCheck(build, second, target);
        CHECK(*one.frame == *two.frame);
        REQUIRE(one.after.instructionCount() == two.after.instructionCount());
        for (auto const instruction: std::views::iota(uint32_t { 0 }, one.after.instructionCount()))
        {
            CHECK(std::ranges::equal(one.after.operands(instruction), two.after.operands(instruction)));
            CHECK(std::ranges::equal(one.after.movesBefore(instruction), two.after.movesBefore(instruction)));
        }
        CHECK(one.after.moveCount() > 0);
    });
}

TEST_CASE("LinearScan.wideCallsKeepTheBound")
{
    onEachRegisterFile([&](isoline::compiler::machine::ITarget const& target) {
        static constexpr auto Width = 1'000U;
        auto memory = std::pmr::monotonic_buffer_resource {};
        auto const bound = [](uint64_t frameCost) {
            return (32 * frameCost) + 256;
        };

        SECTION("1'000 live i64 loads marshalled into a 1'000-parameter host import")
        {
            // A host call's arguments go through the ExecutionState's scratch, not the frame: its outgoing
            // area is the host convention's shadow space only. The loads all live up to the marshalling.
            auto const allocation = allocateAndCheck(
                [](Builder& b) {
                    auto& f = b.function();
                    auto const block = f.createBlock(Hotness::Normal);
                    auto values = std::vector<VirtualRegister> {};
                    for (auto const index: std::views::iota(0U, Width))
                    {
                        values.push_back(b.general());
                        b.load(block, values.back(), static_cast<int32_t>(index * 8));
                    }
                    for (auto const index: std::views::iota(0U, Width))
                        b.store(block, values[index], static_cast<int32_t>(index * 8));
                    b.call(block);
                    auto const result = b.general();
                    b.load(block, result);
                    b.ret(block, result);
                },
                memory,
                target,
                Width);
            CHECK(allocation.frame->totalBytes <= bound(Width));
            CHECK(spillSlots(*allocation.frame) >= Width - registerCount(target, RegisterClass::General));
        }

        SECTION("a 1'000-parameter, 1'000-result Wasm callee")
        {
            auto const types = std::vector<isoline::compiler::intermediate::Type>(
                Width, isoline::compiler::intermediate::Type::I64);
            auto const assignment = isoline::compiler::machine::assignCall(
                target.conventionRow(Convention::WasmInternal), types, types, memory);
            auto const outgoing = assignment.arguments.memoryBytes + assignment.results.memoryBytes;
            REQUIRE(outgoing > 0);
            auto call = uint32_t { 0 };
            auto const allocation = allocateAndCheck(
                [&](Builder& b) {
                    auto& f = b.function();
                    auto const block = f.createBlock(Hotness::Normal);
                    auto arguments = std::vector<VirtualRegister> {};
                    for (auto const index: std::views::iota(0U, Width))
                    {
                        arguments.push_back(b.general());
                        b.load(block, arguments.back(), static_cast<int32_t>(index * 8));
                    }
                    for (auto const index: std::views::iota(0U, Width))
                        b.store(block, arguments[index], static_cast<int32_t>(index * 8));
                    call = b.call(block);
                    f.addCallSite(call, outgoing);
                    auto results = std::vector<VirtualRegister> {};
                    for (auto const index: std::views::iota(0U, Width))
                    {
                        results.push_back(b.general());
                        b.load(block, results.back(), static_cast<int32_t>(index * 8));
                    }
                    b.ret(block, b.sum(block, results));
                },
                memory,
                target,
                Width);
            auto const& frame = *allocation.frame;
            CHECK(frame.outgoingBytes == std::max(outgoing, hostShadowOf(target)));
            CHECK(frame.spillOffset >= frame.outgoingBytes);
            CHECK(frame.totalBytes <= bound(Width));
            // The arguments' slots are free once the call has read them: the results take them again.
            CHECK(spillSlots(frame) <= Width + 1);
        }
    });
}

TEST_CASE("LinearScan.refusesPastTheFrameRule")
{
    auto const check = [](isoline::compiler::machine::ITarget const& target) {
        auto memory = std::pmr::monotonic_buffer_resource {};
        auto const allocation = allocateBuilt(loopWith(6, true), target, 4, memory);
        REQUIRE_FALSE(allocation.frame.has_value());
        CHECK(allocation.frame.error().code == CompileErrorCode::FrameTooLarge);
        CHECK(allocation.frame.error().limit == 4);
        CHECK(allocation.frame.error().requested > 4);
    };
    auto const tight = FrameRule { .bytesPerStackValue = 1, .frameOverhead = 0 };
    SECTION("two registers per class")
    {
        check(FakeTarget { tight });
    }
    SECTION("x86-64's register file")
    {
        check(X64FakeTarget { tight });
    }
    SECTION("x86-64's register file under a Microsoft x64 host")
    {
        check(X64WindowsFakeTarget { tight });
    }
}

namespace
{

/// @p count values live at once: every one spilled but as many as there are registers.
Build manyLiveValues(uint32_t count)
{
    return [count](Builder& b) {
        auto& f = b.function();
        auto const block = f.createBlock(Hotness::Normal);
        auto values = std::vector<VirtualRegister> {};
        for (auto const index: std::views::iota(0U, count))
        {
            values.push_back(b.general());
            b.constant(block, values.back(), index);
        }
        for (auto const value: values)
            b.store(block, value);
        auto const result = b.general();
        b.constant(block, result);
        b.ret(block, result);
    };
}

/// How long allocating @p count live values on @p target takes, and checks it spilled them.
std::chrono::nanoseconds timeManyLiveValues(isoline::compiler::machine::ITarget const& target, uint32_t count)
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto const build = manyLiveValues(count);
    auto const started = std::chrono::steady_clock::now();
    auto const allocation = allocateBuilt(build, target, count, memory);
    auto const elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE(allocation.frame.has_value());
    CHECK(spillSlots(*allocation.frame) >= count - registerCount(target, RegisterClass::General));
    return elapsed;
}

} // namespace

TEST_CASE("LinearScan.manySpillsAreNLogN", "[.perf]")
{
    // 50'000 spilled intervals well under a second, and four times the values in not much more than four
    // times the time: each slot comes off the heap (first-fit packing is quadratic here).
    onEachRegisterFile([&](isoline::compiler::machine::ITarget const& target) {
        auto const quarter = timeManyLiveValues(target, 12'500);
        auto const full = timeManyLiveValues(target, 50'000);
        INFO("12'500: " << quarter.count() << " ns, 50'000: " << full.count() << " ns");
        CHECK(full < std::chrono::milliseconds { 500 });
        CHECK(full < quarter * 8);
    });
}

namespace
{

/// @p locals values live across a body of @p temporaries short ones, and one used early and then only past
/// them all: its rest is placed far from its next use, against registers whose holders need them sooner.
Build localsOverTemporaries(uint32_t locals, uint32_t temporaries)
{
    return [=](Builder& b) {
        auto& f = b.function();
        auto const block = f.createBlock(Hotness::Normal);
        auto values = std::vector<VirtualRegister> {};
        for ([[maybe_unused]] auto const index: std::views::iota(0U, locals))
        {
            values.push_back(b.general());
            b.constant(block, values.back());
        }
        auto const far = b.general();
        b.constant(block, far);
        b.store(block, far);
        for ([[maybe_unused]] auto const index: std::views::iota(0U, temporaries))
        {
            auto const temporary = b.general();
            b.constant(block, temporary);
            b.store(block, temporary);
        }
        for (auto const value: values)
            b.store(block, value);
        b.store(block, far);
        auto const result = b.general();
        b.constant(block, result);
        b.ret(block, result);
    };
}

/// The scan's work on @p build, which it allocates.
uint64_t scanWorkOf(Build const& build, uint64_t frameCost)
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto function = MachineFunction { memory };
    auto builder = Builder { function };
    build(builder);
    auto work = ScanWork {};
    auto const frame = LinearScanAllocator { CompileLimits {}, work }.allocate(
        function, FakeTarget {}, frameCost, CompileCancellation {}, memory);
    REQUIRE(frame.has_value());
    return work.steps;
}

} // namespace

TEST_CASE("LinearScan.rankingIsLinearInTheHolders")
{
    // Counted rather than timed: K locals live across N short temporaries, K growing with N, and a rest far
    // from its next use. Each register's ranking stops at the first holder that needs it by the use, so four
    // times the function is about four times the work, not sixteen.
    auto const workOf = [](uint32_t temporaries) {
        return scanWorkOf(localsOverTemporaries(temporaries / 8, temporaries), temporaries);
    };
    auto const quarter = workOf(1'000);
    auto const full = workOf(4'000);
    INFO("1'000: " << quarter << " steps, 4'000: " << full << " steps");
    CHECK(full < quarter * 5);
}

TEST_CASE("LinearScan.validatesAssignment")
{
    onEachRegisterFile([&](isoline::compiler::machine::ITarget const& target) {
        auto memory = std::pmr::monotonic_buffer_resource {};
        auto build = loopWith(4, true);
        auto allocation = allocateAndCheck(build, memory, target);
        auto const& frame = *allocation.frame;
        auto const check = [&](FrameLayout const& checked) {
            return checkAssignment(allocation.before, allocation.after, target, checked, 64, memory);
        };
        REQUIRE(check(frame).has_value());

        SECTION("a read from the wrong register")
        {
            // The first read of a value moved to the other register reads what that one holds instead.
            auto& f = allocation.after;
            auto tampered = false;
            for (auto const instruction: std::views::iota(uint32_t { 0 }, f.instructionCount()))
            {
                if (f.instruction(instruction).opcode != opcodeOf(FakeOpcode::Add) || tampered)
                    continue;
                auto& operand = std::get<RegisterOperand>(f.operands(instruction)[1]);
                operand.name.number ^= 1U;
                tampered = true;
            }
            REQUIRE(tampered);
            auto const checked = check(frame);
            REQUIRE_FALSE(checked.has_value());
            CHECK(checked.error().kind == AssignmentFaultKind::WrongValue);
        }

        SECTION("an outgoing area smaller than a call's")
        {
            allocation.after.addCallSite(0, 64);
            auto const checked = check(frame);
            REQUIRE_FALSE(checked.has_value());
            CHECK(checked.error().kind == AssignmentFaultKind::OutgoingTooSmall);
        }

        SECTION("a misaligned frame")
        {
            auto misaligned = frame;
            misaligned.totalBytes += 8;
            auto const checked = check(misaligned);
            REQUIRE_FALSE(checked.has_value());
            CHECK(checked.error().kind == AssignmentFaultKind::FrameRefused);
        }

        SECTION("a slot outside the spill area")
        {
            auto shrunk = frame;
            REQUIRE(shrunk.spillBytes > 0);
            shrunk.spillBytes = 0;
            auto const checked = check(shrunk);
            REQUIRE_FALSE(checked.has_value());
            CHECK(checked.error().kind == AssignmentFaultKind::BadPlace);
        }

        SECTION("an allocated operand off its row")
        {
            // The return's operand is fixed to g0; naming g1 there breaks the row, whatever g1 holds.
            auto& f = allocation.after;
            auto tampered = false;
            for (auto const instruction: std::views::iota(uint32_t { 0 }, f.instructionCount()))
                if (f.instruction(instruction).opcode == opcodeOf(FakeOpcode::Return))
                {
                    std::get<RegisterOperand>(f.operands(instruction)[0]).name = generalRegister(1);
                    tampered = true;
                }
            REQUIRE(tampered);
            auto const checked = check(frame);
            REQUIRE_FALSE(checked.has_value());
            CHECK(checked.error().kind == AssignmentFaultKind::ConstraintBroken);
        }

        SECTION("an unallocated function")
        {
            auto const checked =
                checkAssignment(allocation.before, allocation.before, target, frame, 64, memory);
            REQUIRE_FALSE(checked.has_value());
            CHECK(checked.error().kind == AssignmentFaultKind::VirtualLeft);
        }
    });
}

TEST_CASE("LinearScan.callsClobberAcrossSplits")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    // Two values live across two calls, one of them read between: each call splits them into their slots,
    // and the read between reloads.
    auto const allocation = allocateAndCheck(
        [](Builder& b) {
            auto& f = b.function();
            auto const block = f.createBlock(Hotness::Normal);
            auto const x = b.general();
            auto const y = b.general();
            b.constant(block, x, 1);
            b.constant(block, y, 2);
            b.call(block);
            auto const between = b.general();
            b.add(block, between, x, x);
            b.call(block);
            auto const z = b.general();
            b.add(block, z, x, y);
            auto const w = b.general();
            b.add(block, w, z, between);
            b.ret(block, w);
        },
        memory);
    CHECK(spillSlots(*allocation.frame) >= 3);
    auto const moves = allMoves(allocation.after);
    CHECK(std::ranges::any_of(moves, [](Move const& move) { return move.from.kind == LocationKind::Slot; }));
    CHECK(std::ranges::any_of(moves, [](Move const& move) { return move.to.kind == LocationKind::Slot; }));
}

TEST_CASE("LinearScan.edgeMovesAreParallel")
{
    auto memory = std::pmr::monotonic_buffer_resource {};

    SECTION("a back edge that swaps two parameters")
    {
        auto const allocation = allocateAndCheck(
            [](Builder& b) {
                auto& f = b.function();
                auto const entry = f.createBlock(Hotness::Normal);
                auto const header = f.createBlock(Hotness::Normal);
                auto const latch = f.createBlock(Hotness::Normal);
                auto const exit = f.createBlock(Hotness::Normal);
                auto const a0 = b.general();
                auto const b0 = b.general();
                b.constant(entry, a0, 1);
                b.constant(entry, b0, 2);
                b.jump(entry, header, { a0, b0 });
                auto const a = b.general();
                auto const c = b.general();
                f.setParameters(header, std::to_array({ a, c }));
                b.branch(header, a, latch, exit);
                b.jump(latch, header, { c, a });
                auto const out = b.general();
                b.add(exit, out, a, c);
                b.ret(exit, out);
            },
            memory);
        // Both stay in registers, so the swap is a cycle broken through the scratch slot.
        CHECK(spillSlots(*allocation.frame) == 1);
    }

    SECTION("a rotation of three parameters, under pressure")
    {
        auto const allocation = allocateAndCheck(
            [](Builder& b) {
                auto& f = b.function();
                auto const entry = f.createBlock(Hotness::Normal);
                auto const header = f.createBlock(Hotness::Normal);
                auto const latch = f.createBlock(Hotness::Normal);
                auto const exit = f.createBlock(Hotness::Normal);
                auto initial = std::vector<VirtualRegister> {};
                for (auto const index: std::views::iota(0, 3))
                {
                    initial.push_back(b.general());
                    b.constant(entry, initial.back(), index);
                }
                b.jump(entry, header, { initial[0], initial[1], initial[2] });
                auto const x = b.general();
                auto const y = b.general();
                auto const z = b.general();
                f.setParameters(header, std::to_array({ x, y, z }));
                b.branch(header, x, latch, exit);
                b.jump(latch, header, { y, z, x });
                auto const values = std::to_array({ x, y, z });
                b.ret(exit, b.sum(exit, values));
            },
            memory);
        CHECK(allocation.frame.has_value());
    }

    SECTION("a branch whose targets take arguments in their own blocks")
    {
        std::ignore = allocateAndCheck(
            [](Builder& b) {
                auto& f = b.function();
                auto const entry = f.createBlock(Hotness::Normal);
                auto const left = f.createBlock(Hotness::Normal);
                auto const right = f.createBlock(Hotness::Cold);
                auto const join = f.createBlock(Hotness::Normal);
                auto const x = b.general();
                auto const y = b.general();
                b.constant(entry, x, 1);
                b.constant(entry, y, 2);
                b.branch(entry, x, left, right);
                b.jump(left, join, { x, y });
                b.jump(right, join, { y, x });
                auto const p = b.general();
                auto const q = b.general();
                f.setParameters(join, std::to_array({ p, q }));
                auto const out = b.general();
                b.add(join, out, p, q);
                b.ret(join, out);
            },
            memory);
    }
}

TEST_CASE("LinearScan.spillsAndReloadsAroundPollsAndTraps")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    // A loop polling on its back edge, a value live across the poll, and a cold trap block reading a value
    // that was spilled across a call.
    auto poll = uint32_t { 0 };
    auto const allocation = allocateAndCheck(
        [&](Builder& b) {
            auto& f = b.function();
            auto const entry = f.createBlock(Hotness::Normal);
            auto const header = f.createBlock(Hotness::Normal);
            auto const body = f.createBlock(Hotness::Normal);
            auto const latch = f.createBlock(Hotness::Normal);
            auto const trap = f.createBlock(Hotness::Cold);
            auto const exit = f.createBlock(Hotness::Normal);
            auto const kept = b.general();
            auto const i0 = b.general();
            b.constant(entry, kept, 42);
            b.constant(entry, i0, 10);
            b.call(entry);
            b.jump(entry, header, { i0 });
            auto const i = b.general();
            f.setParameters(header, std::to_array({ i }));
            b.branch(header, i, body, exit);
            auto const next = b.general();
            b.add(body, next, i, kept);
            poll = b.poll(body);
            b.branch(body, next, latch, trap);
            b.jump(latch, header, { next });
            b.store(trap, kept);
            b.ret(trap, kept);
            b.ret(exit, kept);
        },
        memory);
    // Whatever was in g0 before the poll is not read from it after.
    CHECK(allocation.frame.has_value());
    CHECK(nameOf(allocation.after, poll, 0) == generalRegister(0));
}

TEST_CASE("LinearScan.usesLifetimeHoles")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    // v lives only into the right arm; the left arm's w takes its register through v's hole.
    auto const allocation = allocateAndCheck(
        [](Builder& b) {
            auto& f = b.function();
            auto const entry = f.createBlock(Hotness::Normal);
            auto const left = f.createBlock(Hotness::Normal);
            auto const right = f.createBlock(Hotness::Normal);
            auto const join = f.createBlock(Hotness::Normal);
            auto const u = b.general();
            auto const v = b.general();
            b.constant(entry, u, 1);
            b.constant(entry, v, 2);
            b.branch(entry, u, left, right);
            auto const w = b.general();
            b.constant(left, w, 3);
            b.store(left, w);
            b.jump(left, join);
            b.store(right, v);
            b.jump(right, join);
            b.ret(join, u);
        },
        memory);
    CHECK(spillSlots(*allocation.frame) == 0);
}

TEST_CASE("LinearScan.recordsTheCalleeSavedRegisters")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto const target = FakeTarget { FakeFrameRule, FakeSaves::SecondGeneral };
    auto const allocation = allocateAndCheck(
        [](Builder& b) {
            auto& f = b.function();
            auto const block = f.createBlock(Hotness::Normal);
            auto const x = b.general();
            auto const y = b.general();
            b.constant(block, x, 1);
            b.constant(block, y, 2);
            auto const z = b.general();
            b.add(block, z, x, y);
            b.ret(block, z);
        },
        memory,
        target);
    auto const g1 = PhysicalRegister { .registerClass = RegisterClass::General, .number = 1 };
    CHECK(allocation.frame->calleeSaved.contains(g1));
    CHECK(allocation.frame->calleeSaved.countIn(RegisterClass::General) == 1);
    CHECK(allocation.frame->totalBytes == 32); // 8 saved, 8 frame pointer, 8 return address, aligned
}

TEST_CASE("LinearScan.outgoingAreaCoversEveryCall")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto const allocation = allocateAndCheck(
        [](Builder& b) {
            auto& f = b.function();
            auto const block = f.createBlock(Hotness::Normal);
            auto const x = b.general();
            b.constant(block, x, 1);
            b.store(block, x, 40); // a stack argument
            f.addCallSite(b.call(block), 48);
            f.addCallSite(b.call(block), 16);
            auto const y = b.general();
            b.constant(block, y, 2);
            b.ret(block, y);
        },
        memory);
    CHECK(allocation.frame->outgoingBytes == 48);
    CHECK(allocation.frame->spillOffset == 48);
    CHECK(allocation.frame->totalBytes % 16 == 0);
}

TEST_CASE("LinearScan.hostCallStackArgumentsReachTheFrame")
{
    // A call under the real Microsoft x64 row with six integer arguments: four in rcx, rdx, r8 and r9, two on
    // the stack above the 32 bytes of shadow space. The selector records the call's need from assignCall()
    // (shadow space included) and the frame's outgoing area covers it; the host's shadow space alone is the
    // frame's floor (ledger carries to T10/T11).
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto const target = X64WindowsFakeTarget {};
    auto const types =
        std::vector<isoline::compiler::intermediate::Type>(6, isoline::compiler::intermediate::Type::I64);
    auto const assignment = isoline::compiler::machine::assignCall(
        target.conventionRow(Convention::Windows64), types, {}, memory);
    REQUIRE(assignment.arguments.memoryBytes == 48);
    auto const allocation = allocateAndCheck(
        [&](Builder& b) {
            auto& f = b.function();
            auto const block = f.createBlock(Hotness::Normal);
            auto const x = b.general();
            b.constant(block, x, 1);
            b.store(block, x, 32); // the fifth argument
            b.store(block, x, 40); // the sixth
            f.addCallSite(b.poll(block), assignment.arguments.memoryBytes);
            b.ret(block, x);
        },
        memory,
        target);
    auto const& frame = *allocation.frame;
    CHECK(frame.outgoingBytes == assignment.arguments.memoryBytes);
    CHECK(frame.spillOffset >= frame.outgoingBytes);
    CHECK(frame.totalBytes % 16 == 0);

    // With no call of its own, the frame still holds the host's shadow space: its entry poll calls the host.
    auto const leaf = allocateAndCheck(
        [](Builder& b) {
            auto& f = b.function();
            auto const block = f.createBlock(Hotness::Normal);
            auto const x = b.general();
            b.constant(block, x, 1);
            b.ret(block, x);
        },
        memory,
        target);
    CHECK(leaf.frame->outgoingBytes == 32);
}

TEST_CASE("LinearScan.refusesWhatItCannotAllocate")
{
    auto memory = std::pmr::monotonic_buffer_resource {};

    SECTION("a critical edge")
    {
        auto const allocation = allocateBuilt(
            [](Builder& b) {
                auto& f = b.function();
                auto const entry = f.createBlock(Hotness::Normal);
                auto const middle = f.createBlock(Hotness::Normal);
                auto const join = f.createBlock(Hotness::Normal);
                auto const x = b.general();
                b.constant(entry, x, 1);
                b.branch(entry, x, middle, join); // entry → join is critical: join has two predecessors
                b.jump(middle, join);
                b.ret(join, x);
            },
            FakeTarget {},
            64,
            memory);
        REQUIRE_FALSE(allocation.frame.has_value());
        CHECK(allocation.frame.error().code == CompileErrorCode::InternalVerificationFailed);
        CHECK(allocation.frame.error().pass == "register-allocation");
    }

    SECTION("an instruction asking for more registers than there are")
    {
        auto const allocation = allocateBuilt(
            [](Builder& b) {
                auto& f = b.function();
                auto const block = f.createBlock(Hotness::Normal);
                auto const x = b.general();
                auto const y = b.general();
                auto const z = b.general();
                b.constant(block, x, 1);
                b.constant(block, y, 2);
                b.constant(block, z, 3);
                auto const result = b.general();
                b.append(
                    block,
                    FakeOpcode::Add,
                    { def(f, result), use(f, x), MemoryOperand { .base = f.name(y), .index = f.name(z) } });
                b.ret(block, result);
            },
            FakeTarget {},
            64,
            memory);
        REQUIRE_FALSE(allocation.frame.has_value());
        CHECK(allocation.frame.error().code == CompileErrorCode::InternalVerificationFailed);
    }

    SECTION("a function the machine verifier refuses")
    {
        auto const allocation = allocateBuilt(
            [](Builder& b) {
                auto& f = b.function();
                auto const block = f.createBlock(Hotness::Normal);
                auto const x = b.general();
                b.append(block, FakeOpcode::Constant, { use(f, x), Immediate { 1 } }); // a Def row read
                b.ret(block, x);
            },
            FakeTarget {},
            64,
            memory);
        REQUIRE_FALSE(allocation.frame.has_value());
        CHECK(allocation.frame.error().pass == "machine-verifier");
    }
}

TEST_CASE("LinearScan.honoursCancellation")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto cancellation = CompileCancellation {};
    cancellation.cancel();
    SECTION("at the first interval")
    {
        auto const allocation = allocateBuilt(loopWith(3, false), FakeTarget {}, 64, memory, cancellation);
        REQUIRE_FALSE(allocation.frame.has_value());
        CHECK(allocation.frame.error().code == CompileErrorCode::Cancelled);
    }
    SECTION("past 4'096 intervals, in one block liveness takes without a poll")
    {
        auto const allocation =
            allocateBuilt(manyLiveValues(5'000), FakeTarget {}, 5'000, memory, cancellation);
        REQUIRE_FALSE(allocation.frame.has_value());
        CHECK(allocation.frame.error().code == CompileErrorCode::Cancelled);
    }
}

namespace
{

/// A fixed sequence of 64-bit words (splitmix64), the same on every host and run.
class SplitMix
{
  public:
    explicit SplitMix(uint64_t state) noexcept: _state { state } {}

    /// The next word below @p bound (non-zero).
    uint64_t below(uint64_t bound) noexcept
    {
        _state += 0x9E37'79B9'7F4A'7C15ULL;
        auto mix = _state;
        mix = (mix ^ (mix >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        mix = (mix ^ (mix >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        return (mix ^ (mix >> 31U)) % bound;
    }

  private:
    uint64_t _state;
};

/// A generator of random, well-formed FakeTarget functions: straight runs of every kind of instruction
/// (fixed, tied, early, calls, polls, memory), diamonds whose arms pass arguments to the join, and loops
/// whose back edges pass arguments to the header, nested, over a pool of values long enough to spill.
class RandomFunction
{
  public:
    RandomFunction(Builder& builder, uint64_t seed): _b { builder }, _random { seed } {}

    void build()
    {
        auto& f = _b.function();
        _current = f.createBlock(Hotness::Normal);
        auto pool = Pool {};
        for ([[maybe_unused]] auto const index: std::views::iota(0, 3))
        {
            pool.general.push_back(_b.general());
            _b.constant(_current, pool.general.back(), 7);
            pool.floats.push_back(_b.floating());
            _b.constantFloat(_current, pool.floats.back());
        }
        region(pool, 3);
        auto const picks = std::to_array({ pick(pool.general), pick(pool.general), pick(pool.general) });
        _b.ret(_current, _b.sum(_current, picks));
    }

  private:
    /// The values in scope, by class.
    struct Pool
    {
        std::vector<VirtualRegister> general;
        std::vector<VirtualRegister> floats;
    };

    VirtualRegister pick(std::vector<VirtualRegister> const& values)
    {
        return values[_random.below(values.size())];
    }

    void region(Pool& pool, uint32_t depth)
    {
        auto const parts = 1 + _random.below(3);
        for ([[maybe_unused]] auto const part: std::views::iota(uint64_t { 0 }, parts))
        {
            straight(pool, 1 + static_cast<uint32_t>(_random.below(6)));
            if (depth == 0)
                continue;
            switch (_random.below(3))
            {
                case 0: diamond(pool, depth - 1); break;
                case 1: loop(pool, depth - 1); break;
                default: break;
            }
        }
    }

    void straight(Pool& pool, uint32_t count)
    {
        auto& f = _b.function();
        auto& values = pool.general;
        for ([[maybe_unused]] auto const index: std::views::iota(0U, count))
        {
            if (_random.below(3) == 0)
            {
                // A Float value: a copy of one in scope, or a constant; calls and polls clobber them too.
                auto const result = _b.floating();
                if (_random.below(4) == 0)
                    _b.constantFloat(_current, result);
                else
                    _b.copyFloat(_current, result, pick(pool.floats));
                pool.floats.push_back(result);
                continue;
            }
            auto const result = _b.general();
            switch (_random.below(12))
            {
                case 0: _b.constant(_current, result, 1); break;
                case 1: _b.add(_current, result, pick(values), pick(values)); break;
                case 2:
                    _b.append(_current,
                              FakeOpcode::Divide,
                              { def(f, result), use(f, pick(values)), use(f, pick(values)) });
                    break;
                case 3:
                    _b.append(_current, FakeOpcode::Copy, { def(f, result), use(f, pick(values)) });
                    _b.append(_current, FakeOpcode::Shift, { useDef(f, result), use(f, pick(values)) });
                    break;
                case 4:
                    _b.append(_current, FakeOpcode::EarlyClobber, { early(f, result), use(f, pick(values)) });
                    break;
                case 5:
                    _b.append(_current, FakeOpcode::Copy, { def(f, result), use(f, pick(values)) });
                    _b.append(_current, FakeOpcode::AddTied, { useDef(f, result), use(f, pick(values)) });
                    break;
                case 6:
                    _b.call(_current);
                    _b.load(_current, result);
                    break;
                case 7:
                    _b.poll(_current);
                    _b.add(_current, result, pick(values), pick(values));
                    break;
                case 8:
                    _b.store(_current, pick(values));
                    _b.load(_current, result, 8);
                    break;
                case 9:
                    // A call's arguments in g0 and g1, either operand order.
                    _b.append(_current,
                              _random.below(2) == 0 ? FakeOpcode::PassArguments
                                                    : FakeOpcode::PassArgumentsSwapped,
                              { use(f, pick(values)), use(f, pick(values)) });
                    _b.add(_current, result, pick(values), pick(values));
                    break;
                case 10:
                    // An indirect call reads its target from a register while it clobbers every one.
                    _b.callIndirect(_current, pick(values), result);
                    break;
                default: _b.constant(_current, result, 2); break;
            }
            values.push_back(result);
        }
    }

    /// The arguments an edge passes: two General values and one Float, picked from @p pool.
    std::array<VirtualRegister, 3> arguments(Pool const& pool)
    {
        return { pick(pool.general), pick(pool.general), pick(pool.floats) };
    }

    /// Gives @p block the parameters arguments() passes, and adds them to @p pool.
    void parameters(BlockId block, Pool& pool)
    {
        auto const values = std::to_array({ _b.general(), _b.general(), _b.floating() });
        _b.function().setParameters(block, values);
        pool.general.push_back(values[0]);
        pool.general.push_back(values[1]);
        pool.floats.push_back(values[2]);
    }

    void diamond(Pool& pool, uint32_t depth)
    {
        auto& f = _b.function();
        auto const left = f.createBlock(Hotness::Normal);
        auto const right = f.createBlock(_random.below(2) == 0 ? Hotness::Normal : Hotness::Cold);
        auto const join = f.createBlock(Hotness::Normal);
        _b.branch(_current, pick(pool.general), left, right);
        for (auto const arm: { left, right })
        {
            _current = arm;
            auto armPool = pool;
            region(armPool, depth);
            _b.jumpWith(_current, join, arguments(armPool));
        }
        parameters(join, pool);
        _current = join;
    }

    void loop(Pool& pool, uint32_t depth)
    {
        auto& f = _b.function();
        auto const header = f.createBlock(Hotness::Normal);
        auto const body = f.createBlock(Hotness::Normal);
        auto const exit = f.createBlock(Hotness::Normal);
        _b.jumpWith(_current, header, arguments(pool));
        auto inner = pool;
        parameters(header, inner);
        auto const counter = inner.general[inner.general.size() - 2];
        _b.branch(header, counter, body, exit);
        _current = body;
        region(inner, depth);
        _b.jumpWith(_current, header, arguments(inner));
        _current = exit;
        // After the loop: what was in scope before it, and the header's parameters.
        auto const header0 = f.parameters(header);
        pool.general.push_back(header0[0]);
        pool.general.push_back(header0[1]);
        pool.floats.push_back(header0[2]);
    }

    Builder& _b;
    SplitMix _random;
    BlockId _current {};
};

} // namespace

TEST_CASE("LinearScan.randomFunctionsCheck")
{
    // Each seed's function is allocated and re-simulated by the AssignmentChecker: every read finds its
    // value.
    auto spilled = uint64_t { 0 };
    auto moves = uint64_t { 0 };
    auto scratched = 0;
    for (auto const seed: std::views::iota(uint64_t { 1 }, uint64_t { 401 }))
    {
        INFO("seed " << seed);
        auto memory = std::pmr::monotonic_buffer_resource {};
        auto const allocation = allocateAndCheck(
            [seed](Builder& b) { RandomFunction { b, seed }.build(); }, memory, FakeTarget {}, 100'000);
        spilled += spillSlots(*allocation.frame);
        moves += allocation.after.moveCount();
        auto const all = allMoves(allocation.after);
        auto const scratch = allocation.frame->spillOffset + allocation.frame->spillBytes - 8;
        if (std::ranges::any_of(all, [&](Move const& move) {
                return move.to.kind == LocationKind::Slot && move.to.index == scratch;
            }))
            ++scratched;
    }
    // The shapes reach what they are for: spills, moves, and cycles broken through the scratch slot.
    CHECK(spilled > 1'000);
    CHECK(moves > 10'000);
    CHECK(scratched > 0);
}

namespace
{

/// A target that forwards to a FakeTarget, for test targets that change one answer of it.
class ForwardingTarget: public isoline::compiler::machine::ITarget
{
  public:
    [[nodiscard]] isoline::compiler::machine::TargetDescription const& description() const noexcept override
    {
        return _fake.description();
    }
    [[nodiscard]] FrameRule frameRule() const noexcept override { return _fake.frameRule(); }
    [[nodiscard]] isoline::compiler::machine::StackRule stackRule() const noexcept override
    {
        return _fake.stackRule();
    }
    [[nodiscard]] isoline::compiler::machine::ConventionRow const& conventionRow(
        isoline::compiler::machine::Convention convention) const override
    {
        return _fake.conventionRow(convention);
    }
    [[nodiscard]] std::span<PhysicalRegister const> allocatable(
        RegisterClass registerClass) const noexcept override
    {
        return _fake.allocatable(registerClass);
    }
    [[nodiscard]] std::span<isoline::compiler::machine::OperandConstraint const> constraints(
        uint16_t machineOpcode) const override
    {
        return _fake.constraints(machineOpcode);
    }
    [[nodiscard]] std::expected<MachineFunction, CompileError> select(
        isoline::compiler::intermediate::Function const& function,
        isoline::compiler::machine::SelectionContext const& context) const override
    {
        return _fake.select(function, context);
    }
    [[nodiscard]] std::expected<void, CompileError> encode(
        MachineFunction const& function,
        FrameLayout const& frame,
        isoline::compiler::machine::CodeBuffer& buffer) const override
    {
        return _fake.encode(function, frame, buffer);
    }
    void emitEntryAdapter(isoline::binary::FuncType const& type,
                          isoline::compiler::machine::CodeBuffer& buffer) const override
    {
        _fake.emitEntryAdapter(type, buffer);
    }
    void emitEntryTrampoline(isoline::compiler::machine::CodeBuffer& buffer) const override
    {
        _fake.emitEntryTrampoline(buffer);
    }

  private:
    FakeTarget _fake {};
};

/// FakeTarget without Float registers: a target bug the allocator refuses rather than trusts.
class NoFloatTarget final: public ForwardingTarget
{
  public:
    [[nodiscard]] std::span<PhysicalRegister const> allocatable(
        RegisterClass registerClass) const noexcept override
    {
        return registerClass == RegisterClass::Float ? std::span<PhysicalRegister const> {}
                                                     : ForwardingTarget::allocatable(registerClass);
    }
};

/// FakeTarget that cancels @p cancellation at the scan's @p after-th placement: the scan asks for the
/// registers of each child it places, and nothing before the scan does.
class CancellingTarget final: public ForwardingTarget
{
  public:
    CancellingTarget(CompileCancellation& cancellation, uint32_t after) noexcept:
        _cancellation { &cancellation }, _after { after }
    {
    }

    [[nodiscard]] std::span<PhysicalRegister const> allocatable(
        RegisterClass registerClass) const noexcept override
    {
        if (++_asked == _after)
            _cancellation->cancel();
        return ForwardingTarget::allocatable(registerClass);
    }

    [[nodiscard]] uint32_t asked() const noexcept { return _asked; }

  private:
    CompileCancellation* _cancellation;
    uint32_t _after;
    mutable uint32_t _asked = 0; ///< How often the scan asked: the test's probe, not the target's state.
};

/// Allocates @p build on @p target under @p limits, without the check.
std::expected<FrameLayout, CompileError> allocateOn(Build const& build,
                                                    isoline::compiler::machine::ITarget const& target,
                                                    std::pmr::memory_resource& memory,
                                                    CompileLimits const& limits = CompileLimits {})
{
    auto function = MachineFunction { memory };
    auto builder = Builder { function };
    build(builder);
    return LinearScanAllocator { limits }.allocate(function, target, 64, CompileCancellation {}, memory);
}

/// Whether @p result is the allocator's refusal of a function it cannot take.
bool isRefusedAsIllFormed(std::expected<FrameLayout, CompileError> const& result)
{
    return !result.has_value() && result.error().code == CompileErrorCode::InternalVerificationFailed
           && result.error().pass == "register-allocation";
}

} // namespace

TEST_CASE("LinearScan.refusesWhatTheSelectorMustNotMake")
{
    auto memory = std::pmr::monotonic_buffer_resource {};

    SECTION("a fixed definition no instruction follows: its copy out has no gap")
    {
        auto const result = allocateOn(
            [](Builder& b) {
                auto& f = b.function();
                auto const block = f.createBlock(Hotness::Normal);
                auto const result = b.general();
                auto operands = clobbers(defaultTarget);
                operands[0] =
                    def(f, result); // the call's result, fixed to g0, as the block's last instruction
                std::ignore = f.append(block, opcodeOf(FakeOpcode::Call), operands);
            },
            FakeTarget {},
            memory);
        CHECK(isRefusedAsIllFormed(result));
    }

    SECTION("a jump that reads a register: its edge moves could not go before it")
    {
        auto const result = allocateOn(
            [](Builder& b) {
                auto& f = b.function();
                auto const entry = f.createBlock(Hotness::Normal);
                auto const exit = f.createBlock(Hotness::Normal);
                auto const x = b.general();
                b.constant(entry, x, 1);
                auto const edge = std::to_array<EdgeTo>({ EdgeTo { .target = exit } });
                f.setEdges(entry, edge);
                b.append(
                    entry, FakeOpcode::Jump, { use(f, x), isoline::compiler::machine::testing::label(exit) });
                b.ret(exit, x);
            },
            FakeTarget {},
            memory);
        CHECK(isRefusedAsIllFormed(result));
    }

    SECTION("an edge into a block without instructions")
    {
        auto const result = allocateOn(
            [](Builder& b) {
                auto& f = b.function();
                auto const entry = f.createBlock(Hotness::Normal);
                auto const empty = f.createBlock(Hotness::Normal);
                auto const other = f.createBlock(Hotness::Normal);
                auto const x = b.general();
                b.constant(entry, x, 1);
                b.branch(entry, x, empty, other);
                b.ret(other, x);
            },
            FakeTarget {},
            memory);
        CHECK(isRefusedAsIllFormed(result));
    }

    SECTION("a class without registers")
    {
        auto const target = NoFloatTarget {};
        auto const result = allocateOn(
            [](Builder& b) {
                auto& f = b.function();
                auto const block = f.createBlock(Hotness::Normal);
                auto const p = b.floating();
                auto const q = b.floating();
                b.copyFloat(block, p, q);
                auto const x = b.general();
                b.constant(block, x, 1);
                b.ret(block, x);
            },
            target,
            memory);
        CHECK(isRefusedAsIllFormed(result));
    }
}

TEST_CASE("LinearScan.refusesPastItsCaps")
{
    auto memory = std::pmr::monotonic_buffer_resource {};

    SECTION("liveness past maxLiveSetBits")
    {
        auto limits = CompileLimits {};
        limits.maxLiveSetBits = 1;
        auto const result = allocateOn(loopWith(2, false), FakeTarget {}, memory, limits);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == CompileErrorCode::LiveSetTooLarge);
    }

    SECTION("an outgoing area past 32 bits")
    {
        auto const result = allocateOn(
            [](Builder& b) {
                auto& f = b.function();
                auto const block = f.createBlock(Hotness::Normal);
                f.addCallSite(b.call(block), std::numeric_limits<uint32_t>::max());
                auto const x = b.general();
                b.constant(block, x, 1);
                b.ret(block, x);
            },
            FakeTarget {},
            memory);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == CompileErrorCode::FrameTooLarge);
        CHECK(result.error().requested > std::numeric_limits<uint32_t>::max());
    }

    SECTION("spill slots past 32 bits of offset")
    {
        auto const result = allocateOn(
            [](Builder& b) {
                auto& f = b.function();
                auto const block = f.createBlock(Hotness::Normal);
                auto const x = b.general();
                b.constant(block, x, 1);
                f.addCallSite(b.call(block), std::numeric_limits<uint32_t>::max() - 15);
                auto const y = b.general();
                b.add(block, y, x, x);
                b.ret(block, y);
            },
            FakeTarget {},
            memory);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == CompileErrorCode::FrameTooLarge);
    }
}

TEST_CASE("LinearScan.keepsPhysicalRegistersFromEntryToRead")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    // The entry block reads g0 and g1 as the convention's arguments before anything defines them, and a load
    // addresses through g1: nothing may take g0 before its read, nor g1 before the load.
    auto const allocation = allocateAndCheck(
        [](Builder& b) {
            auto& f = b.function();
            auto const block = f.createBlock(Hotness::Normal);
            auto const argument = b.general();
            b.append(
                block,
                FakeOpcode::Copy,
                { def(f, argument),
                  physical({ .registerClass = RegisterClass::General, .number = 0 }, OperandRole::Use) });
            auto const x = b.general();
            b.constant(block, x, 1);
            auto const loaded = b.general();
            b.append(
                block, FakeOpcode::Load, { def(f, loaded), MemoryOperand { .base = generalRegister(1) } });
            auto const sum = b.general();
            b.add(block, sum, x, argument);
            auto const total = b.general();
            b.add(block, total, sum, loaded);
            b.ret(block, total);
        },
        memory);
    // x is defined while g1 still holds the load's base: it takes g0 from the argument, which spills.
    auto const moves = allMoves(allocation.after);
    CHECK(spillSlots(*allocation.frame) >= 1);
    CHECK(std::ranges::any_of(moves, [](Move const& move) { return move.to.kind == LocationKind::Slot; }));
}

TEST_CASE("LinearScan.readsAnIndirectCallsTargetFromARegister")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    // The target lives on past the call, which clobbers every register: it is read from one at the call and
    // stored to its slot in the gap before, then reloaded after.
    auto call = uint32_t { 0 };
    auto const allocation = allocateAndCheck(
        [&](Builder& b) {
            auto& f = b.function();
            auto const block = f.createBlock(Hotness::Normal);
            auto const target = b.general();
            b.constant(block, target, 64);
            auto const returned = b.general();
            call = b.callIndirect(block, target, returned);
            auto const total = b.general();
            b.add(block, total, returned, target);
            b.ret(block, total);
        },
        memory);
    CHECK(nameOf(allocation.after, call, 0).kind == RegisterKind::Physical);
    CHECK(std::ranges::any_of(allocation.after.movesBefore(call),
                              [](Move const& move) { return move.to.kind == LocationKind::Slot; }));
}

TEST_CASE("LinearScan.leavesUnreachableBlocksAlone")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto orphan = uint32_t { 0 };
    auto const allocation = allocateBuilt(
        [&](Builder& b) {
            auto& f = b.function();
            auto const entry = f.createBlock(Hotness::Normal);
            auto const unreachable = f.createBlock(Hotness::Normal);
            auto const x = b.general();
            b.constant(entry, x, 1);
            b.ret(entry, x);
            auto const y = b.general();
            orphan = b.constant(unreachable, y, 2);
            b.ret(unreachable, y);
        },
        FakeTarget {},
        64,
        memory);
    REQUIRE(allocation.frame.has_value());
    // linearize() leaves the block out: it is never encoded, and keeps its virtual registers.
    CHECK(nameOf(allocation.after, orphan, 0).kind == RegisterKind::Virtual);
}

TEST_CASE("LinearScan.fixedCopiesDoNotDependOnOperandOrder")
{
    // Two copies into fixed registers in one gap, collected in operand order: the same call, its operands the
    // other way round, gets the same moves (std::sort leaves equal keys in an order each standard library
    // picks differently, so the order the copies come in must not decide it).
    auto const build = [](FakeOpcode opcode) {
        return [opcode](Builder& b) {
            auto& f = b.function();
            auto const block = f.createBlock(Hotness::Normal);
            auto const x = b.general();
            auto const y = b.general();
            b.constant(block, x, 1);
            b.constant(block, y, 2);
            // x into g0 and y into g1 either way: the swapped row lists g1 first.
            if (opcode == FakeOpcode::PassArguments)
                b.append(block, opcode, { use(f, x), use(f, y) });
            else
                b.append(block, opcode, { use(f, y), use(f, x) });
            auto const z = b.general();
            b.add(block, z, x, y);
            b.ret(block, z);
        };
    };
    auto first = std::pmr::monotonic_buffer_resource {};
    auto second = std::pmr::monotonic_buffer_resource {};
    auto const inOrder = allocateAndCheck(build(FakeOpcode::PassArguments), first);
    auto const swapped = allocateAndCheck(build(FakeOpcode::PassArgumentsSwapped), second);
    CHECK(*inOrder.frame == *swapped.frame);
    REQUIRE(inOrder.after.instructionCount() == swapped.after.instructionCount());
    auto copies = std::size_t { 0 };
    for (auto const instruction: std::views::iota(uint32_t { 0 }, inOrder.after.instructionCount()))
    {
        CHECK(std::ranges::equal(inOrder.after.movesBefore(instruction),
                                 swapped.after.movesBefore(instruction)));
        copies += inOrder.after.movesBefore(instruction).size();
    }
    CHECK(copies >= 2);
}

TEST_CASE("LinearScan.keepsNothingInACallerSavedRegisterAcrossAPoll")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    // A poll clobbers what the host convention does not preserve: g0, f0 and f1 (g1 is callee-saved there).
    // A General value lives across it in g1, untouched; a Float one has no register to live in, so its slot.
    auto definition = uint32_t { 0 };
    auto read = uint32_t { 0 };
    auto const allocation = allocateAndCheck(
        [&](Builder& b) {
            auto& f = b.function();
            auto const block = f.createBlock(Hotness::Normal);
            auto const kept = b.general();
            definition = b.constant(block, kept, 5);
            auto const real = b.floating();
            b.constantFloat(block, real);
            b.poll(block);
            auto const copied = b.floating();
            b.copyFloat(block, copied, real);
            auto const sum = b.general();
            read = b.add(block, sum, kept, kept);
            b.ret(block, sum);
        },
        memory);
    CHECK(nameOf(allocation.after, definition, 0) == generalRegister(1));
    CHECK(nameOf(allocation.after, read, 1) == generalRegister(1));
    CHECK(spillSlots(*allocation.frame) == 1);
    auto const host =
        isoline::compiler::machine::callerSavedRegisters(FakeTarget {}, Convention::SystemV, memory);
    CHECK(host.size() == 3);
    CHECK(std::ranges::find(host, PhysicalRegister { .registerClass = RegisterClass::General, .number = 1 })
          == host.end());
}

TEST_CASE("LinearScan.framePointerKeepsCallsAligned")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    // One spill slot, the frame pointer's save and the return address: 24 bytes, aligned to 32, so the
    // prologue's push of the frame pointer leaves every call aligned. Without the frame pointer's row the
    // frame would be 16, and a call after `push rbp` would leave 8 off.
    auto const allocation = allocateAndCheck(
        [](Builder& b) {
            auto& f = b.function();
            auto const block = f.createBlock(Hotness::Normal);
            auto const x = b.general();
            b.constant(block, x, 1);
            b.call(block);
            auto const y = b.general();
            b.add(block, y, x, x);
            b.ret(block, y);
        },
        memory);
    auto const& frame = *allocation.frame;
    auto const stack = isoline::compiler::machine::testing::FakeStackRule;
    CHECK(spillSlots(frame) == 1);
    CHECK(frame.totalBytes == 32);
    CHECK(frame.totalBytes % stack.alignment == 0);
    CHECK(frame.spillOffset + frame.spillBytes + stack.framePointerBytes + stack.returnAddressBytes
          <= frame.totalBytes);
}

TEST_CASE("LinearScan.honoursCancellationMidScan")
{
    // Cancelled at the scan's tenth placement, long after liveness and the facts: the scan's next poll, at
    // 4'096 intervals, stops it.
    auto const run = [](uint32_t after) {
        auto memory = std::pmr::monotonic_buffer_resource {};
        auto cancellation = CompileCancellation {};
        auto const target = CancellingTarget { cancellation, after };
        auto function = MachineFunction { memory };
        auto builder = Builder { function };
        manyLiveValues(5'000)(builder);
        auto const result =
            LinearScanAllocator { CompileLimits {} }.allocate(function, target, 5'000, cancellation, memory);
        return std::pair { result.has_value() ? std::optional<CompileErrorCode> {}
                                              : std::optional { result.error().code },
                           target.asked() };
    };
    auto const [whole, placements] = run(0); // never cancelled: every placement
    CHECK_FALSE(whole.has_value());
    auto const [cancelled, asked] = run(10);
    CHECK(cancelled == CompileErrorCode::Cancelled);
    CHECK(asked > 10);
    CHECK(asked < placements);
}

TEST_CASE("LinearScan.checkerRefusesAnUnsavedCalleeSavedRegister")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto const target = FakeTarget { FakeFrameRule, FakeSaves::SecondGeneral };
    auto const build = [](Builder& b) {
        auto& f = b.function();
        auto const block = f.createBlock(Hotness::Normal);
        auto const x = b.general();
        auto const y = b.general();
        b.constant(block, x, 1);
        b.constant(block, y, 2);
        auto const z = b.general();
        b.add(block, z, x, y);
        b.ret(block, z);
    };
    auto const allocation = allocateAndCheck(build, memory, target);
    auto unsaved = *allocation.frame;
    REQUIRE(unsaved.calleeSaved.countIn(RegisterClass::General) == 1);
    unsaved.calleeSaved = {}; // the frame still keeps its rules: the register is what is missing
    auto const checked = checkAssignment(allocation.before, allocation.after, target, unsaved, 64, memory);
    REQUIRE_FALSE(checked.has_value());
    CHECK(checked.error().kind == AssignmentFaultKind::UnsavedRegister);
}
