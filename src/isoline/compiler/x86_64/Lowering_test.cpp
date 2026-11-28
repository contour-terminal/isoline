// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory_resource>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/compiler/CompileLimits.hpp>
#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Guards.hpp>
#include <isoline/compiler/intermediate/testing/GuardEdges.hpp>
#include <isoline/compiler/machine/CodeBuffer.hpp>
#include <isoline/compiler/registerallocation/LinearScan.hpp>
#include <isoline/compiler/translation/testing/OpcodeSamples.hpp>
#include <isoline/compiler/x86_64/Lowering.hpp>
#include <isoline/compiler/x86_64/testing/Evaluator.hpp>
#include <isoline/compiler/x86_64/testing/Selection.hpp>
#include <isoline/runtime/Interrupt.hpp>

namespace
{

using isoline::binary::FuncType;
using isoline::binary::testing::code;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::memoryAccess;
using isoline::compiler::SpectreGuard;
using isoline::compiler::intermediate::Type;
using isoline::compiler::intermediate::testing::EdgeOutcome;
using isoline::compiler::machine::BlockId;
using isoline::compiler::machine::CodeBuffer;
using isoline::compiler::machine::CodeLabel;
using isoline::compiler::machine::FrameLayout;
using isoline::compiler::machine::Hotness;
using isoline::compiler::machine::MachineFunction;
using isoline::compiler::machine::ModuleLabels;
using isoline::compiler::machine::VirtualRegister;
using isoline::compiler::translation::testing::Bytes;
using isoline::compiler::translation::testing::ModuleBuilder;
using isoline::compiler::x86_64::Assembler;
using isoline::compiler::x86_64::CallConventions;
using isoline::compiler::x86_64::FloatWidth;
using isoline::compiler::x86_64::FunctionEmitter;
using isoline::compiler::x86_64::IntegerWidth;
using isoline::compiler::x86_64::Mnemonic;
using isoline::compiler::x86_64::Writer;
using isoline::compiler::x86_64::testing::evaluate;
using isoline::compiler::x86_64::testing::findRun;
using isoline::compiler::x86_64::testing::joined;
using isoline::compiler::x86_64::testing::namesOf;
using isoline::compiler::x86_64::testing::SelectionOptions;
using isoline::compiler::x86_64::testing::selectOf;
using isoline::runtime::TrapCode;
namespace x64 = isoline::compiler::x86_64;
namespace runtime = isoline::runtime;

using Names = std::vector<std::string_view>;

/// A scratch function one sequence is written into.
struct Scratch
{
    std::pmr::monotonic_buffer_resource memory {};
    MachineFunction function { memory };
    BlockId block = function.createBlock(Hotness::Normal);
    Writer writer { function, block };
};

/// The conventions under a System V host.
CallConventions systemV()
{
    return CallConventions { .internal = x64::WasmInternalConvention, .host = x64::SystemVConvention };
}

/// The names @p write appends to a scratch block.
Names shapeOf(std::function<void(Writer const&)> const& write)
{
    auto scratch = Scratch {};
    write(scratch.writer);
    return namesOf(scratch.function, scratch.block);
}

/// The rows @p builder's function encodes to under @p options: selected, allocated, encoded and resolved.
std::vector<std::string_view> encodedNamesOf(ModuleBuilder const& builder,
                                             SelectionOptions const& options = {})
{
    auto const selected = selectOf(builder, options);
    auto const trace = x64::testing::encodedOf(*selected);
    return namesOf(std::span<x64::TraceEntry const> { trace });
}

/// A module of one function (param i32 i32 i32) with @p body and a memory of @p pages pages.
ModuleBuilder moduleOf(Bytes const& body, uint64_t pages = 1)
{
    auto builder = ModuleBuilder {};
    auto const i32 = isoline::binary::I32;
    builder.type(FuncType { .params = { i32, i32, i32 }, .results = {} });
    builder.function(0, {}, body);
    builder.memory(isoline::binary::MemoryType {
        .limits = isoline::binary::Limits {
            .min = pages, .max = std::nullopt, .addressType = isoline::binary::AddressType::I32 } });
    return builder;
}

/// An encode-time harness: a function of one block, a buffer and an emitter over @p frame, its rows traced.
struct EncodeHarness
{
    explicit EncodeHarness(FrameLayout frame = FrameLayout { .totalBytes = 16 }): frame { frame } {}

    std::pmr::monotonic_buffer_resource memory {};
    MachineFunction function { memory };
    BlockId block = function.createBlock(Hotness::Normal);
    FrameLayout frame;
    CodeBuffer buffer { memory };
    std::vector<CodeLabel> blockLabels { buffer.createLabel() };
    std::vector<CodeLabel> functions { buffer.createLabel() };
    ModuleLabels labels { .functions = functions, .trapExit = buffer.createLabel() };
    std::pmr::vector<x64::TraceEntry> trace { &memory };
    Assembler assembler { buffer, blockLabels, &trace };
    FunctionEmitter emitter { assembler, function, frame, systemV(), labels, memory };

    [[nodiscard]] Names names() const
    {
        auto const all = namesOf(std::span<x64::TraceEntry const> { trace });
        return { all.begin(), all.end() };
    }
};

} // namespace

TEST_CASE("Lowering.switchTableClampsTheIndex")
{
    // br_table 0 1 2 inside three blocks: a jump table of three targets and the fallback.
    auto const body = code({ { 0x02, 0x40, 0x02, 0x40, 0x02, 0x40 },
                             localGet(0),
                             { 0x0E, 0x02, 0x00, 0x01, 0x02 },
                             { 0x0B, 0x0B, 0x0B } });
    SECTION("SpectreGuard::Enabled: cmovae to the fallback's entry before the table load")
    {
        auto const names = encodedNamesOf(moduleOf(body));
        INFO(joined(names));
        auto const clamped = Names { "Mov32RR", "Mov32RI",     "Cmp64RR", "Cmovcc64RR.ae",
                                     "Lea64RM", "Movsxd64M32", "Add64RR", "Jmp64R" };
        CHECK(findRun(names, clamped).has_value());
    }
    SECTION("SpectreGuard::Disabled: a bound check branches to the fallback")
    {
        auto const names =
            encodedNamesOf(moduleOf(body), SelectionOptions { .spectre = SpectreGuard::Disabled });
        INFO(joined(names));
        auto const checked =
            Names { "Mov32RR", "Cmp32RI", "Jcc.ae", "Lea64RM", "Movsxd64M32", "Add64RR", "Jmp64R" };
        CHECK(findRun(names, checked).has_value());
        CHECK_FALSE(std::ranges::contains(names, std::string_view { "Cmovcc64RR.ae" }));
    }
}

TEST_CASE("Lowering.pollTestsBeforeDecrement")
{
    // A loop's back edge polls its countdown in a register: an interrupt (non-zero flag) and an empty
    // countdown (zero, tested before the decrement) each take the slow path; the entry polls the stored one.
    auto const names = encodedNamesOf(moduleOf(code({ { 0x03, 0x40 }, localGet(0), { 0x0D, 0x00, 0x0B } })));
    INFO(joined(names));
    auto const inRegister = Names { "Mov64RM", "Cmp8MI", "Jcc.ne", "Test64RR", "Jcc.e", "Dec64R" };
    auto const stored =
        Names { "Mov64RM", "Cmp8MI", "Jcc.ne", "Mov64RM", "Test64RR", "Jcc.e", "Dec64R", "Mov64MR" };
    CHECK(findRun(names, inRegister).has_value());
    CHECK(findRun(names, stored).has_value());
    // A stored poll in the body, not the prologue's: the same tests, through rcx.
    auto late = ModuleBuilder {};
    late.type(FuncType {});
    late.function(0, {}, {});
    auto const inBody = encodedNamesOf(
        late, SelectionOptions { .rewrite = [](isoline::compiler::intermediate::Function& function) {
            auto replacement = isoline::compiler::intermediate::Function { function.memory() };
            auto builder = isoline::compiler::intermediate::Builder { replacement };
            builder.switchTo(builder.createBlock());
            auto const site = std::to_underlying(isoline::runtime::PollSite::LoopBackEdge);
            std::ignore = builder.emit(isoline::compiler::intermediate::Operation::PollStored,
                                       {},
                                       {},
                                       std::array { static_cast<uint64_t>(site) });
            builder.ret({});
            function = std::move(replacement);
        } });
    INFO(joined(inBody));
    CHECK(findRun(inBody, stored).has_value());
    CHECK_FALSE(std::ranges::contains(inBody, std::string_view { "Sub32RI8" }));
    // The slow path stores the countdown, calls PollSlow, leaves on a failed status and reloads it.
    auto const slow = Names { "Mov64MR", "Mov64RR",  "Mov64RR", "Mov64RI", "Mov64RM",
                              "Call64M", "Test64RR", "Jcc.ne",  "Mov64RM", "Jmp" };
    CHECK(findRun(names, slow).has_value());
}

TEST_CASE("Lowering.enterFrameChecksBeforeAllocating")
{
    SECTION("a small frame: the native check, then one sub")
    {
        auto harness = EncodeHarness { FrameLayout { .totalBytes = 64 } };
        x64::enterFrame(harness.emitter);
        CHECK(
            harness.names()
            == Names { "Push64R", "Mov64RR", "Mov64RR", "Sub64RI", "Jcc.b", "Cmp64RM", "Jcc.b", "Sub64RI" });
    }
    SECTION("a frame past a page: the native check, then the probe loop")
    {
        auto harness = EncodeHarness { FrameLayout { .totalBytes = 16 + (3 * x64::ProbePageBytes) + 32 } };
        x64::enterFrame(harness.emitter);
        CHECK(harness.names()
              == Names { "Push64R",
                         "Mov64RR",
                         "Mov64RR",
                         "Sub64RI",
                         "Jcc.b",
                         "Cmp64RM",
                         "Jcc.b",
                         "Mov64RI",
                         "Sub64RI",
                         "Test64MR",
                         "Dec64R",
                         "Jcc.ne",
                         "Sub64RI" });
    }
    SECTION("saved registers after the frame is allocated")
    {
        auto const rbx = x64::physical(x64::Gpr::Rbx);
        auto harness = EncodeHarness { FrameLayout {
            .totalBytes = 32, .calleeSaved = isoline::compiler::machine::RegisterSet::of({ rbx }) } };
        x64::enterFrame(harness.emitter);
        CHECK(harness.names()
              == Names { "Push64R",
                         "Mov64RR",
                         "Mov64RR",
                         "Sub64RI",
                         "Jcc.b",
                         "Cmp64RM",
                         "Jcc.b",
                         "Sub64RI",
                         "Mov64MR" });
        auto epilogue = EncodeHarness { harness.frame };
        x64::leaveFrame(epilogue.emitter);
        CHECK(epilogue.names() == Names { "Mov64RM", "Mov64RR", "Pop64R", "Ret" });
    }
    SECTION("a function's entry: the poll, the depth, the stack values, then the native check and the frame")
    {
        // A callee of 5'000 locals: its frame cost is past any small maxStackValues, so its stack charge
        // traps; it must do so after the FunctionEntry poll (an interrupt pending wins) and before the native
        // check, as the reference orders them (Review Focus 1).
        auto const i32 = isoline::binary::I32;
        auto builder = ModuleBuilder {};
        builder.type(FuncType {});
        builder.function(0, std::vector<isoline::binary::ValueType>(5'000, i32), {});
        auto const names = encodedNamesOf(builder);
        INFO(joined(names));
        auto const prologue =
            Names { "Push64R", "Mov64RR", "Mov64RM", "Cmp8MI",   "Jcc.ne", "Mov64RM", "Test64RR", "Jcc.e",
                    "Dec64R",  "Mov64MR", "Mov32RM", "Sub32RI8", "Jcc.b",  "Mov32MR", "Mov64RM",  "Sub64RI",
                    "Jcc.b",   "Mov64MR", "Mov64RR", "Sub64RI",  "Jcc.b",  "Cmp64RM", "Jcc.b" };
        REQUIRE(names.size() > prologue.size());
        CHECK(std::ranges::equal(std::span { names }.first(prologue.size()), prologue));
        // Nothing of the entry is left in the body: one depth charge, one interrupt test.
        CHECK(std::ranges::count(names, std::string_view { "Sub32RI8" }) == 1);
        CHECK(std::ranges::count(names, std::string_view { "Cmp8MI" }) == 1);
        // The entry poll's slow path keeps every argument register on the reserve around PollSlow.
        auto saves = Names { "Sub64RI" };
        saves.insert(saves.end(), 6, "Mov64MR");
        saves.insert(saves.end(), 8, "MovsdMR");
        CHECK(findRun(names, saves).has_value());
        auto const call =
            Names { "Mov64RR", "Mov64RR", "Mov64RI", "Mov64RM", "Call64M", "Test64RR", "Jcc.ne" };
        CHECK(findRun(names, call).has_value());
        auto restores = Names(6, "Mov64RM");
        restores.insert(restores.end(), 8, "MovsdRM");
        restores.insert(restores.end(), { "Add64RI", "Jmp" });
        CHECK(findRun(names, restores).has_value());
        // The selected function holds the Entry pseudo-instruction first, and no poll or charge of its own.
        auto const selected = selectOf(builder);
        auto const selectedNames = namesOf(*selected->function);
        REQUIRE_FALSE(selectedNames.empty());
        CHECK(selectedNames.front() == "entry");
        CHECK_FALSE(std::ranges::contains(selectedNames, std::string_view { "poll-stored" }));
        CHECK(selected->statistics.hitsOf(x64::SelectionRowId::PollStoredAtEntry) == 1);
        CHECK(selected->statistics.hitsOf(x64::SelectionRowId::EnterFrameAtEntry) == 1);
    }
    SECTION("a frame and a cost past INT32_MAX: each through a register")
    {
        auto harness = EncodeHarness { FrameLayout { .totalBytes = 16 + (uint64_t { 3 } << 30U) } };
        auto const entry =
            std::array { x64::immediate(0), x64::immediate(1), x64::immediate(int64_t { 1 } << 33U) };
        std::ignore = harness.function.append(harness.block, x64::opcodeOf(x64::Pseudo::Entry), entry);
        x64::enterFrame(harness.emitter);
        auto const names = harness.names();
        INFO(joined(names));
        auto const charge = Names { "Mov32RM", "Sub32RI8", "Jcc.b", "Mov32MR", "Mov64RM",
                                    "Mov64RI", "Sub64RR",  "Jcc.b", "Mov64MR" };
        CHECK(findRun(names, charge).has_value());
        auto const native = Names { "Mov64RR", "Mov64RI", "Sub64RR", "Jcc.b", "Cmp64RM", "Jcc.b", "Mov64RI" };
        CHECK(findRun(names, native).has_value());
        // No FunctionEntry poll was asked for.
        CHECK_FALSE(std::ranges::contains(names, std::string_view { "Cmp8MI" }));
    }
    SECTION("everything before the frame fits the reserve every caller keeps below its own")
    {
        // A callee runs its entry on the StubReserveBytes its caller's native check left free: the return
        // address, the saved rbp, the slow path's saves and outgoing area, and the stub's own stack.
        auto memory = std::pmr::monotonic_buffer_resource {};
        for (auto const* host: { &x64::SystemVConvention, &x64::Windows64Convention })
        {
            auto const conventions =
                CallConventions { .internal = x64::WasmInternalConvention, .host = *host };
            auto const bytes = x64::entryPathBytes(conventions, memory);
            CHECK(bytes % 16 == 0);
            CHECK(bytes >= 16 + (8 * 14));
            CHECK(bytes + x64::StubStackBudgetBytes <= x64::StubReserveBytes);
        }
    }
}

TEST_CASE("Lowering.sequencesHaveTheExpectedShape")
{
    struct Case
    {
        std::string_view name;
        std::function<void(Writer const&)> write;
        Names shape;
    };
    // A Ref conversion's emit and registers over a Writer.
    auto const refEmit = [](Writer const& w) {
        return [&w](Mnemonic mnemonic,
                    std::initializer_list<isoline::compiler::machine::MachineOperand> operands) {
            w.emit(mnemonic, operands);
        };
    };
    auto const refRegister = [](Writer const& w, std::array<VirtualRegister, 3> const& registers) {
        return [&w, registers](x64::RefRegister which, isoline::compiler::machine::OperandRole role) {
            auto const reg = registers[static_cast<std::size_t>(which)];
            switch (role)
            {
                case isoline::compiler::machine::OperandRole::Def: return w.def(reg);
                case isoline::compiler::machine::OperandRole::UseDef: return w.useDef(reg);
                default: return w.use(reg);
            }
        };
    };
    auto const general = [](Writer const& w) {
        return w.general();
    };
    auto const vector = [](Writer const& w) {
        return w.vector();
    };
    auto const cases = std::vector<Case> {
        { "floatMin (single)",
          [&](Writer const& w) { std::ignore = x64::floatMin(w, FloatWidth::Single, vector(w), vector(w)); },
          { "MovapsRR",
            "MinssRR",
            "MovapsRR",
            "MinssRR",
            "OrpsRR",
            "MovapsRR",
            "AddssRR",
            "MovdRX",
            "MovdRX",
            "UcomissRR",
            "Cmovcc32RR",
            "MovdXR" } },
        { "floatMax (double)",
          [&](Writer const& w) { std::ignore = x64::floatMax(w, FloatWidth::Double, vector(w), vector(w)); },
          { "MovapsRR",
            "MaxsdRR",
            "MovapsRR",
            "MaxsdRR",
            "AndpsRR",
            "MovapsRR",
            "AddsdRR",
            "MovqRX",
            "MovqRX",
            "UcomisdRR",
            "Cmovcc64RR",
            "MovqXR" } },
        { "nearestSse41",
          [&](Writer const& w) {
              std::ignore = x64::nearestSse41(w, FloatWidth::Single, x64::RoundingMode::Nearest, vector(w));
          },
          { "RoundssRRI" } },
        { "nearestBaseline (System V)",
          [&](Writer const& w) {
              std::ignore = x64::nearestBaseline(w,
                                                 systemV(),
                                                 FloatWidth::Double,
                                                 isoline::compiler::FloatRoundOperation::F64Nearest,
                                                 vector(w));
          },
          { "MovqRX",
            "Mov32RI",
            "Mov64RR",
            "Mov64RR",
            "call-stub",
            "Test64RR",
            "trap-if-pending",
            "MovqXR" } },
        { "nearestBaseline (Microsoft x64: the bits on the stack, the result through a hidden pointer)",
          [&](Writer const& w) {
              std::ignore = x64::nearestBaseline(w,
                                                 CallConventions { .internal = x64::WasmInternalConvention,
                                                                   .host = x64::Windows64Convention },
                                                 FloatWidth::Double,
                                                 isoline::compiler::FloatRoundOperation::F64Nearest,
                                                 vector(w));
          },
          { "MovqRX",
            "Mov32RI",
            "Mov64RR",
            "Mov64RR",
            "Mov64MR",
            "Lea64RM",
            "call-stub",
            "Mov64RM",
            "Mov64RM",
            "Test64RR",
            "trap-if-pending",
            "MovqXR" } },
        { "truncToI32Checked (single, signed)",
          [&](Writer const& w) {
              std::ignore = x64::truncToI32Checked(w, FloatWidth::Single, x64::Signedness::Signed, vector(w));
          },
          { "UcomissRR",
            "trap-if",
            "Mov32RI",
            "MovdXR",
            "UcomissRR",
            "trap-if",
            "Mov32RI",
            "MovdXR",
            "UcomissRR",
            "trap-if",
            "Cvttss2si32" } },
        { "truncSatF64ToU64",
          [&](Writer const& w) { std::ignore = x64::truncSatF64ToU64(w, vector(w)); },
          { "Mov64RI", "MovqXR",    "Cvttsd2si64", "MovapsRR",   "SubsdRR",   "Cvttsd2si64", "Mov64RI",
            "Xor64RR", "UcomisdRR", "Cmovcc64RR",  "Mov64RI",    "MovqXR",    "Mov64RI32",   "Mov32RI",
            "Mov32RI", "MovqXR",    "UcomisdRR",   "Cmovcc64RR", "UcomisdRR", "Cmovcc64RR" } },
        { "convertU64ToF32",
          [&](Writer const& w) { std::ignore = x64::convertU64ToF32(w, general(w)); },
          { "Mov64RR",
            "Shr64RI8",
            "Mov64RR",
            "And64RI8",
            "Or64RR",
            "Cvtsi2ss64",
            "AddssRR",
            "Cvtsi2ss64",
            "MovdRX",
            "MovdRX",
            "Test64RR",
            "Cmovcc32RR",
            "MovdXR" } },
        { "popcntBaseline (32)",
          [&](Writer const& w) { std::ignore = x64::popcntBaseline(w, IntegerWidth::Bits32, general(w)); },
          { "Mov32RR",
            "Shr32RI8",
            "And32RI",
            "Mov32RR",
            "Sub32RR",
            "Mov32RR",
            "Shr32RI8",
            "And32RI",
            "And32RI",
            "Add32RR",
            "Mov32RR",
            "Shr32RI8",
            "Add32RR",
            "And32RI",
            "Imul32RRI",
            "Shr32RI8" } },
        { "clzBaseline (32)",
          [&](Writer const& w) { std::ignore = x64::clzBaseline(w, IntegerWidth::Bits32, general(w)); },
          { "Mov32RI", "Bsr32RR", "Cmovcc32RR", "Xor32RI8" } },
        { "ctzBaseline (64)",
          [&](Writer const& w) { std::ignore = x64::ctzBaseline(w, IntegerWidth::Bits64, general(w)); },
          { "Mov32RI", "Bsf64RR", "Cmovcc64RR" } },
        { "divSigned (32)",
          [&](Writer const& w) {
              std::ignore = x64::divSigned(w, IntegerWidth::Bits32, general(w), general(w));
          },
          { "Test32RR",
            "trap-if",
            "Mov32RR",
            "Xor32RI",
            "Mov32RR",
            "Add32RI8",
            "Or32RR",
            "trap-if",
            "Mov32RR",
            "Cdq",
            "Idiv32R" } },
        { "remSigned (64): a divisor of -1 replaced by 1 before idiv, which never faults",
          [&](Writer const& w) {
              std::ignore = x64::remSigned(w, IntegerWidth::Bits64, general(w), general(w));
          },
          { "Test64RR",
            "trap-if",
            "Mov64RR",
            "Mov32RI",
            "Cmp64RI8",
            "Cmovcc64RR",
            "Mov64RR",
            "Cqo",
            "Idiv64R" } },
        { "canonicalizeNan (single)",
          [&](Writer const& w) { std::ignore = x64::canonicalizeNan(w, FloatWidth::Single, vector(w)); },
          { "MovdRX", "Mov32RI", "UcomissRR", "Cmovcc32RR", "MovdXR" } },
        { "checkMemory32 (Enabled)",
          [&](Writer const& w) {
              std::ignore = x64::checkMemory32(
                  w,
                  x64::MemoryCheck {
                      .index = general(w), .base = general(w), .bound = general(w), .offset = 8, .size = 4 });
          },
          { "Mov32RR", "Lea64RM", "Lea64RM", "Mov32RI", "Cmp64RR", "trap-if", "Cmovcc64RR" } },
        { "checkMemory32 (Disabled)",
          [&](Writer const& w) {
              std::ignore = x64::checkMemory32(w,
                                               x64::MemoryCheck { .index = general(w),
                                                                  .base = general(w),
                                                                  .bound = general(w),
                                                                  .offset = 8,
                                                                  .size = 4,
                                                                  .spectre = SpectreGuard::Disabled });
          },
          { "Mov32RR", "Lea64RM", "Lea64RM", "Cmp64RR", "trap-if" } },
        { "checkMemory32 of an offset past INT32_MAX: mov imm and add, never a lea",
          [&](Writer const& w) {
              std::ignore = x64::checkMemory32(w,
                                               x64::MemoryCheck { .index = general(w),
                                                                  .base = general(w),
                                                                  .bound = general(w),
                                                                  .offset = 0x8000'0000U,
                                                                  .size = 4 });
          },
          { "Mov32RR",
            "Mov64RR",
            "Mov32RI",
            "Add64RR",
            "Mov64RR",
            "Add64RR",
            "Mov32RI",
            "Add64RR",
            "Mov32RI",
            "Cmp64RR",
            "trap-if",
            "Cmovcc64RR" } },
        { "checkMemory64 (Enabled): the carry folded into the one compare",
          [&](Writer const& w) {
              std::ignore = x64::checkMemory64(
                  w,
                  x64::MemoryCheck {
                      .index = general(w), .base = general(w), .bound = general(w), .offset = 8, .size = 4 });
          },
          { "Lea64RM",
            "Mov64RR",
            "Add64RI",
            "Mov64RI32",
            "Cmovcc64RR",
            "Mov32RI",
            "Cmp64RR",
            "trap-if",
            "Cmovcc64RR" } },
        { "guardAddress (Enabled)",
          [&](Writer const& w) { std::ignore = x64::guardAddress(w, general(w), 4, SpectreGuard::Enabled); },
          { "Lea64RM", "Test64RR", "Cmovcc64RR" } },
        { "guardAddress (Disabled)",
          [&](Writer const& w) { std::ignore = x64::guardAddress(w, general(w), 4, SpectreGuard::Disabled); },
          { "Lea64RM" } },
        { "inBoundsAddress",
          [&](Writer const& w) { std::ignore = x64::inBoundsAddress(w, general(w), 0x7FFF'FFFF); },
          { "Lea64RM" } },
        { "inBoundsAddress of an offset past INT32_MAX (ledger carry from T8)",
          [&](Writer const& w) { std::ignore = x64::inBoundsAddress(w, general(w), 0x8000'0000U); },
          { "Mov64RR", "Mov32RI", "Add64RR" } },
        { "inBoundsAddress of an offset past 2^32",
          [&](Writer const& w) { std::ignore = x64::inBoundsAddress(w, general(w), 0x1'0000'0010U); },
          { "Mov64RR", "Mov64RI", "Add64RR" } },
        { "checkTable (UndefinedElement, Enabled)",
          [&](Writer const& w) {
              std::ignore = x64::checkTable(w,
                                            x64::TableCheck { .index = general(w),
                                                              .indexType = Type::I32,
                                                              .data = general(w),
                                                              .size = general(w),
                                                              .code = TrapCode::UndefinedElement });
          },
          { "Mov32RR",
            "Mov64RR",
            "Shl64RI8",
            "Lea64RM",
            "Mov32RI",
            "Cmp64RR",
            "trap-if-subject",
            "Cmovcc64RR" } },
        { "checkTable (OutOfBoundsTableAccess): no subject",
          [&](Writer const& w) {
              std::ignore = x64::checkTable(w,
                                            x64::TableCheck { .index = general(w),
                                                              .indexType = Type::I64,
                                                              .data = general(w),
                                                              .size = general(w),
                                                              .code = TrapCode::OutOfBoundsTableAccess });
          },
          { "Mov64RR", "Shl64RI8", "Lea64RM", "Mov32RI", "Cmp64RR", "trap-if", "Cmovcc64RR" } },
        { "checkDispatch (Enabled)",
          [&](Writer const& w) {
              std::ignore = x64::checkDispatch(w,
                                               x64::DispatchCheck { .reference = general(w),
                                                                    .index = general(w),
                                                                    .indexType = Type::I32,
                                                                    .view = general(w),
                                                                    .code = TrapCode::UndefinedElement });
          },
          { "Mov32RM",
            "Mov64RR",
            "Shr64RI8",
            "Mov32RR",
            "Mov32RI",
            "Cmp64RR",
            "trap-if-subject",
            "Mov64RR",
            "Cmovcc64RR",
            "Mov64RR",
            "Shr64RI8",
            "Mov64RM",
            "Mov64RM",
            "Mov64RR",
            "And64RI",
            "Imul64RRI",
            "Lea64RM",
            "Cmp64RR",
            "Cmovcc64RR" } },
        { "loadRef: field by field",
          [&](Writer const& w) {
              std::ignore = x64::loadRef(
                  w,
                  general(w),
                  x64::RefOffsets { .kind = 0, .heapKind = 4, .heapTypeIndex = 8, .address = 12 });
          },
          { "Movzx32M8", "Movzx32M8", "Shl32RI8", "Or32RR", "Mov32RM", "Shl64RI8", "Or64RR" } },
        { "storeRef: field by field, the type index 0",
          [&](Writer const& w) {
              x64::storeRef(w,
                            general(w),
                            general(w),
                            x64::RefOffsets { .kind = 0, .heapKind = 4, .heapTypeIndex = 8, .address = 12 });
          },
          { "Mov8MR", "Mov64RR", "Shr64RI8", "Mov8MR", "Mov32MI", "Mov64RR", "Shr64RI8", "Mov32MR" } },
        { "chargeFrame",
          [&](Writer const& w) { x64::chargeFrame(w, 5); },
          { "Mov32RM", "Sub32RI8", "trap-if", "Mov32MR", "Mov64RM", "Sub64RI", "trap-if", "Mov64MR" } },
        { "releaseFrame",
          [&](Writer const& w) { x64::releaseFrame(w, 5); },
          { "Mov32RM", "Add32RI8", "Mov32MR", "Mov64RM", "Add64RI", "Mov64MR" } },
        { "refWordFromCell",
          [&](Writer const& w) {
              auto const registers = std::array { general(w), general(w), general(w) };
              x64::refWordFromCell(
                  refEmit(w), refRegister(w, registers), w.at(registers[0]), w.at(registers[0], 8));
          },
          { "Movzx32M16", "Mov32RM", "Shl64RI8", "Or64RR" } },
        { "refWordToCell",
          [&](Writer const& w) {
              auto const registers = std::array { general(w), general(w), general(w) };
              x64::refWordToCell(
                  refEmit(w), refRegister(w, registers), w.at(registers[0]), w.at(registers[0], 8));
          },
          { "Movzx32R16", "Mov64MR", "Mov64RR", "Shr64RI8", "Mov64MR" } },
        { "chargeFrame past INT32_MAX",
          [&](Writer const& w) { x64::chargeFrame(w, uint64_t { 1 } << 33U); },
          { "Mov32RM",
            "Sub32RI8",
            "trap-if",
            "Mov32MR",
            "Mov64RM",
            "Mov64RI",
            "Sub64RR",
            "trap-if",
            "Mov64MR" } },
        { "releaseFrame past INT32_MAX",
          [&](Writer const& w) { x64::releaseFrame(w, uint64_t { 1 } << 33U); },
          { "Mov32RM", "Add32RI8", "Mov32MR", "Mov64RM", "Mov64RI", "Add64RR", "Mov64MR" } },
    };
    for (auto const& entry: cases)
    {
        INFO(entry.name);
        auto const shape = shapeOf(entry.write);
        INFO(joined(shape));
        CHECK(shape == entry.shape);
    }
    SECTION("operands and rows looked up at run time")
    {
        // No truncation row for another operation; a Float register's operand names that register.
        for (auto const operation: { isoline::compiler::intermediate::Operation::I32Add,
                                     isoline::compiler::intermediate::Operation::I32TruncF32S })
            CHECK(x64::truncationOf(operation).has_value()
                  == (operation == isoline::compiler::intermediate::Operation::I32TruncF32S));
        for (auto const reg: { x64::Xmm::Xmm0, x64::Xmm::Xmm15 })
        {
            auto const operand = x64::physicalOperand(reg);
            CHECK(std::get<isoline::compiler::machine::RegisterOperand>(operand).name.physicalRegister()
                  == x64::physical(reg));
        }
    }
    SECTION("trap sites write pending in one store")
    {
        auto harness = EncodeHarness {};
        x64::trapSite(harness.emitter, TrapCode::Unreachable, std::nullopt);
        x64::trapSite(harness.emitter, TrapCode::UndefinedElement, x64::physical(x64::Gpr::Rcx));
        CHECK(harness.names() == Names { "Mov32MI", "Jmp", "Mov32MI", "Mov64MR", "Jmp" });
    }
}

TEST_CASE("Lowering.guardsHoldOnEveryEdgeRow")
{
    // Every GuardTable row's lowering executed on every edge row of intermediate/testing/GuardEdges.hpp, past
    // its trap as a mispredicted branch would run: the trap where the contract traps, and under
    // SpectreGuard::Enabled the pointer its select leaves, 0 there (ledger carry from T6).
    constexpr auto Base = uint64_t { 0x10'0000 };
    SECTION("checkMemory")
    {
        for (auto const& edge: isoline::compiler::intermediate::testing::MemoryGuardEdges)
            for (auto const spectre: { SpectreGuard::Enabled, SpectreGuard::Disabled })
            {
                INFO("index " << edge.index << " offset " << edge.offset << " size " << edge.size << " bound "
                              << edge.bound << " spectre " << static_cast<int>(spectre));
                auto scratch = Scratch {};
                auto const index = scratch.writer.general();
                auto const base = scratch.writer.general();
                auto const bound = scratch.writer.general();
                auto const check = x64::MemoryCheck { .index = index,
                                                      .base = base,
                                                      .bound = bound,
                                                      .offset = edge.offset,
                                                      .size = edge.size,
                                                      .spectre = spectre };
                auto const pointer = edge.indexType == Type::I32 ? x64::checkMemory32(scratch.writer, check)
                                                                 : x64::checkMemory64(scratch.writer, check);
                auto const evaluation = evaluate(scratch.function,
                                                 scratch.block,
                                                 { { std::to_underlying(index), edge.index },
                                                   { std::to_underlying(base), Base },
                                                   { std::to_underlying(bound), edge.bound } });
                CHECK(evaluation.misunderstood == 0);
                if (edge.outcome == EdgeOutcome::Traps)
                {
                    CHECK(evaluation.trap == TrapCode::OutOfBoundsMemoryAccess);
                    if (spectre == SpectreGuard::Enabled)
                        CHECK(evaluation.valueOf(pointer) == 0);
                }
                else
                {
                    CHECK_FALSE(evaluation.trap.has_value());
                    CHECK(evaluation.valueOf(pointer) == Base + edge.fits);
                }
            }
    }
    SECTION("guardAddress: 0 stays 0")
    {
        for (auto const address: { uint64_t { 0 }, Base })
            for (auto const offset: { uint64_t { 4 }, uint64_t { 0x9000'0000 } })
            {
                auto scratch = Scratch {};
                auto const input = scratch.writer.general();
                auto const moved = x64::guardAddress(scratch.writer, input, offset, SpectreGuard::Enabled);
                auto const evaluation =
                    evaluate(scratch.function, scratch.block, { { std::to_underlying(input), address } });
                CHECK(evaluation.misunderstood == 0);
                CHECK(evaluation.valueOf(moved) == (address == 0 ? 0 : address + offset));
            }
    }
    SECTION("checkTable")
    {
        constexpr auto Data = uint64_t { 0x2000'0000 };
        for (auto const& edge: isoline::compiler::intermediate::testing::TableGuardEdges)
            for (auto const code: { TrapCode::UndefinedElement, TrapCode::OutOfBoundsTableAccess })
            {
                INFO("index " << edge.index << " size " << edge.size);
                auto scratch = Scratch {};
                auto const index = scratch.writer.general();
                auto const data = scratch.writer.general();
                auto const size = scratch.writer.general();
                auto const pointer = x64::checkTable(scratch.writer,
                                                     x64::TableCheck { .index = index,
                                                                       .indexType = edge.indexType,
                                                                       .data = data,
                                                                       .size = size,
                                                                       .code = code });
                auto const evaluation = evaluate(scratch.function,
                                                 scratch.block,
                                                 { { std::to_underlying(index), edge.index },
                                                   { std::to_underlying(data), Data },
                                                   { std::to_underlying(size), edge.size } });
                CHECK(evaluation.misunderstood == 0);
                if (edge.outcome == EdgeOutcome::Traps)
                {
                    CHECK(evaluation.trap == code);
                    CHECK(evaluation.valueOf(pointer) == 0);
                    // UndefinedElement names the guarded index: an I32's low 32 bits.
                    if (code == TrapCode::UndefinedElement)
                        CHECK(evaluation.subject
                              == isoline::compiler::intermediate::guardedIndexOf(edge.indexType, edge.index));
                    else
                        CHECK_FALSE(evaluation.subject.has_value());
                }
                else
                {
                    CHECK_FALSE(evaluation.trap.has_value());
                    CHECK(evaluation.valueOf(pointer) == Data + (edge.fits * isoline::compiler::RefBytes));
                }
            }
    }
    SECTION("checkDispatch")
    {
        constexpr auto View = uint64_t { 0x3000'0000 };
        constexpr auto Chunks = uint64_t { 0x4000'0000 };
        auto const chunkOf = [](uint64_t chunk) {
            return uint64_t { 0x5000'0000 } + (chunk << 24U);
        };
        for (auto const& edge: isoline::compiler::intermediate::testing::DispatchGuardEdges)
        {
            INFO("reference " << edge.reference << " rows " << edge.rows);
            auto scratch = Scratch {};
            auto const reference = scratch.writer.general();
            auto const index = scratch.writer.general();
            auto const view = scratch.writer.general();
            auto const pointer =
                x64::checkDispatch(scratch.writer,
                                   x64::DispatchCheck { .reference = reference,
                                                        .index = index,
                                                        .indexType = Type::I32,
                                                        .view = view,
                                                        .code = TrapCode::UndefinedElement });
            auto const rowsAt =
                View + isoline::compiler::layoutOf(isoline::compiler::DispatchViewField::Rows).offset;
            auto const chunksAt =
                View + isoline::compiler::layoutOf(isoline::compiler::DispatchViewField::Chunks).offset;
            auto const read = [&](uint64_t address, uint32_t /*bytes*/) -> uint64_t {
                if (address == rowsAt)
                    return edge.rows;
                if (address == chunksAt)
                    return Chunks;
                REQUIRE(address >= Chunks);
                REQUIRE((address - Chunks) % 8 == 0);
                return chunkOf((address - Chunks) / 8);
            };
            auto const evaluation = evaluate(scratch.function,
                                             scratch.block,
                                             { { std::to_underlying(reference), edge.reference },
                                               { std::to_underlying(index), 0xFFFF'FFFF'0000'0007U },
                                               { std::to_underlying(view), View } },
                                             read);
            CHECK(evaluation.misunderstood == 0);
            if (edge.outcome == EdgeOutcome::Traps)
            {
                CHECK(evaluation.trap == TrapCode::UndefinedElement);
                CHECK(evaluation.subject == 7);
                CHECK(evaluation.valueOf(pointer) == 0);
            }
            else
            {
                CHECK_FALSE(evaluation.trap.has_value());
                auto const row = edge.fits % runtime::DispatchChunkRows;
                CHECK(evaluation.valueOf(pointer)
                      == chunkOf(edge.fits / runtime::DispatchChunkRows)
                             + (row * sizeof(runtime::FunctionDispatch)));
            }
        }
    }
}

TEST_CASE("Lowering.inBoundsAddressTakesWideOffsets")
{
    // A memory of at least 2 GiB and a constant index past INT32_MAX: bounds-check elimination proves it
    // in bounds, and its address is base + offset by mov imm and add, never a lea of a wrapped displacement
    // (ledger carry from T8).
    auto const body =
        code({ i32Const(static_cast<int32_t>(0x8000'0010U)), memoryAccess(0x28, 0, 0), { 0x1A } });
    auto const selected = selectOf(
        moduleOf(body, 32'769),
        SelectionOptions { .optimization = isoline::compiler::x86_64::testing::Optimization::Passes });
    auto const names = namesOf(*selected->function);
    INFO(joined(names));
    CHECK(selected->statistics.hitsOf(x64::SelectionRowId::InBoundsAddress) == 1);
    CHECK(findRun(names, Names { "Mov64RR", "Mov32RI", "Add64RR", "Mov32RM" }).has_value());
    // A large memarg offset on a checked access evaluates exactly: index 0x10 + offset 0xFFFF'FFF0 lies past
    // 2^32, within a bound past it.
    auto scratch = Scratch {};
    auto const index = scratch.writer.general();
    auto const base = scratch.writer.general();
    auto const bound = scratch.writer.general();
    auto const pointer = x64::checkMemory32(
        scratch.writer,
        x64::MemoryCheck { .index = index, .base = base, .bound = bound, .offset = 0xFFFF'FFF0U, .size = 4 });
    auto const evaluation = evaluate(scratch.function,
                                     scratch.block,
                                     { { std::to_underlying(index), 0x10 },
                                       { std::to_underlying(base), 0x1000 },
                                       { std::to_underlying(bound), 0x1'0000'0004U } });
    CHECK(evaluation.misunderstood == 0);
    CHECK_FALSE(evaluation.trap.has_value());
    CHECK(evaluation.valueOf(pointer) == 0x1000 + 0x1'0000'0000U);
}

TEST_CASE("Lowering.encodeRefusesWhatTheEncoderRefuses")
{
    // A row the encoder refuses (an immediate past its field) fails the function as a verification failure.
    auto harness = EncodeHarness {};
    auto const operands =
        std::array { x64::physicalOperand(x64::Gpr::Rax), x64::immediate(int64_t { 1 } << 40U) };
    std::ignore = harness.function.append(harness.block, x64::opcodeOf(Mnemonic::Cmp64RI), operands);
    std::ignore = harness.function.append(harness.block, x64::opcodeOf(x64::Pseudo::Return), {});
    auto const encoded = x64::encodeFunction(
        harness.function, harness.frame, systemV(), harness.labels, harness.buffer, nullptr, harness.memory);
    REQUIRE_FALSE(encoded.has_value());
    CHECK(encoded.error().code == isoline::compiler::CompileErrorCode::InternalVerificationFailed);
    CHECK(encoded.error().pass == "encoder");
}

TEST_CASE("Lowering.allocatorMovesTakeTheirForms")
{
    // Every kind of move the allocator inserts: register to register, a reload, a spill, and a slot to a slot
    // (push m64, pop m64), of either register file.
    using isoline::compiler::machine::Location;
    using isoline::compiler::machine::Move;
    using isoline::compiler::machine::RegisterClass;
    auto harness = EncodeHarness {};
    auto const instruction = harness.function.append(harness.block, x64::opcodeOf(x64::Pseudo::Return), {});
    auto const rax = Location::of(x64::physical(x64::Gpr::Rax));
    auto const rcx = Location::of(x64::physical(x64::Gpr::Rcx));
    auto const xmm0 = Location::of(x64::physical(x64::Xmm::Xmm0));
    auto const xmm1 = Location::of(x64::physical(x64::Xmm::Xmm1));
    auto const slot0 = Location::slot(RegisterClass::General, 0);
    auto const slot8 = Location::slot(RegisterClass::General, 8);
    auto const floatSlot = Location::slot(RegisterClass::Float, 16);
    auto const moves =
        std::array { Move { .from = rax, .to = rcx },       Move { .from = slot0, .to = rax },
                     Move { .from = rcx, .to = slot8 },     Move { .from = slot0, .to = slot8 },
                     Move { .from = xmm0, .to = xmm1 },     Move { .from = floatSlot, .to = xmm0 },
                     Move { .from = xmm1, .to = floatSlot } };
    harness.function.setMovesBefore(instruction, moves);
    auto const encoded = x64::encodeFunction(harness.function,
                                             FrameLayout { .totalBytes = 48, .spillBytes = 24 },
                                             systemV(),
                                             harness.labels,
                                             harness.buffer,
                                             &harness.trace,
                                             harness.memory);
    REQUIRE(encoded.has_value());
    auto const names = harness.names();
    INFO(joined(names));
    CHECK(
        findRun(
            names,
            Names { "Mov64RR", "Mov64RM", "Mov64MR", "Push64M", "Pop64M", "MovapsRR", "MovsdRM", "MovsdMR" })
            .has_value());
}

TEST_CASE("Lowering.dispatchedCallsReserveTheHostCallsArea")
{
    // A dispatched call may reach an import through its host-call path, which calls CallHost under the host
    // convention from the same frame bottom: its call site records that stub's outgoing area at least.
    auto memory = std::pmr::monotonic_buffer_resource {};
    for (auto const abi: { isoline::compiler::machine::OperatingSystemAbi::SystemV,
                           isoline::compiler::machine::OperatingSystemAbi::Windows })
    {
        auto const samples = isoline::compiler::translation::testing::everyCompiledOpcode();
        auto const sample = std::ranges::find_if(samples, [](auto const& candidate) {
            return candidate.info->opcode == isoline::binary::Opcode::CallIndirect;
        });
        REQUIRE(sample != samples.end());
        auto const selected = selectOf(sample->module, SelectionOptions { .abi = abi });
        auto const& function = *selected->function;
        auto const host = selected->target->conventions().host;
        auto const needed = x64::stubOutgoingBytes(host, 1, memory);
        auto dispatched = 0;
        for (auto const& site: function.callSites())
        {
            if (function.instruction(site.instruction).opcode != x64::opcodeOf(x64::Pseudo::CallDispatched))
                continue;
            ++dispatched;
            CHECK(site.outgoingBytes >= needed);
        }
        CHECK(dispatched == 1);
    }
}

TEST_CASE("Lowering.branchesFallThroughWhereTheyCan")
{
    // A two-way branch: an inverted jcc when its taken target follows, a jcc alone when its other target
    // follows, and a jcc then a jmp when neither does.
    std::pmr::monotonic_buffer_resource memory {};
    auto function = MachineFunction { memory };
    auto const entry = function.createBlock(Hotness::Normal);
    auto const ifTrue = function.createBlock(Hotness::Normal);
    auto const ifFalse = function.createBlock(Hotness::Normal);
    auto const other = function.createBlock(Hotness::Normal);
    auto const operands = std::array<isoline::compiler::machine::MachineOperand, 3> {
        x64::physicalOperand(x64::Gpr::Rax),
        isoline::compiler::machine::LabelOperand { .block = ifTrue },
        isoline::compiler::machine::LabelOperand { .block = ifFalse }
    };
    auto const branch = function.append(entry, x64::opcodeOf(x64::Pseudo::Branch), operands);
    struct Case
    {
        std::optional<BlockId> next;
        Names names;
    };
    for (auto const& [next, names]: { Case { .next = ifTrue, .names = { "Test32RR", "Jcc.e" } },
                                      Case { .next = ifFalse, .names = { "Test32RR", "Jcc.ne" } },
                                      Case { .next = other, .names = { "Test32RR", "Jcc.ne", "Jmp" } } })
    {
        auto buffer = CodeBuffer { memory };
        auto blockLabels = std::vector<CodeLabel> {};
        for ([[maybe_unused]] auto const block: std::views::iota(0, 4))
            blockLabels.push_back(buffer.createLabel());
        auto const functions = std::vector<CodeLabel> {};
        auto const labels = ModuleLabels { .functions = functions, .trapExit = buffer.createLabel() };
        auto trace = std::pmr::vector<x64::TraceEntry> { &memory };
        auto assembler = Assembler { buffer, blockLabels, &trace };
        auto const frame = FrameLayout { .totalBytes = 16 };
        auto emitter = FunctionEmitter { assembler, function, frame, systemV(), labels, memory };
        emitter.expand(branch, next);
        auto const traced = namesOf(std::span<x64::TraceEntry const> { trace });
        CHECK(Names { traced.begin(), traced.end() } == names);
    }
}
