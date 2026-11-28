// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/HostTarget.hpp>
#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/compiler/machine/CallingConvention.hpp>
#include <isoline/compiler/machine/CodeBuffer.hpp>
#include <isoline/compiler/machine/TargetDescription.hpp>
#include <isoline/compiler/x86_64/Lowering.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>
#include <isoline/compiler/x86_64/Target.hpp>
#include <isoline/execution/CodeMemory.hpp>
#include <isoline/execution/StackBounds.hpp>
#include <isoline/execution/testing/CompileOne.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>

namespace x64 = isoline::compiler::x86_64;

using isoline::compiler::EntryTrampoline;
using isoline::compiler::machine::OperatingSystemAbi;
using isoline::compiler::machine::PhysicalRegister;
using isoline::compiler::machine::TargetDescription;
using isoline::execution::PortableCodeMemory;
using isoline::execution::testing::CompiledInstance;
using isoline::execution::testing::hostConfig;
using isoline::execution::testing::hostRunsCompiledCode;
using isoline::execution::testing::moduleOf;
using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;

namespace
{

constexpr auto Deterministic = isoline::runtime::ExecutionMode::Deterministic;

/// The registers a SentinelThunk checks, at most: every General and every Float one.
constexpr auto SentinelSlots = std::size_t { 32 };

/// What every register is set to before the trampoline runs: its file and number in the low bits; an XMM
/// register's high quadword holds the complement, so that all 128 bits are checked.
[[nodiscard]] constexpr uint64_t sentinelOf(PhysicalRegister reg) noexcept
{
    auto const file = reg.registerClass == isoline::compiler::machine::RegisterClass::Float ? 0x100U : 0U;
    return 0x5eed'0000'0000'0000ULL | file | reg.number;
}

/// The registers the host convention preserves, General then Float, in register order.
[[nodiscard]] std::vector<PhysicalRegister> preservedOf(isoline::compiler::machine::ConventionRow const& host)
{
    auto preserved = std::vector<PhysicalRegister> {};
    for (auto const number: std::views::iota(std::size_t { 0 }, x64::RegistersPerFile))
        if (auto const reg = x64::physical(static_cast<x64::Gpr>(number)); host.calleeSaved.contains(reg))
            preserved.push_back(reg);
    for (auto const number: std::views::iota(std::size_t { 0 }, x64::RegistersPerFile))
        if (auto const reg = x64::physical(static_cast<x64::Xmm>(number)); host.calleeSaved.contains(reg))
            preserved.push_back(reg);
    return preserved;
}

/// A thunk of EntryTrampoline's signature around @p trampoline: it saves what the host convention preserves,
/// sets every such register to its sentinel (an XMM register all 128 bits, loaded from a 16-byte slot), calls
/// the trampoline with its own arguments, writes what each register holds after into its 16-byte slot of
/// observed() (by preservedOf()'s order), restores and returns the trampoline's status. So a trampoline that
/// restored a register wrongly, or half of one, leaves something other than its sentinel there. Its slots'
/// addresses are in its code, so it is neither copied nor moved.
class SentinelThunk
{
  public:
    explicit SentinelThunk(EntryTrampoline trampoline):
        _host { x64::conventionRow(isoline::compiler::machine::hostConvention(hostDescription())) },
        _preserved { preservedOf(_host) }
    {
        REQUIRE(_preserved.size() <= SentinelSlots);
        for (auto const [index, reg]: core::views::enumerate(_preserved))
        {
            auto const slot = 2 * static_cast<std::size_t>(index);
            _sentinels[slot] = sentinelOf(reg);
            _sentinels[slot + 1] =
                reg.registerClass == isoline::compiler::machine::RegisterClass::Float ? ~sentinelOf(reg) : 0;
        }
        auto const addressOf = [](std::span<uint64_t const> words) {
            return x64::immediate(static_cast<int64_t>(reinterpret_cast<std::uintptr_t>(words.data())));
        };
        auto arena = std::pmr::monotonic_buffer_resource {};
        auto buffer = isoline::compiler::machine::CodeBuffer { arena };
        auto assembler = x64::Assembler { buffer, {}, nullptr };
        auto const gpr = [](x64::Gpr reg) {
            return x64::physicalOperand(reg);
        };
        auto const operandOf = [](PhysicalRegister reg) {
            return isoline::compiler::machine::MachineOperand { isoline::compiler::machine::RegisterOperand {
                .name = isoline::compiler::machine::RegisterName::of(reg),
                .role = isoline::compiler::machine::OperandRole::Use } };
        };
        auto general = std::vector<PhysicalRegister> {};
        auto floats = std::vector<PhysicalRegister> {};
        for (auto const reg: _preserved)
            (reg.registerClass == isoline::compiler::machine::RegisterClass::Float ? floats : general)
                .push_back(reg);
        std::erase(general, x64::physical(x64::Gpr::Rbp));
        // The trampoline's arguments as the host passes them; the stack ones are copied to the outgoing area.
        auto const parameters = std::vector(5, isoline::compiler::intermediate::Type::Pointer);
        auto const results = std::array { isoline::compiler::intermediate::Type::I32 };
        auto const call = isoline::compiler::machine::assignCall(_host, parameters, results, arena);
        auto const outgoing = (static_cast<int64_t>(call.arguments.memoryBytes) + 15) / 16 * 16;
        auto const pushed = int64_t { 8 } * static_cast<int64_t>(general.size());
        auto const below = outgoing + (16 * static_cast<int64_t>(floats.size()));
        auto const frame = ((pushed + below + 15) / 16 * 16) - pushed;
        assembler.emit(x64::Mnemonic::Push64R, { gpr(x64::Gpr::Rbp) });
        assembler.emit(x64::Mnemonic::Mov64RR, { gpr(x64::Gpr::Rbp), gpr(x64::Gpr::Rsp) });
        for (auto const reg: general)
            assembler.emit(x64::Mnemonic::Push64R, { operandOf(reg) });
        assembler.emit(x64::Mnemonic::Sub64RI, { gpr(x64::Gpr::Rsp), x64::immediate(frame) });
        for (auto const [index, reg]: core::views::enumerate(floats))
            assembler.emit(
                x64::Mnemonic::MovapsMR,
                { x64::at(x64::Gpr::Rsp, outgoing + (16 * static_cast<int64_t>(index))), operandOf(reg) });
        for (auto const& location: call.arguments.locations)
            if (auto const* offset = std::get_if<isoline::compiler::machine::StackOffset>(&location))
            {
                assembler.emit(
                    x64::Mnemonic::Mov64RM,
                    { gpr(x64::Gpr::Rax), x64::at(x64::Gpr::Rbp, int64_t { 16 } + offset->bytes) });
                assembler.emit(
                    x64::Mnemonic::Mov64MR,
                    { x64::at(x64::Gpr::Rsp, static_cast<int64_t>(offset->bytes)), gpr(x64::Gpr::Rax) });
            }
        // No preserved register is an argument register of either host convention, so the arguments stay.
        // rax is no argument register of either host convention; the slots are 16-aligned, so movaps moves
        // an XMM register's 128 bits whole.
        assembler.emit(x64::Mnemonic::Mov64RI, { gpr(x64::Gpr::Rax), addressOf(_sentinels) });
        for (auto const [index, reg]: core::views::enumerate(_preserved))
        {
            auto const slot = x64::at(x64::Gpr::Rax, 16 * static_cast<int64_t>(index));
            if (reg.registerClass == isoline::compiler::machine::RegisterClass::Float)
                assembler.emit(x64::Mnemonic::MovapsRM, { operandOf(reg), slot });
        }
        for (auto const reg: _preserved)
            if (reg.registerClass == isoline::compiler::machine::RegisterClass::General)
                assembler.emit(x64::Mnemonic::Mov64RI,
                               { operandOf(reg), x64::immediate(std::bit_cast<int64_t>(sentinelOf(reg))) });
        assembler.emit(
            x64::Mnemonic::Mov64RI,
            { gpr(x64::Gpr::Rax),
              x64::immediate(static_cast<int64_t>(reinterpret_cast<std::uintptr_t>(trampoline))) });
        assembler.emit(x64::Mnemonic::Call64R, { gpr(x64::Gpr::Rax) });
        assembler.emit(x64::Mnemonic::Mov64RI, { gpr(x64::Gpr::R11), addressOf(_observed) });
        for (auto const [index, reg]: core::views::enumerate(_preserved))
            assembler.emit(reg.registerClass == isoline::compiler::machine::RegisterClass::General
                               ? x64::Mnemonic::Mov64MR
                               : x64::Mnemonic::MovapsMR,
                           { x64::at(x64::Gpr::R11, 16 * static_cast<int64_t>(index)), operandOf(reg) });
        for (auto const [index, reg]: core::views::enumerate(floats))
            assembler.emit(
                x64::Mnemonic::MovapsRM,
                { operandOf(reg), x64::at(x64::Gpr::Rsp, outgoing + (16 * static_cast<int64_t>(index))) });
        assembler.emit(x64::Mnemonic::Add64RI, { gpr(x64::Gpr::Rsp), x64::immediate(frame) });
        for (auto const reg: general | std::views::reverse)
            assembler.emit(x64::Mnemonic::Pop64R, { operandOf(reg) });
        assembler.emit(x64::Mnemonic::Pop64R, { gpr(x64::Gpr::Rbp) });
        assembler.emit(x64::Mnemonic::Ret, {});
        REQUIRE_FALSE(assembler.failure().has_value());
        // Off the mapping's first bytes, as every entry is (UBSan reads before an indirect call's target).
        auto padded = std::vector<std::byte>(16, std::byte { 0xcc });
        std::ranges::copy(std::as_bytes(buffer.bytes()), std::back_inserter(padded));
        auto sealed = PortableCodeMemory {}.seal(padded);
        REQUIRE(sealed.has_value());
        _sealed = std::move(*sealed);
    }

    /// The thunk.
    [[nodiscard]] EntryTrampoline entry() const noexcept
    {
        return reinterpret_cast<EntryTrampoline>(
            reinterpret_cast<std::uintptr_t>(_sealed->code().data() + 16));
    }

    SentinelThunk(SentinelThunk const&) = delete;
    SentinelThunk(SentinelThunk&&) = delete;
    SentinelThunk& operator=(SentinelThunk const&) = delete;
    SentinelThunk& operator=(SentinelThunk&&) = delete;
    ~SentinelThunk() = default;

    /// Checks that every register it checks held its sentinel after the trampoline, all 128 bits of an XMM
    /// one; and clears what it observed for the next call.
    void checkRestored()
    {
        for (auto const [index, reg]: core::views::enumerate(_preserved))
        {
            auto const slot = 2 * static_cast<std::size_t>(index);
            CAPTURE(static_cast<int>(reg.registerClass), static_cast<int>(reg.number));
            CHECK(_observed[slot] == _sentinels[slot]);
            if (reg.registerClass == isoline::compiler::machine::RegisterClass::Float)
                CHECK(_observed[slot + 1] == _sentinels[slot + 1]);
        }
        _observed = {};
    }

    /// The registers it checks, in the order it writes them.
    [[nodiscard]] std::span<PhysicalRegister const> preserved() const noexcept { return _preserved; }

    /// The host's description, as the tests compile for it.
    [[nodiscard]] static TargetDescription hostDescription() noexcept
    {
        return TargetDescription { .architecture = isoline::compiler::machine::Architecture::X64,
                                   .abi = isoline::HostOperatingSystemAbi };
    }

  private:
    isoline::compiler::machine::ConventionRow _host;
    std::vector<PhysicalRegister> _preserved;
    std::unique_ptr<isoline::execution::ISealedCode> _sealed;
    alignas(16) std::array<uint64_t, 2 * SentinelSlots> _sentinels {};
    alignas(16) std::array<uint64_t, 2 * SentinelSlots> _observed {};
};

/// How a clobbering function leaves, by its code index less 2: a trap in its callee $g, a return through $id,
/// or a memory.fill out of bounds, whose stub returns Pending.
constexpr auto ClobberingExits = std::to_array<std::string_view>({
    "(local.set $a (call $g (local.get $a)))",
    "(local.set $a (call $id (local.get $a)))",
    "(memory.fill (i32.const 65000) (i32.const 0) (i32.const 1000))",
});

/// Functions that keep 16 integers live across a call or a stub, and have 16 floats live at once before it,
/// so that they write rbx, r12, r13 and every XMM register; then each leaves as its ClobberingExits row says.
[[nodiscard]] std::string clobberingModule()
{
    auto text = std::string { "(module\n (memory 1)\n (func $g (param i64) (result i64) unreachable)\n"
                              " (func $id (param i64) (result i64) local.get 0)\n" };
    for (auto const exit: ClobberingExits)
    {
        text += std::format(" (func (param $a i64) (param $b f64) (result i64)\n");
        for (auto const index: std::views::iota(0, 16))
            text += std::format("  (local $i{0} i64) (local $x{0} f64)\n", index);
        for (auto const index: std::views::iota(0, 16))
            text += std::format("  (local.set $i{0} (i64.mul (local.get $a) (i64.const {1})))\n"
                                "  (local.set $x{0} (f64.mul (local.get $b) (f64.const {1})))\n",
                                index,
                                index + 3);
        text += "  (local.set $a (i64.trunc_sat_f64_s ";
        for ([[maybe_unused]] auto const index: std::views::iota(0, 15))
            text += "(f64.add ";
        text += "(local.get $x0)";
        for (auto const index: std::views::iota(1, 16))
            text += std::format(" (local.get $x{}))", index);
        text += std::format("))\n  {}\n  (local.get $a)\n", exit);
        for (auto const index: std::views::iota(0, 16))
            text += std::format("  (local.get $i{}) i64.add\n", index);
        text += " )\n";
    }
    return text + ")\n";
}

} // namespace

TEST_CASE("Trampoline.trapExitRestoresCalleeSaved")
{
    // Every way out of the trampoline restores every register the host preserves, all 128 bits of an XMM one:
    // a return, the trap exit from a trap site, from a stub that returned Pending, and from an interrupted
    // poll.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    auto const module = moduleOf(clobberingModule());
    auto const arguments =
        std::vector<Value> { I64 { .bits = 2 }, F64 { .bits = std::bit_cast<uint64_t>(1.5) } };
    SECTION("a trap in a callee, its caller's registers in use")
    {
        auto created = CompiledInstance::create(module, hostConfig(Deterministic, {}));
        REQUIRE(created.has_value());
        auto thunk = SentinelThunk { (*created)->code().trampoline() };
        auto const called = (*created)->callThrough(thunk.entry(), 2, arguments);
        REQUIRE_FALSE(called.has_value());
        CHECK(called.error() == isoline::runtime::Abrupt { Trap { TrapCode::Unreachable } });
        thunk.checkRestored();
    }
    SECTION("a return")
    {
        auto created = CompiledInstance::create(module, hostConfig(Deterministic, {}));
        REQUIRE(created.has_value());
        auto thunk = SentinelThunk { (*created)->code().trampoline() };
        auto const called = (*created)->callThrough(thunk.entry(), 3, arguments);
        REQUIRE(called.has_value());
        // 2·(3 + … + 18), and the floats' sum, 1.5·(3 + … + 18) = 252, through $id.
        CHECK(*called == std::vector<Value> { I64 { .bits = 252 + (2 * 168) } });
        thunk.checkRestored();
    }
    SECTION("a stub that returned Pending: memory.fill out of bounds")
    {
        auto created = CompiledInstance::create(module, hostConfig(Deterministic, {}));
        REQUIRE(created.has_value());
        auto thunk = SentinelThunk { (*created)->code().trampoline() };
        auto const called = (*created)->callThrough(thunk.entry(), 4, arguments);
        REQUIRE_FALSE(called.has_value());
        CHECK(called.error() == isoline::runtime::Abrupt { Trap { TrapCode::OutOfBoundsMemoryAccess } });
        thunk.checkRestored();
    }
    SECTION("an interrupted poll: $id's entry, after its caller wrote its registers")
    {
        // Observed, every poll takes PollSlow; the second FunctionEntry poll ($id's) interrupts the store.
        auto polls = isoline::runtime::testing::PollCounter { isoline::runtime::testing::PollTrigger {
            .site = isoline::runtime::PollSite::FunctionEntry, .ordinal = 2 } };
        auto created =
            CompiledInstance::create(module,
                                     hostConfig(Deterministic, {}),
                                     isoline::execution::testing::InstanceOptions { .observer = &polls });
        REQUIRE(created.has_value());
        polls.interruptAtTrigger((*created)->store().interrupter());
        auto thunk = SentinelThunk { (*created)->code().trampoline() };
        auto const called = (*created)->callThrough(thunk.entry(), 3, arguments);
        REQUIRE_FALSE(called.has_value());
        CHECK(called.error() == isoline::runtime::Abrupt { Trap { TrapCode::Interrupted } });
        thunk.checkRestored();
    }
}

TEST_CASE("Trampoline.savesWhatTheHostPreserves")
{
    // Both host rows, on every host: Microsoft x64 saves xmm6–15 whole and finds its fifth argument on the
    // stack.
    auto arena = std::pmr::monotonic_buffer_resource {};
    auto const countOf = [](std::span<uint8_t const> bytes, std::array<uint8_t, 2> pattern) {
        auto count = std::size_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 1 }, bytes.size()))
            if (bytes[index - 1] == pattern[0] && bytes[index] == pattern[1])
                ++count;
        return count;
    };
    struct Row
    {
        OperatingSystemAbi abi;
        std::size_t floatSaves;
    };
    for (auto const& [abi, floatSaves]:
         std::to_array<Row>({ { OperatingSystemAbi::SystemV, 0 }, { OperatingSystemAbi::Windows, 10 } }))
    {
        auto const target = x64::X64Target::create(
            TargetDescription { .architecture = isoline::compiler::machine::Architecture::X64, .abi = abi });
        REQUIRE(target.has_value());
        auto buffer = isoline::compiler::machine::CodeBuffer { arena };
        auto const trapExit = buffer.createLabel();
        target->emitEntryTrampoline(buffer, trapExit);
        CAPTURE(static_cast<int>(abi));
        REQUIRE(buffer.labelOffset(trapExit).has_value());
        CHECK(countOf(buffer.bytes(), { 0x0f, 0x29 }) == floatSaves); // movaps m128, xmm
        CHECK(countOf(buffer.bytes(), { 0x0f, 0x28 }) == floatSaves); // movaps xmm, m128
        CHECK(buffer.bytes().back() != 0xc3);                         // the trap exit's jump comes last
    }
}

TEST_CASE("Trampoline.adaptersPassEveryValue")
{
    // More values than the internal convention has registers for, each way, of every type: the adapter's
    // stack arguments, its result area through the hidden pointer, and a Ref's word to and from its cell.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    constexpr auto Module = std::string_view { R"((module
  (func (param i64 i64 i64 i64 i64 i64 i64 i64 f64 f64 f64 f64 f64 f64 f64 f64 f64 f64 externref i32 f32 externref)
        (result i64 f64 externref i32 f32 externref i64 f64 i64 f64)
    (local.get 7) (local.get 17) (local.get 21) (local.get 19) (local.get 20)
    (local.get 18) (local.get 0) (local.get 8) (i64.add (local.get 6) (local.get 5)) (local.get 16)))
)" };
    auto created = CompiledInstance::create(moduleOf(Module), hostConfig(Deterministic, {}));
    REQUIRE(created.has_value());
    auto& instance = **created;
    auto arguments = std::vector<Value> {};
    for (auto const index: std::views::iota(uint64_t { 0 }, uint64_t { 8 }))
        arguments.emplace_back(I64 { .bits = 100 + index });
    for (auto const index: std::views::iota(0, 10))
        arguments.emplace_back(F64 { .bits = std::bit_cast<uint64_t>(0.5 + index) });
    auto const first =
        Ref { .kind = RefKind::Extern, .heap = isoline::binary::ExternRef.heap, .address = 77 };
    auto const second = Ref { .kind = RefKind::Null, .heap = isoline::binary::ExternRef.heap };
    arguments.emplace_back(first);
    arguments.emplace_back(I32 { .bits = 0xffff'fffe });
    arguments.emplace_back(F32 { .bits = std::bit_cast<uint32_t>(-2.25F) });
    arguments.emplace_back(second);
    auto const called = instance.call(0, arguments);
    REQUIRE(called.has_value());
    CHECK(*called
          == std::vector<Value> { I64 { .bits = 107 },
                                  F64 { .bits = std::bit_cast<uint64_t>(9.5) },
                                  second,
                                  I32 { .bits = 0xffff'fffe },
                                  F32 { .bits = std::bit_cast<uint32_t>(-2.25F) },
                                  first,
                                  I64 { .bits = 100 },
                                  F64 { .bits = std::bit_cast<uint64_t>(0.5) },
                                  I64 { .bits = 211 },
                                  F64 { .bits = std::bit_cast<uint64_t>(8.5) } });
}

TEST_CASE("Trampoline.entryOverheadCoversTheEntry")
{
    // EntryOverheadBytes, against the trampoline's and the adapter's real layout: the stack from the host's
    // call of the trampoline to the function's entry is within it for a type that fits the registers, and
    // within it plus entryAdapterAreaBytes() for one that does not. A function that records rsp at its entry
    // stands in for the compiled one.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    auto arena = std::pmr::monotonic_buffer_resource {};
    auto buffer = isoline::compiler::machine::CodeBuffer { arena };
    auto assembler = x64::Assembler { buffer, {}, nullptr };
    // The adapter keeps the cells in r10 up to its call: the recorder stores rsp into cell 0.
    assembler.emit(x64::Mnemonic::Mov64MR,
                   { x64::at(x64::Gpr::R10, 0), x64::physicalOperand(x64::Gpr::Rsp) });
    assembler.emit(x64::Mnemonic::Ret, {});
    REQUIRE_FALSE(assembler.failure().has_value());
    auto padded = std::vector<std::byte>(16, std::byte { 0xcc });
    std::ranges::copy(std::as_bytes(buffer.bytes()), std::back_inserter(padded));
    auto sealed = PortableCodeMemory {}.seal(padded);
    REQUIRE(sealed.has_value());
    auto const* recorder = (*sealed)->code().subspan(16).data();
    auto const target = x64::X64Target::create(hostConfig(Deterministic, {}).target);
    REQUIRE(target.has_value());
    for (auto const params: { std::size_t { 0 }, std::size_t { 6 }, std::size_t { 200 } })
    {
        INFO(params << " i64 parameters");
        auto text = std::string { "(module (func (param" };
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, params))
            text.append(" i64");
        text.append(")))");
        auto const validated = moduleOf(text);
        auto created = CompiledInstance::create(validated, hostConfig(Deterministic, {}));
        REQUIRE(created.has_value());
        auto const& code = (*created)->code();
        auto values = std::vector<isoline::runtime::RawValue>(std::max<std::size_t>(params, 1));
        auto state = isoline::compiler::ExecutionState {};
        auto const context = isoline::compiler::InstanceContext {};
        auto const before = isoline::execution::currentStackAddress();
        auto const status = code.trampoline()(&state, &context, code.adapter(0), recorder, values.data());
        CHECK(status == std::to_underlying(isoline::compiler::EntryStatus::Returned));
        auto const atEntry = static_cast<std::uintptr_t>(values[0].words[0]);
        REQUIRE(atEntry < before);
        // The frame address is the caller's stack pointer less its return address and saved frame pointer.
        auto const taken = static_cast<uint64_t>(before - atEntry) + 16;
        auto const area = target->entryAdapterAreaBytes(validated->module().types[0]);
        CAPTURE(taken, area);
        CHECK(taken <= isoline::execution::EntryOverheadBytes + area);
        if (params <= 6)
            CHECK(area == 0);
        else
            CHECK(area >= (params - 6) * 8);
    }
}
