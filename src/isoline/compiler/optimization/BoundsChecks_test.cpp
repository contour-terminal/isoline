// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Guards.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/intermediate/Printer.hpp>
#include <isoline/compiler/intermediate/Verifier.hpp>
#include <isoline/compiler/intermediate/testing/GuardEdges.hpp>
#include <isoline/compiler/machine/TargetDescription.hpp>
#include <isoline/compiler/optimization/PassTable.hpp>
#include <isoline/compiler/translation/testing/Translation.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidatedModule.hpp>

using isoline::binary::AddressType;
using isoline::binary::FuncType;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::code;
using isoline::binary::testing::encodeU64;
using isoline::binary::testing::ModuleBuilder;
using isoline::compiler::CompileCancellation;
using isoline::compiler::CompileConfig;
using isoline::compiler::RuntimeStub;
using isoline::compiler::SpectreGuard;
using isoline::compiler::intermediate::BlockId;
using isoline::compiler::intermediate::Builder;
using isoline::compiler::intermediate::ContextValue;
using isoline::compiler::intermediate::Extension;
using isoline::compiler::intermediate::Function;
using isoline::compiler::intermediate::guardMemoryAccess;
using isoline::compiler::intermediate::Operation;
using isoline::compiler::intermediate::Type;
using isoline::compiler::intermediate::ValueId;
using isoline::compiler::intermediate::verify;
using isoline::compiler::intermediate::testing::MemoryGuardEdges;
using isoline::compiler::machine::TargetDescription;
using isoline::compiler::optimization::memoryMinimumBytesOf;
using isoline::compiler::optimization::Pass;
using isoline::compiler::optimization::PassContext;
using isoline::compiler::optimization::PassRow;
using isoline::compiler::optimization::PassStatistic;
using isoline::compiler::optimization::PassStatistics;
using isoline::compiler::optimization::PassTable;
using isoline::compiler::optimization::runPasses;
using isoline::compiler::optimization::Verification;
using isoline::compiler::translation::testing::Translated;
using isoline::compiler::translation::testing::translateOf;
using isoline::runtime::ExecutionMode;
using isoline::runtime::TrapCode;

namespace intermediate = isoline::compiler::intermediate;

namespace
{

constexpr auto PageBytes = uint64_t { 65'536 };
constexpr auto All = ~uint64_t { 0 };

// --- WebAssembly programs of accesses -------------------------------------------------------------------

/// What one step of a test program does.
enum class StepKind : uint8_t
{
    Load, ///< Loads size bytes at index + offset of a memory, zero-extended to i64, and adds them up.
    Grow, ///< memory.grow of a memory by one page, its result dropped: the memory may move.
    Call, ///< Calls an empty function, which might have grown any memory.
};

/// Where a load's index comes from.
enum class IndexSource : uint8_t
{
    First,    ///< The first parameter: one SSA value for every load that reads it.
    Second,   ///< The second parameter: another SSA value.
    Constant, ///< A constant.
};

/// One step of a test program.
struct Step
{
    StepKind kind = StepKind::Load;         ///< What it does.
    uint32_t memory = 0;                    ///< The memory it loads from or grows.
    uint64_t offset = 0;                    ///< A load's static offset.
    uint64_t size = 4;                      ///< A load's width: 1, 2, 4 or 8.
    IndexSource index = IndexSource::First; ///< A load's index.
    uint64_t constant = 0;                  ///< The index when it is a constant.
};

/// A load of @p size bytes at the first parameter plus @p offset, of @p memory.
Step loadOf(uint64_t offset, uint64_t size, uint32_t memory = 0)
{
    return Step { .kind = StepKind::Load, .memory = memory, .offset = offset, .size = size };
}

/// A load of @p size bytes at the constant @p index plus @p offset.
Step constantLoadOf(uint64_t index, uint64_t offset, uint64_t size)
{
    return Step { .kind = StepKind::Load,
                  .offset = offset,
                  .size = size,
                  .index = IndexSource::Constant,
                  .constant = index };
}

/// The opcode of the i64 load of @p size bytes, zero-extending.
uint8_t loadOpcodeOf(uint64_t size)
{
    switch (size)
    {
        case 1: return 0x31;  // i64.load8_u
        case 2: return 0x33;  // i64.load16_u
        case 4: return 0x35;  // i64.load32_u
        default: return 0x29; // i64.load
    }
}

/// The memarg of an access of @p memory at @p offset, its alignment 0.
Bytes memargOf(uint32_t memory, uint64_t offset)
{
    // A memory other than 0 sets bit 6 of the alignment and follows it with its index.
    auto const flags = memory == 0 ? Bytes { 0x00 } : code({ { 0x40 }, encodeU64(memory) });
    return code({ flags, encodeU64(offset) });
}

/// An index constant of @p type.
Bytes indexConstant(AddressType type, uint64_t value)
{
    if (type == AddressType::I64)
        return isoline::binary::testing::i64Const(static_cast<int64_t>(value));
    return isoline::binary::testing::i32Const(static_cast<int32_t>(static_cast<uint32_t>(value)));
}

/// A test program: its memories (every one of one address type), and the function 0 it runs: (index,
/// other) -> i64, summing what its loads read; function 1 is empty.
struct Program
{
    std::vector<MemoryType> memories;
    std::vector<Step> steps;

    /// The address type every memory has.
    [[nodiscard]] AddressType addressType() const { return memories.front().limits.addressType; }

    /// The module.
    [[nodiscard]] ModuleBuilder module() const
    {
        auto const index = addressType() == AddressType::I64 ? isoline::binary::I64 : isoline::binary::I32;
        auto body = code({ isoline::binary::testing::i64Const(0) });
        for (auto const& step: steps)
            switch (step.kind)
            {
                case StepKind::Load: {
                    auto pushed = Bytes {};
                    switch (step.index)
                    {
                        case IndexSource::First: pushed = isoline::binary::testing::localGet(0); break;
                        case IndexSource::Second: pushed = isoline::binary::testing::localGet(1); break;
                        case IndexSource::Constant:
                            pushed = indexConstant(addressType(), step.constant);
                            break;
                    }
                    body = code({ body,
                                  pushed,
                                  { loadOpcodeOf(step.size) },
                                  memargOf(step.memory, step.offset),
                                  { 0x7C } }); // i64.add
                    break;
                }
                case StepKind::Grow:
                    body = code({ body,
                                  indexConstant(addressType(), 1),
                                  { 0x40 },
                                  encodeU64(step.memory),
                                  isoline::binary::testing::drop() });
                    break;
                case StepKind::Call: body = code({ body, isoline::binary::testing::call(1) }); break;
            }
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = { index, index }, .results = { isoline::binary::I64 } })
            .type(FuncType {});
        for (auto const& memory: memories)
            builder.memory(memory);
        builder.function(0, {}, body).function(1, {}, Bytes {});
        return builder;
    }
};

/// A memory of @p pages pages of 2^@p pageSizeLog2 bytes, at most @p pages + 4.
MemoryType memoryOf(uint64_t pages, uint8_t pageSizeLog2 = 16, AddressType type = AddressType::I32)
{
    return MemoryType { .limits = Limits { .min = pages, .max = pages + 4, .addressType = type },
                        .pageSizeLog2 = pageSizeLog2 };
}

// --- Compiling ------------------------------------------------------------------------------------------

/// A function translated and run through passes, with what they counted.
struct Optimized
{
    std::unique_ptr<Translated> translated;
    std::vector<uint64_t> minimums;
    PassStatistics statistics {};

    /// The function.
    [[nodiscard]] Function const& function() const { return *translated->function; }

    /// How many listed instructions are @p operation.
    [[nodiscard]] std::size_t countOf(Operation operation) const
    {
        auto count = std::size_t { 0 };
        for (auto const block: std::views::iota(uint32_t { 0 }, function().blockCount()))
            for (auto const index: function().block(BlockId { block }).instructions)
                if (function().instruction(index).operation == operation)
                    ++count;
        return count;
    }

    /// How many checks the passes counted as @p statistic.
    [[nodiscard]] uint64_t counted(PassStatistic statistic) const { return statistics.count(statistic); }
};

/// Translates function 0 of @p builder and runs @p rows over it in @p spectre, verifying after each against
/// the module's memory minimums.
Optimized optimize(ModuleBuilder const& builder,
                   SpectreGuard spectre,
                   std::span<PassRow const> rows = PassTable)
{
    auto result = Optimized {};
    result.translated = translateOf(builder);
    REQUIRE(result.translated->function.has_value());
    auto& memory = result.translated->memory;
    auto const minimums = memoryMinimumBytesOf(*result.translated->module, memory);
    result.minimums.assign(minimums.begin(), minimums.end());
    auto const config = CompileConfig { TargetDescription {}, ExecutionMode::Production, spectre };
    auto const cancellation = CompileCancellation {};
    auto context = PassContext { .module = *result.translated->module,
                                 .config = config,
                                 .cancellation = cancellation,
                                 .memory = memory,
                                 .verification = Verification::AfterEachPass,
                                 .codeIndex = 0,
                                 .memoryMinimumBytes = result.minimums };
    auto const ran = runPasses(*result.translated->function, context, rows);
    INFO((ran ? std::string {} : isoline::compiler::describe(ran.error())) << "\n"
                                                                           << result.translated->text());
    REQUIRE(ran.has_value());
    result.statistics = context.statistics;
    return result;
}

/// The PassTable row of @p pass, alone.
std::array<PassRow, 1> rowOf(Pass pass)
{
    auto const found = std::ranges::find(PassTable, pass, &PassRow::pass);
    REQUIRE(found != PassTable.end());
    return { *found };
}

// --- Evaluating the optimized function ------------------------------------------------------------------

/// How a function ended.
struct Outcome
{
    std::optional<TrapCode> trap; ///< The trap; none when it returned.
    uint64_t value = 0;           ///< What it returned.

    /// Whether both ended alike.
    [[nodiscard]] bool operator==(Outcome const&) const = default;
};

/// Whether trap sites stop the evaluation.
enum class Speculation : uint8_t
{
    Architectural, ///< As the hardware retires it: a failed check traps.
    Mispredicted, ///< As a mispredicted branch runs ahead: no trap is taken, a failed check's select gives 0.
};

/// One memory as the evaluator holds it.
struct EvaluatedMemory
{
    std::vector<std::byte> bytes;
    MemoryType type;
    uint64_t generation = 0; ///< Bumped by every call, stub or grow: where the memory may have moved to.
};

/// A Pointer value: an address into a memory of one generation, or 0.
struct Pointer
{
    std::optional<uint32_t> memory; ///< Its memory; none for the null pointer a select left.
    uint64_t generation = 0;        ///< The memory's generation it was formed in.
    uint64_t offset = 0;            ///< Its offset into the memory.
};

/// A small interpreter of the intermediate representation, for the shapes Program translates to: every
/// access through a pointer must address its memory's current generation, in bounds, and under
/// Speculation::Mispredicted every non-null pointer must still be in bounds: a removed check that left
/// a guest-chosen address unclamped reads past the end there.
class Evaluator
{
    using Instruction = intermediate::Instruction;

  public:
    Evaluator(Function const& function, std::vector<EvaluatedMemory> memories, Speculation speculation):
        _function { function },
        _memories { std::move(memories) },
        _speculation { speculation },
        _bits(function.valueCount(), 0),
        _pointers(function.valueCount())
    {
    }

    /// Runs the function on @p arguments.
    Outcome run(std::span<uint64_t const> arguments)
    {
        auto const parameters = _function.operands(_function.block(BlockId { 0 }).parameters);
        REQUIRE(parameters.size() == arguments.size());
        for (auto const position: std::views::iota(std::size_t { 0 }, parameters.size()))
            _bits[std::to_underlying(parameters[position])] = arguments[position];
        auto block = BlockId { 0 };
        for ([[maybe_unused]] auto const step: std::views::iota(0, 1'000))
        {
            auto next = std::optional<BlockId> {};
            for (auto const index: _function.block(block).instructions)
            {
                auto const& instruction = _function.instruction(index);
                if (instruction.operation == Operation::Removed)
                    continue;
                auto ended = execute(instruction, next);
                if (ended)
                    return *ended;
                if (next)
                    break;
            }
            REQUIRE(next.has_value());
            block = *next;
        }
        FAIL("the evaluation did not end");
        return {};
    }

  private:
    [[nodiscard]] uint64_t bitsOf(ValueId value) const { return _bits[std::to_underlying(value)]; }

    void define(Instruction const& instruction, uint64_t bits)
    {
        _bits[std::to_underlying(_function.operands(instruction.results)[0])] = bits;
    }

    void definePointer(Instruction const& instruction, Pointer pointer)
    {
        _pointers[std::to_underlying(_function.operands(instruction.results)[0])] = pointer;
    }

    [[nodiscard]] Pointer pointerOf(ValueId value) const { return _pointers[std::to_underlying(value)]; }

    void movesEveryMemory()
    {
        for (auto& memory: _memories)
            ++memory.generation;
    }

    void passTo(intermediate::BranchTarget const& target, std::optional<BlockId>& next)
    {
        auto const parameters = _function.operands(_function.block(target.block).parameters);
        auto const arguments = _function.operands(target.arguments);
        auto bits = std::vector<uint64_t> {};
        auto pointers = std::vector<Pointer> {};
        for (auto const argument: arguments)
        {
            bits.push_back(bitsOf(argument));
            pointers.push_back(pointerOf(argument));
        }
        for (auto const position: std::views::iota(std::size_t { 0 }, parameters.size()))
        {
            _bits[std::to_underlying(parameters[position])] = bits[position];
            _pointers[std::to_underlying(parameters[position])] = pointers[position];
        }
        next = target.block;
    }

    /// Executes @p instruction; @return how the function ended, if it did here.
    std::optional<Outcome> execute(Instruction const& instruction, std::optional<BlockId>& next)
    {
        auto const operands = _function.operands(instruction.operands);
        auto const immediates = _function.immediates(instruction.immediates);
        auto const trapped = [&](TrapCode code) -> std::optional<Outcome> {
            if (_speculation == Speculation::Mispredicted)
                return std::nullopt;
            return Outcome { .trap = code };
        };
        switch (instruction.operation)
        {
            case Operation::I32Const: define(instruction, immediates[0] & 0xFFFF'FFFF); return {};
            case Operation::I64Const: define(instruction, immediates[0]); return {};
            case Operation::I64Add: define(instruction, bitsOf(operands[0]) + bitsOf(operands[1])); return {};
            case Operation::I32WrapI64: define(instruction, bitsOf(operands[0]) & 0xFFFF'FFFF); return {};
            case Operation::I64ExtendI32U: define(instruction, bitsOf(operands[0]) & 0xFFFF'FFFF); return {};
            case Operation::LoadContext:
                switch (static_cast<ContextValue>(immediates[0]))
                {
                    case ContextValue::StepCountdown: define(instruction, 1'000'000); return {};
                    case ContextValue::Memory0Base:
                        definePointer(instruction,
                                      Pointer { .memory = 0, .generation = _memories[0].generation });
                        return {};
                    case ContextValue::Memory0Bound:
                        define(instruction, _memories[0].bytes.size());
                        return {};
                    default: FAIL("an unexpected context value"); return {};
                }
            case Operation::MemoryBase: {
                auto const memory = static_cast<uint32_t>(immediates[0]);
                definePointer(instruction,
                              Pointer { .memory = memory, .generation = _memories[memory].generation });
                return {};
            }
            case Operation::MemoryBound:
                define(instruction, _memories[static_cast<std::size_t>(immediates[0])].bytes.size());
                return {};
            case Operation::StoreContext:
            case Operation::PollStored:
            case Operation::EnterFrame:
            case Operation::LeaveFrame: return {};
            case Operation::Poll: define(instruction, bitsOf(operands[0])); return {};
            case Operation::CheckMemory: {
                auto const memory = static_cast<uint32_t>(immediates[0]);
                auto const base = pointerOf(operands[1]);
                REQUIRE(base.memory == std::optional { memory });
                CHECK(base.generation == _memories[memory].generation); // a base from before a move
                auto const effective = guardMemoryAccess(_function.typeOf(operands[0]),
                                                         bitsOf(operands[0]),
                                                         immediates[1],
                                                         immediates[2],
                                                         bitsOf(operands[2]));
                if (!effective)
                {
                    definePointer(instruction, Pointer {});
                    return trapped(TrapCode::OutOfBoundsMemoryAccess);
                }
                definePointer(
                    instruction,
                    Pointer { .memory = memory, .generation = base.generation, .offset = *effective });
                return {};
            }
            case Operation::GuardAddress: {
                auto pointer = pointerOf(operands[0]);
                if (pointer.memory)
                    pointer.offset += immediates[0];
                definePointer(instruction, pointer);
                return {};
            }
            case Operation::InBoundsAddress: {
                auto const memory = static_cast<uint32_t>(immediates[0]);
                auto const base = pointerOf(operands[0]);
                REQUIRE(base.memory == std::optional { memory });
                definePointer(
                    instruction,
                    Pointer { .memory = memory, .generation = base.generation, .offset = immediates[1] });
                return {};
            }
            case Operation::Load: return load(instruction, operands, immediates);
            case Operation::CallStub: {
                REQUIRE(immediates[0] == std::to_underlying(RuntimeStub::MemoryGrow));
                REQUIRE(operands.size() == 2);
                auto& memory = _memories[static_cast<std::size_t>(bitsOf(operands[0]))];
                auto const pageBytes = uint64_t { 1 } << memory.type.pageSizeLog2;
                auto const pages = memory.bytes.size() / pageBytes;
                auto const delta = bitsOf(operands[1]);
                auto result = All;
                if (delta <= *memory.type.limits.max - pages)
                {
                    memory.bytes.resize(memory.bytes.size() + (delta * pageBytes));
                    result = pages;
                }
                movesEveryMemory(); // a stub may move any memory, granted or not
                define(instruction, result);
                return {};
            }
            case Operation::Call: movesEveryMemory(); return {};
            case Operation::TrapIf:
                if (bitsOf(operands[0]) != 0)
                    return trapped(static_cast<TrapCode>(immediates[0]));
                return {};
            case Operation::Trap:
                if (_speculation == Speculation::Mispredicted)
                    return Outcome { .trap = TrapCode::Unreachable };
                return Outcome { .trap = static_cast<TrapCode>(immediates[0]) };
            case Operation::Jump:
            case Operation::Branch: {
                auto const targets = _function.targets(instruction);
                if (instruction.operation == Operation::Jump || bitsOf(operands[0]) != 0)
                    passTo(targets[0], next);
                else
                    passTo(targets[1], next);
                return {};
            }
            case Operation::Switch: {
                // The fallback first, then one target per index below their count.
                auto const targets = _function.targets(instruction);
                auto const index = bitsOf(operands[0]);
                passTo(index < targets.size() - 1 ? targets[1 + index] : targets[0], next);
                return {};
            }
            case Operation::Return: return Outcome { .trap = std::nullopt, .value = bitsOf(operands[0]) };
            default:
                FAIL("the evaluator has no case for " << intermediate::nameOf(instruction.operation));
                return {};
        }
    }

    std::optional<Outcome> load(Instruction const& instruction,
                                std::span<ValueId const> operands,
                                std::span<uint64_t const> immediates)
    {
        auto const pointer = pointerOf(operands[0]);
        auto const size = immediates[1];
        if (!pointer.memory)
        {
            // Only where a mispredicted check left its select's 0: the access reads near address 0.
            CHECK(_speculation == Speculation::Mispredicted);
            define(instruction, 0);
            return {};
        }
        auto const& memory = _memories[*pointer.memory];
        INFO("load of " << size << " bytes at " << pointer.offset << " of memory " << *pointer.memory << " ("
                        << memory.bytes.size() << " bytes)");
        // The sandbox: never a stale base, never a byte past the end, even under misprediction.
        REQUIRE(pointer.generation == memory.generation);
        REQUIRE(pointer.offset <= memory.bytes.size());
        REQUIRE(size <= memory.bytes.size() - pointer.offset);
        auto bits = uint64_t { 0 };
        for (auto const position: std::views::iota(uint64_t { 0 }, size))
            bits |= uint64_t { std::to_integer<uint8_t>(memory.bytes[pointer.offset + position]) }
                    << (8 * position);
        CHECK(immediates[2] == std::to_underlying(Extension::Zero));
        define(instruction, bits);
        return {};
    }

    Function const& _function;
    std::vector<EvaluatedMemory> _memories;
    Speculation _speculation;
    std::vector<uint64_t> _bits;
    std::vector<Pointer> _pointers;
};

// --- The reference ------------------------------------------------------------------------------------

/// A program instantiated on a fresh store, run by the reference interpreter, its memories filled with
/// a pattern first.
class Reference
{
  public:
    explicit Reference(Program const& program): Reference { program.module() } {}

    /// @p builder's module, whose function 0 returns an i64.
    explicit Reference(ModuleBuilder const& builder):
        _store { isoline::runtime::StoreLimits {},
                 isoline::runtime::StoreServices { .allocator = _allocator, .pollObserver = nullptr } }
    {
        auto validated = isoline::validate::ValidatedModule::load(
            builder.build(), isoline::FeatureSet::all(), isoline::binary::DecodeLimits {});
        REQUIRE(validated.has_value());
        auto const address = isoline::runtime::instantiate(_store, _interpreter, std::move(*validated), {});
        REQUIRE(address.has_value());
        _address = *address;
        for (auto const memory: _store.moduleInstance(_address).memories)
        {
            auto const bytes = _store.memory(memory).bytes();
            for (auto const position: std::views::iota(std::size_t { 0 }, bytes.size()))
                bytes[position] = static_cast<std::byte>(((position * 131U) + 17U) & 0xFFU);
        }
    }

    /// The memories as they are now, for an evaluator.
    [[nodiscard]] std::vector<EvaluatedMemory> memories() const
    {
        auto copies = std::vector<EvaluatedMemory> {};
        for (auto const memory: _store.moduleInstance(_address).memories)
        {
            auto const& instance = _store.memory(memory);
            copies.push_back(EvaluatedMemory { .bytes = { instance.bytes().begin(), instance.bytes().end() },
                                               .type = instance.type });
        }
        return copies;
    }

    /// Runs function 0 on @p arguments.
    Outcome run(AddressType type, std::span<uint64_t const> arguments)
    {
        auto values = std::vector<isoline::runtime::Value> {};
        for (auto const argument: arguments)
            if (type == AddressType::I64)
                values.emplace_back(isoline::runtime::I64 { argument });
            else
                values.emplace_back(isoline::runtime::I32 { static_cast<uint32_t>(argument) });
        auto results = std::vector<isoline::runtime::Value>(1);
        auto const function = _store.moduleInstance(_address).functions.at(0);
        auto const ended = isoline::runtime::invokeChecked(_store, _interpreter, function, values, results);
        if (!ended.outcome)
        {
            REQUIRE(std::holds_alternative<isoline::runtime::Trap>(ended.outcome.error()));
            return Outcome { .trap = std::get<isoline::runtime::Trap>(ended.outcome.error()).code };
        }
        return Outcome { .trap = std::nullopt, .value = std::get<isoline::runtime::I64>(results[0]).bits };
    }

  private:
    isoline::runtime::HeapMemoryAllocator _allocator;
    isoline::runtime::Store _store;
    isoline::interpreter::ReferenceInterpreter _interpreter;
    uint32_t _address = 0;
};

/// The indices worth running @p program on: the edges of its first memory and of the address type.
std::vector<uint64_t> indicesOf(Program const& program)
{
    auto const bytes = program.memories.front().limits.min << program.memories.front().pageSizeLog2;
    auto indices = std::vector<uint64_t> { 0, 1, 3, 4, 7, 8, bytes - 1, bytes, bytes + 1 };
    for (auto const size: { 1U, 2U, 4U, 8U })
        indices.push_back(bytes - size);
    if (program.addressType() == AddressType::I64)
        indices.insert(indices.end(),
                       { All, All - 3, All - 7, uint64_t { 1 } << 63U, uint64_t { 1 } << 32U });
    else
        indices.insert(indices.end(), { 0xFFFF'FFFF, 0xFFFF'FFFC, 0xFFFF'FFF8, 0x8000'0000 });
    if (program.addressType() == AddressType::I32)
        for (auto& index: indices)
            index &= 0xFFFF'FFFF;
    return indices;
}

/// Runs @p program in both Spectre modes against the reference on every index of indicesOf(): the same
/// outcome architecturally, and every access in bounds when every check is mispredicted.
/// @return The checks the passes eliminated, by statistic, summed over both modes.
PassStatistics differential(Program const& program)
{
    auto totals = PassStatistics {};
    // One instance for every run: each evaluator starts from the memories as the reference holds them then,
    // grown by earlier runs or not, so both sides start alike.
    auto reference = Reference { program };
    for (auto const spectre: { SpectreGuard::Disabled, SpectreGuard::Enabled })
    {
        auto const optimized = optimize(program.module(), spectre);
        INFO(optimized.translated->text());
        for (auto const statistic: { PassStatistic::ChecksProvedByMinimum,
                                     PassStatistic::ChecksSharingDominatingAddress,
                                     PassStatistic::ChecksMovedFromDominatingAddress })
            totals.add(statistic, optimized.counted(statistic));
        for (auto const index: indicesOf(program))
        {
            INFO("index " << index);
            auto const arguments = std::array { index, uint64_t { 0 } };
            auto architectural =
                Evaluator { optimized.function(), reference.memories(), Speculation::Architectural };
            auto mispredicted =
                Evaluator { optimized.function(), reference.memories(), Speculation::Mispredicted };
            CHECK(architectural.run(arguments) == reference.run(program.addressType(), arguments));
            std::ignore = mispredicted.run(arguments);
        }
    }
    return totals;
}

/// A fixed sequence of 64-bit words (splitmix64), the same on every host and run: the programs are data,
/// not samples.
class SplitMix
{
  public:
    /// Starts the sequence at @p state.
    explicit SplitMix(uint64_t state) noexcept: _state { state } {}

    /// The next word.
    uint64_t operator()() noexcept
    {
        _state += 0x9E37'79B9'7F4A'7C15ULL;
        auto mix = _state;
        mix = (mix ^ (mix >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
        mix = (mix ^ (mix >> 27U)) * 0x94D0'49BB'1331'11EBULL;
        return mix ^ (mix >> 31U);
    }

  private:
    uint64_t _state; ///< The generator's state.
};

} // namespace

TEST_CASE("BoundsChecks.constantBelowMinimumIsElided")
{
    // Rule (a): a constant index whose access ends within the memory's declared minimum needs no check, in
    // either mode, and the verifier holds the in-bounds address to that minimum.
    SECTION("one page: i32.load at 100 and at 65'532 elided, at 65'533 kept")
    {
        for (auto const spectre: { SpectreGuard::Disabled, SpectreGuard::Enabled })
            for (auto const& [index, elided]: std::array {
                     std::pair { 100, true }, std::pair { 65'532, true }, std::pair { 65'533, false } })
            {
                INFO("index " << index);
                auto const program =
                    Program { .memories = { memoryOf(1) }, .steps = { constantLoadOf(index, 0, 4) } };
                auto const optimized = optimize(program.module(), spectre);
                CHECK(optimized.countOf(Operation::CheckMemory) == (elided ? 0U : 1U));
                CHECK(optimized.countOf(Operation::InBoundsAddress) == (elided ? 1U : 0U));
                CHECK(optimized.counted(PassStatistic::ChecksProvedByMinimum) == (elided ? 1U : 0U));
            }
    }
    SECTION("the offset counts: an index of 0 at offset 65'533 is kept")
    {
        auto const program =
            Program { .memories = { memoryOf(1) }, .steps = { constantLoadOf(0, 65'533, 4) } };
        auto const optimized = optimize(program.module(), SpectreGuard::Enabled);
        CHECK(optimized.countOf(Operation::CheckMemory) == 1);
        CHECK(optimized.counted(PassStatistic::ChecksProvedByMinimum) == 0);
    }
    SECTION("an imported memory: its declared minimum bounds the one it is bound to from below")
    {
        auto builder = ModuleBuilder {};
        builder
            .importMemory(
                "host", "memory", Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 })
            .type(FuncType { .params = {}, .results = { isoline::binary::I64 } })
            .function(0, {}, code({ isoline::binary::testing::i32Const(16), { 0x29 }, memargOf(0, 0) }));
        auto const optimized = optimize(builder, SpectreGuard::Enabled);
        CHECK(optimized.counted(PassStatistic::ChecksProvedByMinimum) == 1);
        CHECK(optimized.countOf(Operation::CheckMemory) == 0);
    }
    SECTION("a memory of no pages: nothing is elided")
    {
        auto const program = Program { .memories = { memoryOf(0) }, .steps = { constantLoadOf(0, 0, 1) } };
        auto const optimized = optimize(program.module(), SpectreGuard::Enabled);
        CHECK(optimized.countOf(Operation::CheckMemory) == 1);
    }
    SECTION("custom page sizes: a memory of five 1-byte pages holds an i32 at 1, not at 2")
    {
        for (auto const& [index, elided]: std::array { std::pair { 1, true }, std::pair { 2, false } })
        {
            INFO("index " << index);
            auto const program =
                Program { .memories = { memoryOf(5, 0) }, .steps = { constantLoadOf(index, 0, 4) } };
            auto const optimized = optimize(program.module(), SpectreGuard::Enabled);
            CHECK(optimized.counted(PassStatistic::ChecksProvedByMinimum) == (elided ? 1U : 0U));
            std::ignore = differential(program);
        }
    }
    SECTION("the verifier holds an in-bounds address to the minimum it is given")
    {
        auto const program = Program { .memories = { memoryOf(1) }, .steps = { constantLoadOf(100, 0, 4) } };
        auto const optimized = optimize(program.module(), SpectreGuard::Enabled);
        REQUIRE(optimized.countOf(Operation::InBoundsAddress) == 1);
        CHECK(verify(optimized.function(), optimized.minimums).has_value());
        auto const smaller = std::array { uint64_t { 103 } };
        auto const refused = verify(optimized.function(), smaller);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == intermediate::VerifierErrorCode::UnguardedAddress);
        auto const none = verify(optimized.function());
        REQUIRE_FALSE(none.has_value());
        CHECK(none.error().code == intermediate::VerifierErrorCode::UnguardedAddress);
    }
}

TEST_CASE("BoundsChecks.dominatingWiderCheckElides")
{
    // Rule (b), Spectre off: an i64.load at index i dominates an i32.load at i + 4 and an i32.load at i: both
    // checks go. The first is moved from the dominating address by guardAddress, the second shares it.
    auto const program =
        Program { .memories = { memoryOf(1) }, .steps = { loadOf(0, 8), loadOf(4, 4), loadOf(0, 4) } };
    auto const optimized = optimize(program.module(), SpectreGuard::Disabled);
    CHECK(optimized.countOf(Operation::CheckMemory) == 1);
    CHECK(optimized.countOf(Operation::GuardAddress) == 1);
    CHECK(optimized.counted(PassStatistic::ChecksMovedFromDominatingAddress) == 1);
    CHECK(optimized.counted(PassStatistic::ChecksSharingDominatingAddress) == 1);
    CHECK(optimized.counted(PassStatistic::ChecksProvedByMinimum) == 0);
    auto const totals = differential(program);
    CHECK(totals.count(PassStatistic::ChecksMovedFromDominatingAddress) == 2);
}

TEST_CASE("BoundsChecks.spectreKeepsTheClampWhenEliding")
{
    // Spectre on: the dominated check becomes guardAddress of the dominating check's address. Its trap
    // branch is gone (one trap site left), its clamp is the dominating check's select, which guardAddress
    // keeps (0 stays 0): under misprediction of every check, every access still reads in bounds, on every
    // GuardEdges row (Evaluator's Mispredicted run), and architecturally it ends as the reference does.
    auto const program = Program { .memories = { memoryOf(1) }, .steps = { loadOf(0, 8), loadOf(4, 4) } };
    auto const optimized = optimize(program.module(), SpectreGuard::Enabled);
    REQUIRE(optimized.countOf(Operation::CheckMemory) == 1);
    REQUIRE(optimized.countOf(Operation::GuardAddress) == 1);
    CHECK(optimized.counted(PassStatistic::ChecksMovedFromDominatingAddress) == 1);
    // The guardAddress moves the checked address, the one the select clamps, and the load takes it.
    auto const& function = optimized.function();
    for (auto const block: std::views::iota(uint32_t { 0 }, function.blockCount()))
        for (auto const index: function.block(BlockId { block }).instructions)
        {
            auto const& instruction = function.instruction(index);
            if (instruction.operation != Operation::GuardAddress)
                continue;
            CHECK(function.immediates(instruction.immediates)[0] == 4);
            auto const moved = function.operands(instruction.operands)[0];
            auto const defining = std::ranges::find_if(
                std::views::iota(uint32_t { 0 }, function.instructionCount()), [&](uint32_t candidate) {
                    auto const& other = function.instruction(candidate);
                    return other.operation == Operation::CheckMemory
                           && function.operands(other.results)[0] == moved;
                });
            CHECK(defining != std::views::iota(uint32_t { 0 }, function.instructionCount()).end());
        }
    for (auto const& edge: MemoryGuardEdges)
    {
        INFO("edge index " << edge.index << " offset " << edge.offset << " size " << edge.size << " bound "
                           << edge.bound);
        auto const isI64 = edge.indexType == Type::I64;
        auto const fitsMemarg = isI64 || edge.offset <= 0xFFFF'FFFF;
        if (!fitsMemarg)
            continue;
        // The memory is the row's bound: 64 KiB pages where it is a whole number of them, 1-byte pages else.
        auto const pageSizeLog2 = edge.bound % PageBytes == 0 && edge.bound != 0 ? 16 : 0;
        auto const pages = edge.bound >> pageSizeLog2;
        auto const type = isI64 ? AddressType::I64 : AddressType::I32;
        // The row's access dominates a narrower one inside it, and the same constant index loads too.
        auto steps = std::vector<Step> { loadOf(edge.offset, edge.size) };
        auto const narrowOffset = edge.offset + (edge.size / 2);
        auto const hasNarrow = edge.size > 1 && (isI64 || narrowOffset <= 0xFFFF'FFFF);
        if (hasNarrow)
            steps.push_back(loadOf(narrowOffset, edge.size / 2));
        steps.push_back(loadOf(edge.offset, edge.size));
        steps.push_back(constantLoadOf(edge.index, edge.offset, edge.size));
        auto const edgeProgram =
            Program { .memories = { memoryOf(pages, static_cast<uint8_t>(pageSizeLog2), type) },
                      .steps = steps };
        auto const totals = differential(edgeProgram);
        // A memory64 access whose static end alone passes 2^64 translates to a trap, and what follows it to
        // nothing: no check is left to elide.
        if (!isoline::compiler::intermediate::staticEndOverflows(edge.offset, edge.size))
        {
            CHECK(totals.count(PassStatistic::ChecksSharingDominatingAddress) == 2);
            CHECK(totals.count(PassStatistic::ChecksMovedFromDominatingAddress) == (hasNarrow ? 2U : 0U));
        }
        // Run on the row's own index too.
        auto reference = Reference { edgeProgram };
        auto const index = isI64 ? edge.index : edge.index & 0xFFFF'FFFF;
        auto const arguments = std::array { index, uint64_t { 0 } };
        auto const edgeOptimized = optimize(edgeProgram.module(), SpectreGuard::Enabled);
        auto architectural =
            Evaluator { edgeOptimized.function(), reference.memories(), Speculation::Architectural };
        auto mispredicted =
            Evaluator { edgeOptimized.function(), reference.memories(), Speculation::Mispredicted };
        CHECK(architectural.run(arguments) == reference.run(type, arguments));
        std::ignore = mispredicted.run(arguments);
    }
}

TEST_CASE("BoundsChecks.narrowerDominatorDoesNotElide")
{
    // A dominating check proves only the bytes it covers: a narrower one, one ending earlier, or one starting
    // later than the dominated access (guardAddress moves an address forward only) proves nothing.
    for (auto const& steps: { std::vector { loadOf(0, 1), loadOf(0, 4) },
                              std::vector { loadOf(0, 4), loadOf(2, 4) },
                              std::vector { loadOf(4, 4), loadOf(0, 4) } })
    {
        auto const program = Program { .memories = { memoryOf(1) }, .steps = steps };
        INFO("first " << steps[0].offset << "+" << steps[0].size << ", then " << steps[1].offset << "+"
                      << steps[1].size);
        auto const optimized = optimize(program.module(), SpectreGuard::Enabled);
        CHECK(optimized.countOf(Operation::CheckMemory) == 2);
        CHECK(optimized.countOf(Operation::GuardAddress) == 0);
        auto const totals = differential(program);
        CHECK(totals.count(PassStatistic::ChecksMovedFromDominatingAddress) == 0);
        CHECK(totals.count(PassStatistic::ChecksSharingDominatingAddress) == 0);
    }
}

TEST_CASE("BoundsChecks.neverAcrossSiblingsMemoriesOrIndices")
{
    // Rule (b) needs the same memory, the same SSA index, a dominating check, and nothing on any path
    // between that may grow or move a memory: a call, a stub (memory.grow among them) or a poll, whose
    // observer may run host code.
    auto const keptBoth = [](Program const& program) {
        auto const optimized = optimize(program.module(), SpectreGuard::Enabled);
        CHECK(optimized.countOf(Operation::CheckMemory) == 2);
        CHECK(optimized.counted(PassStatistic::ChecksMovedFromDominatingAddress) == 0);
        CHECK(optimized.counted(PassStatistic::ChecksSharingDominatingAddress) == 0);
        std::ignore = differential(program);
    };
    SECTION("another memory")
    {
        keptBoth(
            Program { .memories = { memoryOf(1), memoryOf(1) }, .steps = { loadOf(0, 8), loadOf(0, 4, 1) } });
    }
    SECTION("another memory between keeps the first one's proof")
    {
        auto const program =
            Program { .memories = { memoryOf(1), memoryOf(1) },
                      .steps = { loadOf(0, 8), loadOf(0, 8, 1), loadOf(0, 4), loadOf(0, 4, 1) } };
        auto const optimized = optimize(program.module(), SpectreGuard::Enabled);
        CHECK(optimized.countOf(Operation::CheckMemory) == 2);
        CHECK(optimized.counted(PassStatistic::ChecksSharingDominatingAddress) == 2);
        std::ignore = differential(program);
    }
    SECTION("a fact past the lookup's reads is not found: kept")
    {
        // Five memories checked at one index, then the first again: the lookup reads the newest four facts
        // (FactsReadPerIndex), none of the first memory's, so the last check stays.
        auto steps = std::vector<Step> {};
        for (auto const memory: std::views::iota(uint32_t { 0 }, uint32_t { 5 }))
            steps.push_back(loadOf(0, 8, memory));
        steps.push_back(loadOf(0, 4, 0));
        auto const program = Program {
            .memories = { memoryOf(1), memoryOf(1), memoryOf(1), memoryOf(1), memoryOf(1) },
            .steps = steps,
        };
        auto const optimized = optimize(program.module(), SpectreGuard::Enabled);
        CHECK(optimized.countOf(Operation::CheckMemory) == 6);
        CHECK(optimized.counted(PassStatistic::ChecksSharingDominatingAddress) == 0);
        std::ignore = differential(program);
    }
    SECTION("another SSA index")
    {
        auto second = loadOf(0, 4);
        second.index = IndexSource::Second;
        keptBoth(Program { .memories = { memoryOf(1) }, .steps = { loadOf(0, 8), second } });
    }
    SECTION("a memory.grow between: kept, its base may have moved")
    {
        keptBoth(Program { .memories = { memoryOf(1) },
                           .steps = { loadOf(0, 8), Step { .kind = StepKind::Grow }, loadOf(0, 4) } });
    }
    SECTION("a memory.grow of another memory between: kept, any stub may move any memory")
    {
        keptBoth(Program {
            .memories = { memoryOf(1), memoryOf(1) },
            .steps = { loadOf(0, 8), Step { .kind = StepKind::Grow, .memory = 1 }, loadOf(0, 4) } });
    }
    SECTION("a call between: kept")
    {
        keptBoth(Program { .memories = { memoryOf(1) },
                           .steps = { loadOf(0, 8), Step { .kind = StepKind::Call }, loadOf(0, 4) } });
    }
    SECTION("if/else siblings: neither dominates the other")
    {
        // (if (local.get 1) (then load i) (else load i)): two checks, neither elided.
        auto const load = code({ isoline::binary::testing::localGet(0), { 0x29 }, memargOf(0, 0), { 0x1A } });
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = { isoline::binary::I32, isoline::binary::I32 }, .results = {} })
            .memory(memoryOf(1))
            .function(0,
                      {},
                      code({ isoline::binary::testing::localGet(1),
                             { 0x04, 0x40 },
                             load,
                             { 0x05 },
                             load,
                             { 0x0B } }));
        auto const optimized = optimize(builder, SpectreGuard::Enabled);
        CHECK(optimized.countOf(Operation::CheckMemory) == 2);
        CHECK(optimized.counted(PassStatistic::ChecksSharingDominatingAddress) == 0);
    }
    SECTION("a loop's back edge polls: a check before the loop proves nothing inside it")
    {
        // load i; (loop (load i) (br_if 0 (local.get 1))): the poll on the back edge may run an observer.
        auto const load = code({ isoline::binary::testing::localGet(0), { 0x29 }, memargOf(0, 0), { 0x1A } });
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = { isoline::binary::I32, isoline::binary::I32 }, .results = {} })
            .memory(memoryOf(1))
            .function(0,
                      {},
                      code({ load,
                             isoline::binary::testing::loop(code({ load,
                                                                   isoline::binary::testing::localGet(1),
                                                                   isoline::binary::testing::brIf(0) })) }));
        auto const optimized = optimize(builder, SpectreGuard::Enabled);
        CHECK(optimized.countOf(Operation::CheckMemory) == 2);
        CHECK(optimized.counted(PassStatistic::ChecksSharingDominatingAddress) == 0);
    }
    SECTION("a dominating check in an enclosing block, no barrier between: elided")
    {
        // load i; (if (local.get 1) (then load i)): the then arm is dominated, and no poll or call is
        // between.
        auto const load = code({ isoline::binary::testing::localGet(0), { 0x29 }, memargOf(0, 0), { 0x1A } });
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = { isoline::binary::I32, isoline::binary::I32 }, .results = {} })
            .memory(memoryOf(1))
            .function(
                0, {}, code({ load, isoline::binary::testing::localGet(1), { 0x04, 0x40 }, load, { 0x0B } }));
        auto const optimized = optimize(builder, SpectreGuard::Enabled);
        CHECK(optimized.countOf(Operation::CheckMemory) == 1);
        CHECK(optimized.counted(PassStatistic::ChecksSharingDominatingAddress) == 1);
    }
}

TEST_CASE("BoundsChecks.brTableEndsTheProofOnlyOnTheArmThatCalls")
{
    // load i; (br_table on the second parameter: arm 0 loads i, arm 1 calls and then loads i, the default
    // goes on); after the arms meet, load i. Arm 0 shares the first check's address; arm 1's check follows a
    // call, and the merge's follows a path through it: both kept.
    auto const load = code({ isoline::binary::testing::localGet(0), { 0x29 }, memargOf(0, 0) });
    auto const dropped = code({ load, isoline::binary::testing::drop() });
    auto builder = ModuleBuilder {};
    builder
        .type(FuncType { .params = { isoline::binary::I32, isoline::binary::I32 },
                         .results = { isoline::binary::I64 } })
        .type(FuncType {})
        .memory(memoryOf(1))
        .function(0,
                  {},
                  code({ dropped,
                         { 0x02, 0x40 }, // block $exit
                         { 0x02, 0x40 }, // block $calls
                         { 0x02, 0x40 }, // block $shares
                         isoline::binary::testing::localGet(1),
                         { 0x0E, 0x02, 0x00, 0x01, 0x02 }, // br_table $shares $calls $exit
                         { 0x0B },                         // end $shares
                         dropped,
                         isoline::binary::testing::br(1), // to $exit
                         { 0x0B },                        // end $calls
                         isoline::binary::testing::call(1),
                         dropped,
                         { 0x0B }, // end $exit
                         load }))
        .function(1, {}, Bytes {});
    auto reference = Reference { builder };
    for (auto const spectre: { SpectreGuard::Disabled, SpectreGuard::Enabled })
    {
        auto const optimized = optimize(builder, spectre);
        INFO(optimized.translated->text());
        CHECK(optimized.countOf(Operation::Switch) == 1);
        CHECK(optimized.countOf(Operation::CheckMemory) == 3);
        CHECK(optimized.counted(PassStatistic::ChecksSharingDominatingAddress) == 1);
        for (auto const arm: { uint64_t { 0 }, uint64_t { 1 }, uint64_t { 2 }, uint64_t { 7 } })
            for (auto const index: { uint64_t { 0 }, uint64_t { 65'532 }, uint64_t { 65'533 } })
            {
                INFO("arm " << arm << " index " << index);
                auto const arguments = std::array { index, arm };
                auto architectural =
                    Evaluator { optimized.function(), reference.memories(), Speculation::Architectural };
                auto mispredicted =
                    Evaluator { optimized.function(), reference.memories(), Speculation::Mispredicted };
                CHECK(architectural.run(arguments) == reference.run(AddressType::I32, arguments));
                std::ignore = mispredicted.run(arguments);
            }
    }
}

TEST_CASE("BoundsChecks.memory64OverflowCheckKeptUnlessConstant")
{
    // A 64-bit memory's check also catches an end that wraps past 2^64. A variable index keeps it unless a
    // dominating check of a wider access already proved its end; a constant index is elided only when its
    // end, computed without wrapping, is within the minimum: one near 2^64 keeps its check.
    auto const memory = memoryOf(1, 16, AddressType::I64);
    SECTION("a variable index: kept alone, elided under a wider dominating check")
    {
        auto const alone = optimize(Program { .memories = { memory }, .steps = { loadOf(8, 8) } }.module(),
                                    SpectreGuard::Enabled);
        CHECK(alone.countOf(Operation::CheckMemory) == 1);
        auto const program =
            Program { .memories = { memory }, .steps = { loadOf(All - 15, 8), loadOf(All - 11, 4) } };
        auto const dominated = optimize(program.module(), SpectreGuard::Enabled);
        CHECK(dominated.countOf(Operation::CheckMemory) == 1);
        CHECK(dominated.counted(PassStatistic::ChecksMovedFromDominatingAddress) == 1);
        std::ignore = differential(program);
    }
    SECTION("a constant index whose end wraps past 2^64: kept, and it traps")
    {
        for (auto const& [index, offset]: std::array { std::pair { All - 3, uint64_t { 8 } },
                                                       std::pair { uint64_t { 8 }, All - 3 },
                                                       std::pair { All, uint64_t { 1 } } })
        {
            INFO("index " << index << " offset " << offset);
            auto const program =
                Program { .memories = { memory }, .steps = { constantLoadOf(index, offset, 4) } };
            auto const optimized = optimize(program.module(), SpectreGuard::Enabled);
            CHECK(optimized.counted(PassStatistic::ChecksProvedByMinimum) == 0);
            std::ignore = differential(program);
        }
    }
    SECTION("a constant index within the minimum: elided")
    {
        auto const program =
            Program { .memories = { memory }, .steps = { constantLoadOf(PageBytes - 8, 0, 8) } };
        auto const optimized = optimize(program.module(), SpectreGuard::Enabled);
        CHECK(optimized.countOf(Operation::CheckMemory) == 0);
        CHECK(optimized.counted(PassStatistic::ChecksProvedByMinimum) == 1);
        std::ignore = differential(program);
    }
}

TEST_CASE("BoundsChecks.randomProgramsNearTheBounds")
{
    // Seeded programs of three to six steps near the edges of a memory: offsets at 0, near the size, near
    // 2^32 and (memory64) near 2^64; every width; constant and variable indices; a grow or a call between
    // now and then; one memory or two; 64 KiB or 1-byte pages. Each runs against the reference on the
    // indices at the edges (differential()), in both modes; the totals show every rule was taken.
    auto random = SplitMix { 0x15'01'1E'08 };
    auto const pick = [&](auto const& choices) {
        return choices[random() % choices.size()];
    };
    auto totals = PassStatistics {};
    for ([[maybe_unused]] auto const program: std::views::iota(0, 120))
    {
        auto const type = pick(std::array { AddressType::I32, AddressType::I64 });
        auto const pageSizeLog2 = pick(std::array<uint8_t, 2> { 16, 0 });
        auto const pages = pageSizeLog2 == 16 ? pick(std::array<uint64_t, 3> { 0, 1, 2 })
                                              : pick(std::array<uint64_t, 4> { 0, 5, 8, 64 });
        auto const memories = pick(std::array<uint32_t, 2> { 1, 2 });
        auto const bytes = pages << pageSizeLog2;
        auto offsets =
            std::vector<uint64_t> { 0, 1, 4, 7, bytes, bytes - 4, bytes - 8, 0xFFFF'FFFF, 0xFFFF'FFF8 };
        if (type == AddressType::I64)
            offsets.insert(offsets.end(), { All, All - 7, uint64_t { 1 } << 63U });
        auto generated = Program {};
        for ([[maybe_unused]] auto const memory: std::views::iota(uint32_t { 0 }, memories))
            generated.memories.push_back(memoryOf(pages, pageSizeLog2, type));
        for ([[maybe_unused]] auto const step: std::views::iota(0, 3 + static_cast<int>(random() % 4)))
        {
            auto const roll = random() % 12;
            if (roll == 0)
            {
                generated.steps.push_back(
                    Step { .kind = StepKind::Grow, .memory = static_cast<uint32_t>(random() % memories) });
                continue;
            }
            if (roll == 1)
            {
                generated.steps.push_back(Step { .kind = StepKind::Call });
                continue;
            }
            auto load = loadOf(pick(offsets),
                               pick(std::array<uint64_t, 4> { 1, 2, 4, 8 }),
                               static_cast<uint32_t>(random() % memories));
            if (type == AddressType::I32)
                load.offset &= 0xFFFF'FFFF;
            if (roll == 2)
            {
                load.index = IndexSource::Constant;
                load.constant = pick(offsets);
                if (type == AddressType::I32)
                    load.constant &= 0xFFFF'FFFF;
            }
            generated.steps.push_back(load);
        }
        auto const counted = differential(generated);
        for (auto const statistic: { PassStatistic::ChecksProvedByMinimum,
                                     PassStatistic::ChecksSharingDominatingAddress,
                                     PassStatistic::ChecksMovedFromDominatingAddress })
            totals.add(statistic, counted.count(statistic));
    }
    CHECK(totals.count(PassStatistic::ChecksProvedByMinimum) > 0);
    CHECK(totals.count(PassStatistic::ChecksSharingDominatingAddress) > 0);
    CHECK(totals.count(PassStatistic::ChecksMovedFromDominatingAddress) > 0);
}

TEST_CASE("BoundsChecks.deepDominatorTreeUnderTheDefaultStack")
{
    // 100'000 blocks, each dominating the next and checking the same index: the walk keeps its scopes on an
    // explicit stack, and every check after the first shares the first's address. Verified once, at the end,
    // as a release compile does: the verifier's own depth tests are its own.
    constexpr auto Depth = uint32_t { 100'000 };
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto function = Function { memory };
    auto builder = Builder { function };
    auto const entry = builder.createBlock();
    builder.switchTo(entry);
    auto const index = builder.addParameter(entry, Type::I32);
    auto const condition = builder.addParameter(entry, Type::I32);
    auto const bound = builder.addParameter(entry, Type::I64);
    auto const base = builder.emit(
        Operation::MemoryBase, std::array { Type::Pointer }, {}, std::array<uint64_t, 1> { 0 })[0];
    auto const exit = builder.createBlock();
    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, Depth))
    {
        auto const checked = builder.emit(Operation::CheckMemory,
                                          std::array { Type::Pointer },
                                          std::array { index, base, bound },
                                          std::array<uint64_t, 3> { 0, 0, 4 })[0];
        std::ignore = builder.emit(Operation::Load,
                                   std::array { Type::I32 },
                                   std::array { checked },
                                   std::array<uint64_t, 4> { 0, 4, 0, std::to_underlying(Type::I32) });
        auto const next = builder.createBlock();
        builder.branch(condition, next, {}, exit, {});
        builder.switchTo(next);
    }
    builder.jump(exit, {});
    builder.switchTo(exit);
    builder.ret({});
    auto const anyModule = translateOf(Program { .memories = { memoryOf(1) }, .steps = {} }.module());
    auto const config = CompileConfig { TargetDescription {}, ExecutionMode::Production };
    auto const cancellation = CompileCancellation {};
    auto context = PassContext { .module = *anyModule->module,
                                 .config = config,
                                 .cancellation = cancellation,
                                 .memory = memory,
                                 .verification = Verification::AtEnd };
    auto const row = rowOf(Pass::EliminateBoundsChecks);
    REQUIRE(runPasses(function, context, row).has_value());
    CHECK(context.statistics.count(PassStatistic::ChecksSharingDominatingAddress) == Depth - 1);
}

TEST_CASE("BoundsChecks.pollsTheCancellation")
{
    // Cancelled before it starts, on more than one interval of work, the pass stops with Cancelled: in a
    // block the dominator walk enters (blocks of checks), and in the entry (one long block of them).
    for (auto const longEntry: { false, true })
    {
        INFO((longEntry ? "one long entry block" : "many blocks"));
        auto memory = std::pmr::monotonic_buffer_resource {};
        auto function = Function { memory };
        auto builder = Builder { function };
        auto const entry = builder.createBlock();
        builder.switchTo(entry);
        auto const index = builder.addParameter(entry, Type::I32);
        auto const condition = builder.addParameter(entry, Type::I32);
        auto const bound = builder.addParameter(entry, Type::I64);
        auto const base = builder.emit(
            Operation::MemoryBase, std::array { Type::Pointer }, {}, std::array<uint64_t, 1> { 0 })[0];
        auto const exit = builder.createBlock();
        for ([[maybe_unused]] auto const level:
             std::views::iota(uint32_t { 0 }, longEntry ? 70'000U : 40'000U))
        {
            std::ignore = builder.emit(Operation::CheckMemory,
                                       std::array { Type::Pointer },
                                       std::array { index, base, bound },
                                       std::array<uint64_t, 3> { 0, 0, 4 });
            if (longEntry)
                continue;
            auto const next = builder.createBlock();
            builder.branch(condition, next, {}, exit, {});
            builder.switchTo(next);
        }
        builder.jump(exit, {});
        builder.switchTo(exit);
        builder.ret({});
        auto const anyModule = translateOf(Program { .memories = { memoryOf(1) }, .steps = {} }.module());
        auto const config = CompileConfig { TargetDescription {}, ExecutionMode::Production };
        auto cancellation = CompileCancellation {};
        cancellation.cancel();
        auto context = PassContext { .module = *anyModule->module,
                                     .config = config,
                                     .cancellation = cancellation,
                                     .memory = memory,
                                     .verification = Verification::AtEnd,
                                     .codeIndex = 9 };
        auto const ran = rowOf(Pass::EliminateBoundsChecks)[0].run(function, context);
        REQUIRE_FALSE(ran.has_value());
        CHECK(ran.error().code == isoline::compiler::CompileErrorCode::Cancelled);
        CHECK(ran.error().function == 9U);
    }
}

TEST_CASE("BoundsChecks.linearOnLongChains", "[.perf]")
{
    // [performance] 1'000'000 checks of one index in one block, and as many of 1'000 indices: under 2 s in a
    // release build, with the verifier off as a release compile runs (it verifies once, at the end).
    for (auto const indices: { 1U, 1'000U })
    {
        auto memory = std::pmr::monotonic_buffer_resource {};
        auto function = Function { memory };
        auto builder = Builder { function };
        auto const entry = builder.createBlock();
        builder.switchTo(entry);
        auto parameters = std::vector<ValueId> {};
        for ([[maybe_unused]] auto const index: std::views::iota(0U, indices))
            parameters.push_back(builder.addParameter(entry, Type::I32));
        auto const bound = builder.addParameter(entry, Type::I64);
        auto const base = builder.emit(
            Operation::MemoryBase, std::array { Type::Pointer }, {}, std::array<uint64_t, 1> { 0 })[0];
        for (auto const access: std::views::iota(0U, 1'000'000U))
        {
            auto const checked = builder.emit(Operation::CheckMemory,
                                              std::array { Type::Pointer },
                                              std::array { parameters[access % indices], base, bound },
                                              std::array<uint64_t, 3> { 0, access % 8, 1 })[0];
            std::ignore = builder.emit(Operation::Load,
                                       std::array { Type::I32 },
                                       std::array { checked },
                                       std::array<uint64_t, 4> { 0, 1, 0, std::to_underlying(Type::I32) });
        }
        builder.ret({});
        auto const anyModule = translateOf(Program { .memories = { memoryOf(1) }, .steps = {} }.module());
        auto const config = CompileConfig { TargetDescription {}, ExecutionMode::Production };
        auto const cancellation = CompileCancellation {};
        auto context = PassContext { .module = *anyModule->module,
                                     .config = config,
                                     .cancellation = cancellation,
                                     .memory = memory,
                                     .verification = Verification::AtEnd };
        auto const row = rowOf(Pass::EliminateBoundsChecks);
        auto const started = std::chrono::steady_clock::now();
        REQUIRE(runPasses(function, context, row).has_value());
        auto const seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        INFO(indices << " indices: " << seconds << " s");
        CHECK(seconds < 2.0);
    }
}
