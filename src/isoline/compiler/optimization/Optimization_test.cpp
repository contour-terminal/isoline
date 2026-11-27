// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/intermediate/Printer.hpp>
#include <isoline/compiler/intermediate/Verifier.hpp>
#include <isoline/compiler/intermediate/testing/GuardEdges.hpp>
#include <isoline/compiler/machine/TargetDescription.hpp>
#include <isoline/compiler/optimization/FoldingTable.hpp>
#include <isoline/compiler/optimization/PassTable.hpp>
#include <isoline/compiler/optimization/Rewrite.hpp>
#include <isoline/compiler/translation/testing/OpcodeSamples.hpp>
#include <isoline/compiler/translation/testing/Translation.hpp>
#include <isoline/interpreter/Numerics.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidatedModule.hpp>

using isoline::binary::FuncType;
using isoline::binary::Opcode;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::ModuleBuilder;
using isoline::compiler::CompileCancellation;
using isoline::compiler::CompileConfig;
using isoline::compiler::CompileErrorCode;
using isoline::compiler::intermediate::BlockId;
using isoline::compiler::intermediate::BranchTo;
using isoline::compiler::intermediate::Builder;
using isoline::compiler::intermediate::Function;
using isoline::compiler::intermediate::Operation;
using isoline::compiler::intermediate::OperationTable;
using isoline::compiler::intermediate::Type;
using isoline::compiler::intermediate::ValueId;
using isoline::compiler::intermediate::verify;
using isoline::compiler::machine::TargetDescription;
using isoline::compiler::optimization::foldingRowOf;
using isoline::compiler::optimization::FoldingTable;
using isoline::compiler::optimization::isFoldable;
using isoline::compiler::optimization::Pass;
using isoline::compiler::optimization::PassContext;
using isoline::compiler::optimization::PassOutcome;
using isoline::compiler::optimization::PassRow;
using isoline::compiler::optimization::PassStatistic;
using isoline::compiler::optimization::PassStatistics;
using isoline::compiler::optimization::PassStatisticTable;
using isoline::compiler::optimization::PassTable;
using isoline::compiler::optimization::runPasses;
using isoline::compiler::optimization::Verification;
using isoline::compiler::translation::testing::oneFunction;
using isoline::compiler::translation::testing::Options;
using isoline::compiler::translation::testing::translateOf;
using isoline::runtime::ExecutionMode;
using isoline::runtime::PollSite;
using isoline::runtime::TrapCode;
using isoline::validate::ValidatedModule;

namespace code = isoline::binary::testing;
namespace intermediate = isoline::compiler::intermediate;
namespace optimization = isoline::compiler::optimization;

namespace
{

constexpr auto WasmI32 = isoline::binary::I32;

/// A validated module for the passes' context when the function under test is built by hand.
std::shared_ptr<ValidatedModule const> const& anyModule()
{
    static auto const module = [] {
        auto builder = ModuleBuilder {};
        builder.type(FuncType {}).function(0, {}, Bytes {});
        auto loaded = ValidatedModule::load(
            builder.build(), isoline::FeatureSet::all(), isoline::binary::DecodeLimits {});
        REQUIRE(loaded.has_value());
        return std::shared_ptr<ValidatedModule const> { std::move(*loaded) };
    }();
    return module;
}

/// What the passes read, for one test.
struct Passes
{
    std::shared_ptr<ValidatedModule const> module = anyModule();
    CompileConfig config { TargetDescription {}, ExecutionMode::Production };
    CompileCancellation cancellation {};

    /// The context of a run over a function allocating from @p memory.
    PassContext contextFor(std::pmr::memory_resource& memory,
                           Verification verification = Verification::AfterEachPass)
    {
        return PassContext { .module = *module,
                             .config = config,
                             .cancellation = cancellation,
                             .memory = memory,
                             .verification = verification,
                             .codeIndex = 0 };
    }

    /// Runs @p rows over @p function, verifying after each against @p memoryMinimumBytes; the run must
    /// succeed.
    /// @return What the passes counted.
    PassStatistics run(Function& function,
                       std::span<PassRow const> rows = PassTable,
                       std::span<uint64_t const> memoryMinimumBytes = {})
    {
        auto context = contextFor(function.memory());
        context.memoryMinimumBytes = memoryMinimumBytes;
        auto const ran = runPasses(function, context, rows);
        INFO((ran ? std::string {} : isoline::compiler::describe(ran.error())));
        REQUIRE(ran.has_value());
        return context.statistics;
    }
};

/// The PassTable row of @p pass, alone.
std::array<PassRow, 1> rowOf(Pass pass)
{
    auto const found = std::ranges::find(PassTable, pass, &PassRow::pass);
    REQUIRE(found != PassTable.end());
    return { *found };
}

/// The instructions of @p function's blocks that are @p operation, removed ones excepted.
std::vector<uint32_t> listed(Function const& function, Operation operation)
{
    auto found = std::vector<uint32_t> {};
    for (auto const block: std::views::iota(uint32_t { 0 }, function.blockCount()))
        for (auto const index: function.block(BlockId { block }).instructions)
            if (function.instruction(index).operation == operation)
                found.push_back(index);
    return found;
}

/// How many of @p function's listed instructions are @p operation.
std::size_t countOf(Function const& function, Operation operation)
{
    return listed(function, operation).size();
}

/// How many listed polls @p function has at @p site (defined with the poll tests).
std::size_t pollsAt(Function const& function, PollSite site);

/// How many listed instructions @p function has, of any operation but Removed.
std::size_t instructionsOf(Function const& function)
{
    auto count = std::size_t { 0 };
    for (auto const block: std::views::iota(uint32_t { 0 }, function.blockCount()))
        for (auto const index: function.block(BlockId { block }).instructions)
            if (function.instruction(index).operation != Operation::Removed)
                ++count;
    return count;
}

/// A resource that counts the bytes allocated through it, from the heap.
class CountingResource final: public std::pmr::memory_resource
{
  public:
    /// How many bytes were allocated through it so far, freed ones included.
    [[nodiscard]] uint64_t allocated() const noexcept { return _allocated; }

  private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        _allocated += bytes;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override
    {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override
    {
        return this == &other;
    }

    uint64_t _allocated = 0;
};

/// A function built by hand: an entry block that is the builder's current one. Its arena counts what it
/// allocates.
struct Fixture
{
    CountingResource memory {};
    Function function { memory };
    Builder builder { function };
    BlockId entry = builder.createBlock();

    Fixture() { builder.switchTo(entry); }

    /// A constant of @p type with the bits @p bits.
    ValueId constant(Type type, uint64_t bits)
    {
        auto const operation = std::array {
            Operation::I32Const, Operation::I64Const, Operation::F32Const, Operation::F64Const
        }[std::to_underlying(type)];
        return builder.emit(operation, std::array { type }, {}, std::array { bits })[0];
    }

    /// One instruction of @p operation over @p operands, of one result of @p type.
    ValueId emit(Operation operation, Type type, std::span<ValueId const> operands)
    {
        return builder.emit(operation, std::array { type }, operands)[0];
    }
};

/// The type of each operand of @p row, which isFoldable() admits: Float as @p floatType.
std::vector<Type> operandTypesOf(intermediate::OperationRow const& row, Type floatType = Type::F32)
{
    auto types = std::vector<Type> {};
    for (auto const type: intermediate::operandSignatureRow(row.operands).fixed())
        switch (type)
        {
            case intermediate::OperandType::I32: types.push_back(Type::I32); break;
            case intermediate::OperandType::I64: types.push_back(Type::I64); break;
            case intermediate::OperandType::F32: types.push_back(Type::F32); break;
            case intermediate::OperandType::F64: types.push_back(Type::F64); break;
            default: types.push_back(floatType); break;
        }
    return types;
}

/// The result type of @p row for operands of @p operandTypes.
Type resultTypeOf(intermediate::OperationRow const& row, std::span<Type const> operandTypes)
{
    if (auto const fixed = intermediate::fixedTypeOf(row.result))
        return *fixed;
    return operandTypes.front();
}

/// The edge values of each numeric type: signs, widths, shift counts past the width, the extremes of every
/// truncation's range, zeros, infinities, subnormals, and NaNs canonical, signalling, negative and with
/// payloads.
std::vector<uint64_t> const& samplesOf(Type type)
{
    static auto const i32 = std::vector<uint64_t> {
        0,           1,           2,           7,           31,          32,          33,
        63,          64,          0x7F,        0x80,        0xFF,        0x7FFF,      0x8000,
        0x7FFF'FFFF, 0x8000'0000, 0x8000'0001, 0xFFFF'FFFF, 0xFFFF'FFFE, 0x1234'5678, 0xF0F0'0F0F,
    };
    static auto const i64 = std::vector<uint64_t> {
        0,
        1,
        2,
        31,
        32,
        63,
        64,
        65,
        0x7FFF'FFFF,
        0x8000'0000,
        0xFFFF'FFFF,
        0x1'0000'0000,
        0x7FFF'FFFF'FFFF'FFFF,
        0x8000'0000'0000'0000,
        0x8000'0000'0000'0001,
        0xFFFF'FFFF'FFFF'FFFF,
        0xFFFF'FFFF'FFFF'FFFE,
        0x0123'4567'89AB'CDEF,
        0xFFFF'FFFF'8000'0000,
    };
    static auto const f32 = std::vector<uint64_t> {
        0x0000'0000, 0x8000'0000, 0x3F80'0000, 0xBF80'0000, 0x3F00'0000, 0x3FC0'0000, 0x4020'0000,
        0xC020'0000, 0x7F7F'FFFF, 0x0000'0001, 0x0080'0000, 0x7F80'0000, 0xFF80'0000, 0x7FC0'0000,
        0xFFC0'0000, 0x7FA0'0001, 0x7F80'0001, 0x4EFF'FFFF, 0x4F00'0000, 0xCF00'0000, 0xCF00'0001,
        0x4F80'0000, 0x5F00'0000, 0xDF00'0000, 0x5F80'0000, 0xBF7F'FFFF, 0x4B00'0001,
    };
    static auto const f64 = std::vector<uint64_t> {
        0x0000'0000'0000'0000, 0x8000'0000'0000'0000, 0x3FF0'0000'0000'0000, 0xBFF0'0000'0000'0000,
        0x3FE0'0000'0000'0000, 0x3FF8'0000'0000'0000, 0x4004'0000'0000'0000, 0xC004'0000'0000'0000,
        0x7FEF'FFFF'FFFF'FFFF, 0x0000'0000'0000'0001, 0x7FF0'0000'0000'0000, 0xFFF0'0000'0000'0000,
        0x7FF8'0000'0000'0000, 0xFFF8'0000'0000'0000, 0x7FF4'0000'0000'0001, 0x7FF0'0000'0000'0001,
        0x41DF'FFFF'FFE0'0000, 0x41DF'FFFF'FFFF'FFFF, 0x41E0'0000'0000'0000, 0xC1E0'0000'0000'0000,
        0xC1E0'0000'0020'0000, 0x41EF'FFFF'FFFF'FFFF, 0x41F0'0000'0000'0000, 0x43E0'0000'0000'0000,
        0xC3E0'0000'0000'0000, 0x43F0'0000'0000'0000, 0xBFEF'FFFF'FFFF'FFFF, 0x47EF'FFFF'E000'0000,
        0x3690'0000'0000'0000,
    };
    switch (type)
    {
        case Type::I32: return i32;
        case Type::I64: return i64;
        case Type::F32: return f32;
        default: return f64;
    }
}

/// The opcode of the WebAssembly instruction @p operation is named after.
std::optional<Opcode> opcodeOf(Operation operation)
{
    for (auto const& row: isoline::binary::OpcodeTable)
        if (row.text == intermediate::nameOf(operation))
            return row.opcode;
    return std::nullopt;
}

/// @p bits as a runtime::Value of @p type.
isoline::runtime::Value valueOf(Type type, uint64_t bits)
{
    switch (type)
    {
        case Type::I32: return isoline::runtime::I32 { static_cast<uint32_t>(bits) };
        case Type::I64: return isoline::runtime::I64 { bits };
        case Type::F32: return isoline::runtime::F32 { static_cast<uint32_t>(bits) };
        default: return isoline::runtime::F64 { bits };
    }
}

/// The bits of a numeric runtime::Value, zero-extended.
uint64_t bitsOf(isoline::runtime::Value const& value)
{
    return std::visit(
        [](auto const& alternative) -> uint64_t {
            using Alternative = std::decay_t<decltype(alternative)>;
            if constexpr (std::is_same_v<Alternative, isoline::runtime::I32>
                          || std::is_same_v<Alternative, isoline::runtime::I64>
                          || std::is_same_v<Alternative, isoline::runtime::F32>
                          || std::is_same_v<Alternative, isoline::runtime::F64>)
                return uint64_t { alternative.bits };
            else
            {
                FAIL("not a numeric value");
                return 0;
            }
        },
        value);
}

/// What the reference interpreter computes for @p opcode on @p operands of @p types.
optimization::FoldResult referenceOf(Opcode opcode,
                                     std::span<Type const> types,
                                     std::span<uint64_t const> operands)
{
    using isoline::interpreter::NumericShape;
    auto const fromValue = [](std::expected<isoline::runtime::Value, isoline::runtime::Trap> const& result)
        -> optimization::FoldResult {
        if (!result)
            return std::unexpected { result.error().code };
        return bitsOf(*result);
    };
    switch (isoline::interpreter::numericShape(opcode))
    {
        case NumericShape::Unary:
            return fromValue(isoline::interpreter::applyUnary(opcode, valueOf(types[0], operands[0])));
        case NumericShape::Binary:
            return fromValue(isoline::interpreter::applyBinary(
                opcode, valueOf(types[0], operands[0]), valueOf(types[1], operands[1])));
        case NumericShape::Test:
            return uint64_t { isoline::interpreter::applyTest(opcode, valueOf(types[0], operands[0])).bits };
        case NumericShape::Compare:
            return uint64_t { isoline::interpreter::applyCompare(
                                  opcode, valueOf(types[0], operands[0]), valueOf(types[1], operands[1]))
                                  .bits };
        default: FAIL("not a numeric instruction the folder covers"); return 0;
    }
}

/// The text of @p result, for messages.
std::string textOf(optimization::FoldResult const& result)
{
    if (!result)
        return "trap " + std::to_string(std::to_underlying(result.error()));
    return std::to_string(*result);
}

} // namespace

TEST_CASE("Optimization.passTableOrder")
{
    // The plan's Decisions: trivial parameters, folding, control flow, bounds checks, dead code, control
    // flow.
    auto const order = std::to_array({ Pass::RemoveTrivialParameters,
                                       Pass::FoldConstants,
                                       Pass::SimplifyControlFlow,
                                       Pass::EliminateBoundsChecks,
                                       Pass::EliminateDeadCode,
                                       Pass::SimplifyControlFlow });
    REQUIRE(PassTable.size() == order.size());
    for (auto const [position, pass]: core::views::enumerate(order))
    {
        INFO("row " << position);
        CHECK(PassTable[position].pass == pass);
    }
    // One name per pass, distinct between passes.
    auto names = std::map<std::string_view, Pass> {};
    for (auto const& row: PassTable)
    {
        auto const [entry, inserted] = names.emplace(row.name, row.pass);
        CHECK(entry->second == row.pass);
        CHECK_FALSE(row.name.empty());
        CHECK(row.run != nullptr);
    }
    CHECK(names.size() == std::to_underlying(optimization::LastPass) + 1U);
}

TEST_CASE("Optimization.foldingTableCoversEveryFoldableRow")
{
    // Every WebAssembly numeric instruction has its operation folded, and every folded operation is one of
    // them or canonicalizeNan; the differential test below exercises each row.
    auto numeric = std::size_t { 0 };
    for (auto const& row: OperationTable)
    {
        INFO(intermediate::nameOf(row.operation));
        auto const opcode = opcodeOf(row.operation);
        auto const isNumeric =
            opcode.has_value()
            && isoline::interpreter::numericShape(*opcode) != isoline::interpreter::NumericShape::None
            && row.operands != intermediate::OperandSignature::None;
        if (isNumeric)
            ++numeric;
        CHECK(isFoldable(row) == foldingRowOf(row.operation).has_value());
        CHECK(foldingRowOf(row.operation).has_value()
              == (isNumeric || row.operation == Operation::CanonicalizeNan));
    }
    CHECK(FoldingTable.size() == numeric + 1);
    // Not folded: select (of any type), the guards, the memory and context operations, the traps.
    for (auto const operation: { Operation::Select,
                                 Operation::CheckMemory,
                                 Operation::GuardAddress,
                                 Operation::Load,
                                 Operation::TrapIf,
                                 Operation::Poll,
                                 Operation::LoadContext,
                                 Operation::RefIsNull })
        CHECK_FALSE(foldingRowOf(operation).has_value());
}

TEST_CASE("Optimization.foldsWithReferenceSemantics")
{
    SECTION("every row agrees with the reference interpreter on every edge value")
    {
        // A differential test: each kernel against interpreter::apply* on the same opcode, every sample of
        // each operand's type, pairs for binary rows. Bits and trap codes must be equal.
        auto exercised = std::size_t { 0 };
        for (auto const& row: FoldingTable)
        {
            if (row.operation == Operation::CanonicalizeNan)
                continue;
            auto const& operation = intermediate::operationRow(row.operation);
            auto const types = operandTypesOf(operation);
            auto const opcode = opcodeOf(row.operation);
            REQUIRE(opcode.has_value());
            auto const result = resultTypeOf(operation, types);
            auto disagreements = std::size_t { 0 };
            auto const check = [&](std::span<uint64_t const> operands) {
                auto const folded = row.kernel(operands, result);
                auto const reference = referenceOf(*opcode, types, operands);
                if (folded == reference)
                    return;
                ++disagreements;
                UNSCOPED_INFO(intermediate::nameOf(row.operation)
                              << "(" << operands[0]
                              << (operands.size() > 1 ? ", " + std::to_string(operands[1]) : "")
                              << "): folded " << textOf(folded) << ", reference " << textOf(reference));
            };
            for (auto const lhs: samplesOf(types[0]))
            {
                if (types.size() == 1)
                {
                    check(std::array { lhs });
                    continue;
                }
                for (auto const rhs: samplesOf(types[1]))
                    check(std::array { lhs, rhs });
            }
            CHECK(disagreements == 0);
            ++exercised;
        }
        CHECK(exercised + 1 == FoldingTable.size());
    }
    SECTION("the pass folds each row's first sample into the constant of the result's type")
    {
        // Through foldConstants: the folded instruction becomes the constant of the reference's bits, or the
        // block ends with the reference's trap.
        auto passes = Passes {};
        for (auto const& row: FoldingTable)
        {
            if (row.operation == Operation::CanonicalizeNan)
                continue;
            INFO(intermediate::nameOf(row.operation));
            auto const& operation = intermediate::operationRow(row.operation);
            auto const types = operandTypesOf(operation);
            auto const result = resultTypeOf(operation, types);
            auto fixture = Fixture {};
            auto operands = std::vector<ValueId> {};
            auto bits = std::vector<uint64_t> {};
            for (auto const type: types)
            {
                bits.push_back(samplesOf(type)[3]);
                operands.push_back(fixture.constant(type, bits.back()));
            }
            auto const value = fixture.emit(row.operation, result, operands);
            fixture.builder.ret(std::array { value });
            passes.run(fixture.function, rowOf(Pass::FoldConstants));
            auto const reference = referenceOf(*opcodeOf(row.operation), types, bits);
            auto const& function = fixture.function;
            if (!reference)
            {
                REQUIRE(countOf(function, Operation::Trap) == 1);
                auto const& trap = function.instruction(listed(function, Operation::Trap)[0]);
                CHECK(function.immediates(trap.immediates)[0] == std::to_underlying(reference.error()));
                CHECK(countOf(function, Operation::Return) == 0);
                continue;
            }
            auto const& returned = function.instruction(listed(function, Operation::Return)[0]);
            auto const returnedValue = function.operands(returned.operands)[0];
            auto const constants = listed(function,
                                          std::array { Operation::I32Const,
                                                       Operation::I64Const,
                                                       Operation::F32Const,
                                                       Operation::F64Const }[std::to_underlying(result)]);
            auto const defining = std::ranges::find_if(constants, [&](uint32_t index) {
                return function.operands(function.instruction(index).results)[0] == returnedValue;
            });
            REQUIRE(defining != constants.end());
            CHECK(function.immediates(function.instruction(*defining).immediates)[0] == *reference);
            CHECK(countOf(function, row.operation) == 0);
        }
    }
    SECTION("the plan's samples, and integer traps and shifts")
    {
        struct Sample
        {
            Operation operation;
            Type type;
            uint64_t lhs;
            uint64_t rhs;
            optimization::FoldResult expected;
        };
        auto const samples = std::to_array<Sample>({
            { Operation::F32Div, Type::F32, 0, 0, uint64_t { 0x7FC0'0000 } },
            { Operation::F32Div, Type::F32, 0x8000'0000, 0, uint64_t { 0x7FC0'0000 } },
            { Operation::F64Div, Type::F64, 0, 0, uint64_t { 0x7FF8'0000'0000'0000 } },
            { Operation::F32Add, Type::F32, 0x7FA0'0001, 0x3F80'0000, uint64_t { 0x7FC0'0000 } },
            { Operation::F32Min, Type::F32, 0x8000'0000, 0, uint64_t { 0x8000'0000 } },
            { Operation::F32Max, Type::F32, 0x8000'0000, 0, uint64_t { 0 } },
            { Operation::I32DivS, Type::I32, 1, 0, std::unexpected { TrapCode::IntegerDivideByZero } },
            { Operation::I32DivS,
              Type::I32,
              0x8000'0000,
              0xFFFF'FFFF,
              std::unexpected { TrapCode::IntegerOverflow } },
            { Operation::I32RemS, Type::I32, 0x8000'0000, 0xFFFF'FFFF, uint64_t { 0 } },
            { Operation::I32RemS, Type::I32, 1, 0, std::unexpected { TrapCode::IntegerDivideByZero } },
            { Operation::I32DivU, Type::I32, 7, 0, std::unexpected { TrapCode::IntegerDivideByZero } },
            { Operation::I32RemU, Type::I32, 7, 0, std::unexpected { TrapCode::IntegerDivideByZero } },
            { Operation::I64DivS,
              Type::I64,
              0x8000'0000'0000'0000,
              0xFFFF'FFFF'FFFF'FFFF,
              std::unexpected { TrapCode::IntegerOverflow } },
            { Operation::I64RemS, Type::I64, 0x8000'0000'0000'0000, 0xFFFF'FFFF'FFFF'FFFF, uint64_t { 0 } },
            { Operation::I64DivS, Type::I64, 1, 0, std::unexpected { TrapCode::IntegerDivideByZero } },
            { Operation::I32DivS, Type::I32, 0xFFFF'FFF9, 2, uint64_t { 0xFFFF'FFFD } },
            { Operation::I32RemS, Type::I32, 0xFFFF'FFF9, 2, uint64_t { 0xFFFF'FFFF } },
            { Operation::I32Shl, Type::I32, 1, 33, uint64_t { 2 } },
            { Operation::I32ShrS, Type::I32, 0xFFFF'FFF8, 33, uint64_t { 0xFFFF'FFFC } },
            { Operation::I32ShrU, Type::I32, 0xFFFF'FFF8, 32, uint64_t { 0xFFFF'FFF8 } },
            { Operation::I32Rotl, Type::I32, 0x8000'0001, 33, uint64_t { 3 } },
            { Operation::I64Shl, Type::I64, 1, 65, uint64_t { 2 } },
            { Operation::I64ShrS, Type::I64, 0xFFFF'FFFF'FFFF'FFF8, 65, uint64_t { 0xFFFF'FFFF'FFFF'FFFC } },
            { Operation::I64Rotr, Type::I64, 1, 64, uint64_t { 1 } },
        });
        for (auto const& sample: samples)
        {
            INFO(intermediate::nameOf(sample.operation) << "(" << sample.lhs << ", " << sample.rhs << ")");
            auto const row = foldingRowOf(sample.operation);
            REQUIRE(row.has_value());
            auto const result =
                intermediate::fixedTypeOf(intermediate::operationRow(sample.operation).result);
            CHECK(row->kernel(std::array { sample.lhs, sample.rhs }, result.value_or(sample.type))
                  == sample.expected);
        }
        // Unary traps: a NaN before an out-of-range value, as the reference checks.
        auto const truncate = foldingRowOf(Operation::I32TruncF32S);
        REQUIRE(truncate.has_value());
        CHECK(truncate->kernel(std::array { uint64_t { 0x7FC0'0000 } }, Type::I32)
              == std::unexpected { TrapCode::InvalidConversionToInteger });
        CHECK(truncate->kernel(std::array { uint64_t { 0x4F00'0000 } }, Type::I32)
              == std::unexpected { TrapCode::IntegerOverflow });
        CHECK(truncate->kernel(std::array { uint64_t { 0xCF00'0000 } }, Type::I32)
              == uint64_t { 0x8000'0000 });
    }
    SECTION("canonicalizeNan folds a NaN to the canonical NaN of its type, and keeps any other value")
    {
        auto const row = foldingRowOf(Operation::CanonicalizeNan);
        REQUIRE(row.has_value());
        CHECK(row->kernel(std::array { uint64_t { 0xFFA0'0001 } }, Type::F32) == uint64_t { 0x7FC0'0000 });
        CHECK(row->kernel(std::array { uint64_t { 0xFF80'0000 } }, Type::F32) == uint64_t { 0xFF80'0000 });
        CHECK(row->kernel(std::array { uint64_t { 0xFFF0'0000'0000'0001 } }, Type::F64)
              == uint64_t { 0x7FF8'0000'0000'0000 });
        CHECK(row->kernel(std::array { uint64_t { 0x8000'0000'0000'0000 } }, Type::F64)
              == uint64_t { 0x8000'0000'0000'0000 });
        // Translated in Deterministic mode, f32.div(0, 0) and its canonicalization fold to one constant.
        auto const translated = translateOf(
            oneFunction(
                FuncType { .params = {}, .results = { isoline::binary::F32 } },
                {},
                Bytes { 0x43, 0, 0, 0, 0, 0x43, 0, 0, 0, 0, 0x95 }), // f32.const 0; f32.const 0; f32.div
            Options { .mode = ExecutionMode::Deterministic });
        auto& function = *translated->function;
        REQUIRE(countOf(function, Operation::CanonicalizeNan) == 1);
        Passes {}.run(function);
        CHECK(countOf(function, Operation::CanonicalizeNan) == 0);
        CHECK(countOf(function, Operation::F32Div) == 0);
        auto const constants = listed(function, Operation::F32Const);
        REQUIRE(constants.size() == 1);
        CHECK(function.immediates(function.instruction(constants[0]).immediates)[0] == 0x7FC0'0000);
    }
    SECTION("in a loop, i32.div_s(1, 0) folds to a trap right after the store of the polled countdown")
    {
        // (loop (br_if 0 (local.get 0)) (drop (i32.div_s (i32.const 1) (i32.const 0)))): the division runs on
        // a path a back edge's poll reaches, so the countdown it traps with is stored first; folding keeps
        // that store, which verify() checks (CountdownNotStored), right before the trap.
        auto const translated = translateOf(oneFunction(FuncType { .params = { WasmI32 }, .results = {} },
                                                        {},
                                                        code::loop(code::code({ code::localGet(0),
                                                                                code::brIf(0),
                                                                                code::i32Const(1),
                                                                                code::i32Const(0),
                                                                                Bytes { 0x6D },
                                                                                code::drop() }))));
        auto& function = *translated->function;
        Passes {}.run(function);
        REQUIRE(countOf(function, Operation::Trap) == 1);
        auto const trap = listed(function, Operation::Trap)[0];
        CHECK(function.immediates(function.instruction(trap).immediates)[0]
              == std::to_underlying(TrapCode::IntegerDivideByZero));
        auto const holder = std::ranges::find_if(
            std::views::iota(uint32_t { 0 }, function.blockCount()), [&](uint32_t block) {
                return std::ranges::contains(function.block(BlockId { block }).instructions, trap);
            });
        auto const& instructions = function.block(BlockId { *holder }).instructions;
        REQUIRE(instructions.size() >= 2);
        CHECK(function.instruction(instructions[instructions.size() - 2]).operation
              == Operation::StoreContext);
        CHECK(pollsAt(function, PollSite::LoopBackEdge) == 1);
    }
    SECTION("i32.div_s(1, 0) translated folds to a trap of IntegerDivideByZero")
    {
        auto const translated =
            translateOf(oneFunction(FuncType { .params = {}, .results = { WasmI32 } },
                                    {},
                                    code::code({ code::i32Const(1), code::i32Const(0), Bytes { 0x6D } })));
        auto& function = *translated->function;
        Passes {}.run(function);
        CHECK(countOf(function, Operation::I32DivS) == 0);
        CHECK(countOf(function, Operation::TrapIf) == 0);
        REQUIRE(countOf(function, Operation::Trap) == 1);
        auto const& trap = function.instruction(listed(function, Operation::Trap)[0]);
        CHECK(function.immediates(trap.immediates)[0] == std::to_underlying(TrapCode::IntegerDivideByZero));
        CHECK(countOf(function, Operation::Return) == 0);
    }
}

TEST_CASE("Optimization.foldsConstantBranches")
{
    auto passes = Passes {};
    SECTION("a branch on a constant jumps to the target it takes, with that target's arguments")
    {
        for (auto const condition: { uint64_t { 0 }, uint64_t { 1 }, uint64_t { 0xFFFF'FFFF } })
        {
            INFO("condition " << condition);
            auto fixture = Fixture {};
            auto& builder = fixture.builder;
            auto const parameter = builder.addParameter(fixture.entry, Type::I32);
            auto const ifNonZero = builder.createBlock();
            auto const ifZero = builder.createBlock();
            auto const nonZeroValue = builder.addParameter(ifNonZero, Type::I32);
            auto const zeroValue = builder.addParameter(ifZero, Type::I32);
            auto const seven = fixture.constant(Type::I32, 7);
            builder.branch(fixture.constant(Type::I32, condition),
                           ifNonZero,
                           std::array { seven },
                           ifZero,
                           std::array { parameter });
            builder.switchTo(ifNonZero);
            builder.ret(std::array { nonZeroValue });
            builder.switchTo(ifZero);
            builder.ret(std::array { zeroValue });
            passes.run(fixture.function, rowOf(Pass::FoldConstants));
            auto const& function = fixture.function;
            CHECK(countOf(function, Operation::Branch) == 0);
            CHECK(function.blockCount() == 2);
            REQUIRE(countOf(function, Operation::Jump) == 1);
            auto const& jump = function.instruction(listed(function, Operation::Jump)[0]);
            REQUIRE(jump.operands.count == 1);
            auto const passed = function.operands(jump.operands)[0];
            auto const entryParameter = function.operands(function.block(BlockId { 0 }).parameters)[0];
            CHECK((passed == entryParameter) == (condition == 0));
        }
    }
    SECTION("a switch on a constant takes its case, or the fallback past the cases")
    {
        for (auto const index: { uint64_t { 0 }, uint64_t { 2 }, uint64_t { 3 }, uint64_t { 0xFFFF'FFFF } })
        {
            INFO("index " << index);
            auto fixture = Fixture {};
            auto& builder = fixture.builder;
            auto targets = std::vector<BlockId> {};
            for ([[maybe_unused]] auto const target: std::views::iota(0, 4))
                targets.push_back(builder.createBlock());
            auto arguments = std::vector<std::array<ValueId, 1>> {};
            for (auto const target: targets)
            {
                std::ignore = builder.addParameter(target, Type::I32);
                arguments.push_back(
                    std::array { fixture.constant(Type::I32, uint64_t { std::to_underlying(target) } * 10) });
            }
            auto const cases = std::to_array<BranchTo>(
                { { targets[0], arguments[0] }, { targets[1], arguments[1] }, { targets[2], arguments[2] } });
            builder.switchOn(
                fixture.constant(Type::I32, index), cases, BranchTo { targets[3], arguments[3] });
            for (auto const target: targets)
            {
                builder.switchTo(target);
                builder.ret(fixture.function.operands(fixture.function.block(target).parameters));
            }
            passes.run(fixture.function, rowOf(Pass::FoldConstants));
            auto const& function = fixture.function;
            CHECK(countOf(function, Operation::Switch) == 0);
            CHECK(function.blockCount() == 2);
            REQUIRE(countOf(function, Operation::Jump) == 1);
            auto const& jump = function.instruction(listed(function, Operation::Jump)[0]);
            auto const taken = index < 3 ? index + 1 : 4;
            auto const passed = function.operands(jump.operands)[0];
            auto const constants = listed(function, Operation::I32Const);
            auto const& defining =
                function.instruction(*std::ranges::find_if(constants, [&](uint32_t constant) {
                    return function.operands(function.instruction(constant).results)[0] == passed;
                }));
            CHECK(function.immediates(defining.immediates)[0] == taken * 10);
        }
    }
    SECTION("a trapIf on zero goes, on another constant it ends the block with its trap")
    {
        auto fixture = Fixture {};
        auto& builder = fixture.builder;
        auto const never = fixture.constant(Type::I32, 0);
        std::ignore =
            builder.emit(Operation::TrapIf, {}, std::array { never }, std::array { uint64_t { 3 } });
        auto const always = fixture.constant(Type::I32, 5);
        std::ignore = builder.emit(Operation::TrapIf,
                                   {},
                                   std::array { always },
                                   std::array { uint64_t { std::to_underlying(TrapCode::Unreachable) } });
        auto const after = builder.createBlock();
        builder.jump(after, {});
        builder.switchTo(after);
        builder.ret({});
        passes.run(fixture.function, rowOf(Pass::FoldConstants));
        auto const& function = fixture.function;
        CHECK(countOf(function, Operation::TrapIf) == 0);
        CHECK(function.blockCount() == 1);
        REQUIRE(countOf(function, Operation::Trap) == 1);
        auto const& trap = function.instruction(listed(function, Operation::Trap)[0]);
        CHECK(function.immediates(trap.immediates)[0] == std::to_underlying(TrapCode::Unreachable));
    }
    SECTION("a trapIfSubject on a non-zero constant stays: a trap terminator names no subject")
    {
        auto fixture = Fixture {};
        auto const condition = fixture.constant(Type::I32, 1);
        auto const subject = fixture.constant(Type::I32, 9);
        std::ignore = fixture.builder.emit(
            Operation::TrapIfSubject,
            {},
            std::array { condition, subject },
            std::array { uint64_t { std::to_underlying(TrapCode::UninitializedElement) } });
        fixture.builder.ret({});
        passes.run(fixture.function, rowOf(Pass::FoldConstants));
        CHECK(countOf(fixture.function, Operation::TrapIfSubject) == 1);
    }
    SECTION("translated: br_if, if and br_table on constants leave no conditional branch")
    {
        // block (br_if 0 (i32.const 1)) end; if (i32.const 0) unreachable end; block block (br_table 0 1
        // (i32.const 1))
        auto const translated = translateOf(
            oneFunction(FuncType {},
                        {},
                        code::code({ code::block(code::code({ code::i32Const(1), code::brIf(0) })),
                                     code::i32Const(0),
                                     Bytes { 0x04, 0x40 },
                                     code::unreachable(),
                                     Bytes { 0x0B },
                                     code::block(code::block(code::code(
                                         { code::i32Const(1), Bytes { 0x0E, 0x01, 0x00, 0x01 } }))) })));
        auto& function = *translated->function;
        REQUIRE(countOf(function, Operation::Branch) + countOf(function, Operation::Switch) >= 3);
        passes.run(function);
        CHECK(countOf(function, Operation::Branch) == 0);
        CHECK(countOf(function, Operation::Switch) == 0);
        CHECK(countOf(function, Operation::Trap) == 0);
        CHECK(countOf(function, Operation::Return) == 1);
    }
}

namespace
{

/// How many listed polls @p function has at @p site.
std::size_t pollsAt(Function const& function, PollSite site)
{
    auto count = std::size_t { 0 };
    for (auto const operation: { Operation::Poll, Operation::PollStored })
        for (auto const index: listed(function, operation))
            if (function.immediates(function.instruction(index).immediates)[0] == std::to_underlying(site))
                ++count;
    return count;
}

/// Whether @p function has a cycle: some block reaches itself.
bool hasCycle(Function const& function)
{
    // A depth-first walk that meets a block still on its path.
    enum class Mark : uint8_t
    {
        Unvisited,
        OnPath,
        Done,
    };
    auto marks = std::vector<Mark>(function.blockCount(), Mark::Unvisited);
    auto stack = std::vector<std::pair<BlockId, std::size_t>> { { BlockId { 0 }, 0 } };
    marks[0] = Mark::OnPath;
    while (!stack.empty())
    {
        auto& [block, next] = stack.back();
        auto const successors = function.successors(block);
        if (next == successors.size())
        {
            marks[std::to_underlying(block)] = Mark::Done;
            stack.pop_back();
            continue;
        }
        auto const successor = successors[next++];
        if (marks[std::to_underlying(successor)] == Mark::OnPath)
            return true;
        if (marks[std::to_underlying(successor)] == Mark::Unvisited)
        {
            marks[std::to_underlying(successor)] = Mark::OnPath;
            stack.emplace_back(successor, 0);
        }
    }
    return false;
}

} // namespace

TEST_CASE("Optimization.pollsSurviveEveryPass")
{
    // Review Focus 4: an empty loop, a loop of dead pure code and a br_table loop keep one LoopBackEdge poll
    // per edge into their loop header (a br_table's labels share one), and the FunctionEntry pollStored,
    // after every pass; the loop stays a loop.
    struct Shape
    {
        std::string_view name;
        Bytes body;
        std::size_t backEdges;
    };
    auto const shapes = std::to_array<Shape>({
        { "(loop (br 0))", code::loop(code::br(0)), 1 },
        { "a loop of dead pure code",
          code::loop(code::code({ code::i32Const(1),
                                  code::i32Const(2),
                                  Bytes { 0x6A },
                                  code::drop(),
                                  code::localGet(0),
                                  code::brIf(0) })),
          1 },
        { "a br_table loop",
          code::loop(code::code({ code::localGet(0), Bytes { 0x0E, 0x02, 0x00, 0x00, 0x00 } })),
          1 },
    });
    for (auto const mode: { ExecutionMode::Production, ExecutionMode::Deterministic })
        for (auto const& shape: shapes)
        {
            INFO(shape.name << (mode == ExecutionMode::Deterministic ? ", deterministic" : ""));
            auto const translated =
                translateOf(oneFunction(FuncType { .params = { WasmI32 }, .results = {} }, {}, shape.body),
                            Options { .mode = mode });
            auto& function = *translated->function;
            REQUIRE(pollsAt(function, PollSite::LoopBackEdge) == shape.backEdges);
            REQUIRE(pollsAt(function, PollSite::FunctionEntry) == 1);
            // After each pass alone, and after the whole table.
            for (auto const& row: PassTable)
            {
                INFO("after " << row.name);
                Passes {}.run(function, std::span { &row, 1 });
                CHECK(pollsAt(function, PollSite::LoopBackEdge) == shape.backEdges);
                CHECK(pollsAt(function, PollSite::FunctionEntry) == 1);
                CHECK(countOf(function, Operation::PollStored) == 1);
                CHECK(hasCycle(function));
            }
            CHECK(countOf(function, Operation::I32Add) == 0);
        }
}

TEST_CASE("Optimization.dceKeepsTrapsStoresAndContextStores")
{
    // Unused results: a division (its trapIfs), a store, a global.set, a call whose result goes unused, the
    // countdown stored before it, and a load (ReadsMemory) stay; unused pure arithmetic and an unused
    // memory.size (ReadsContext) go.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { WasmI32, WasmI32 }, .results = {} });
    builder.type(FuncType { .params = {}, .results = { WasmI32 } });
    builder.memory(isoline::binary::Limits { .min = 1, .max = 1 });
    builder.global(
        isoline::binary::GlobalType { .type = WasmI32, .mutability = isoline::binary::Mutability::Var },
        { 0x41, 0x00 });
    builder.function(0,
                     {},
                     code::code({
                         code::localGet(0),
                         code::localGet(1),
                         Bytes { 0x6D },
                         code::drop(), // i32.div_s
                         code::localGet(0),
                         code::localGet(1),
                         Bytes { 0x6A },
                         code::drop(), // i32.add
                         code::localGet(0),
                         code::localGet(1),
                         code::memoryAccess(0x36, 2, 0), // i32.store
                         code::localGet(0),
                         Bytes { 0x24, 0x00 }, // global.set 0
                         code::call(1),
                         code::drop(), // call 1
                         code::localGet(0),
                         code::memoryAccess(0x28, 2, 0),
                         code::drop(), // i32.load
                         Bytes { 0x3F, 0x00 },
                         code::drop(), // memory.size
                     }));
    builder.function(1, {}, code::i32Const(1));
    auto const translated = translateOf(builder);
    auto& function = *translated->function;
    auto const before = std::map<Operation, std::size_t> {
        { Operation::TrapIf, countOf(function, Operation::TrapIf) },
        { Operation::I32DivS, countOf(function, Operation::I32DivS) },
        { Operation::Store, countOf(function, Operation::Store) },
        { Operation::StoreGlobal, countOf(function, Operation::StoreGlobal) },
        { Operation::Call, countOf(function, Operation::Call) },
        { Operation::StoreContext, countOf(function, Operation::StoreContext) },
        { Operation::Load, countOf(function, Operation::Load) },
        { Operation::CheckMemory, countOf(function, Operation::CheckMemory) },
        { Operation::EnterFrame, countOf(function, Operation::EnterFrame) },
        { Operation::LeaveFrame, countOf(function, Operation::LeaveFrame) },
    };
    REQUIRE(countOf(function, Operation::I32Add) == 1);
    for (auto const& [operation, count]: before)
    {
        INFO(intermediate::nameOf(operation));
        REQUIRE(count >= 1);
    }
    Passes {}.run(function, rowOf(Pass::EliminateDeadCode));
    for (auto const& [operation, count]: before)
    {
        INFO(intermediate::nameOf(operation));
        CHECK(countOf(function, operation) == count);
    }
    CHECK(countOf(function, Operation::I32Add) == 0);
    // memory.size reads memory 0's bound, a context value: its shift goes, nothing else uses it.
    CHECK(countOf(function, Operation::I32WrapI64) == 0);
}

TEST_CASE("Optimization.dceRemovesValuesThatOnlyFeedThemselves")
{
    // A loop-carried parameter used only to compute its next value, and pure code around it: all go, the
    // loop's polls and control stay.
    auto fixture = Fixture {};
    auto& builder = fixture.builder;
    auto const condition = builder.addParameter(fixture.entry, Type::I32);
    auto const loop = builder.createBlock();
    auto const exit = builder.createBlock();
    auto const carried = builder.addParameter(loop, Type::I64);
    builder.jump(loop, std::array { fixture.constant(Type::I64, 0) });
    builder.switchTo(loop);
    auto const next =
        fixture.emit(Operation::I64Add, Type::I64, std::array { carried, fixture.constant(Type::I64, 1) });
    builder.branch(condition, loop, std::array { next }, exit, {});
    builder.switchTo(exit);
    builder.ret({});
    Passes {}.run(fixture.function, rowOf(Pass::EliminateDeadCode));
    auto const& function = fixture.function;
    CHECK(function.block(BlockId { 1 }).parameters.count == 0);
    CHECK(countOf(function, Operation::I64Add) == 0);
    CHECK(countOf(function, Operation::Branch) == 1);
    CHECK(function.block(BlockId { 0 }).parameters.count == 1);
}

TEST_CASE("Optimization.removesTrivialParameters")
{
    SECTION("a group of parameters passing only each other and one value collapses to it")
    {
        // h1(p) <- entry(c), h2(q); h2(q) <- h1(p), h2(q): {p, q} take only c from outside.
        auto fixture = Fixture {};
        auto& builder = fixture.builder;
        auto const condition = builder.addParameter(fixture.entry, Type::I32);
        auto const first = builder.createBlock();
        auto const second = builder.createBlock();
        auto const exit = builder.createBlock();
        auto const p = builder.addParameter(first, Type::I32);
        auto const q = builder.addParameter(second, Type::I32);
        auto const c = fixture.constant(Type::I32, 42);
        builder.jump(first, std::array { c });
        builder.switchTo(first);
        builder.branch(condition, second, std::array { p }, exit, {});
        builder.switchTo(second);
        auto const passed = std::array { q };
        auto const back = std::to_array<BranchTo>({ { first, passed } });
        builder.switchOn(condition, back, BranchTo { second, passed });
        builder.switchTo(exit);
        builder.ret(std::array { p });
        Passes {}.run(fixture.function, rowOf(Pass::RemoveTrivialParameters));
        auto const& function = fixture.function;
        for (auto const block: std::views::iota(uint32_t { 1 }, function.blockCount()))
            CHECK(function.block(BlockId { block }).parameters.count == 0);
        auto const& returned = function.instruction(listed(function, Operation::Return)[0]);
        auto const& constant = function.instruction(listed(function, Operation::I32Const)[0]);
        CHECK(function.operands(returned.operands)[0] == function.operands(constant.results)[0]);
    }
    SECTION("a parameter passed two values stays")
    {
        auto fixture = Fixture {};
        auto& builder = fixture.builder;
        auto const condition = builder.addParameter(fixture.entry, Type::I32);
        auto const merge = builder.createBlock();
        auto const value = builder.addParameter(merge, Type::I32);
        builder.branch(condition,
                       merge,
                       std::array { fixture.constant(Type::I32, 1) },
                       merge,
                       std::array { fixture.constant(Type::I32, 2) });
        builder.switchTo(merge);
        builder.ret(std::array { value });
        Passes {}.run(fixture.function, rowOf(Pass::RemoveTrivialParameters));
        CHECK(fixture.function.block(BlockId { 1 }).parameters.count == 1);
    }
}

TEST_CASE("Optimization.simplifiesControlFlow")
{
    auto passes = Passes {};
    SECTION("unreachable blocks go, a chain of jumps merges into one block")
    {
        auto fixture = Fixture {};
        auto& builder = fixture.builder;
        auto const input = builder.addParameter(fixture.entry, Type::I32);
        auto const dead = builder.createBlock();
        auto chain = std::vector<BlockId> {};
        for ([[maybe_unused]] auto const link: std::views::iota(0, 4))
            chain.push_back(builder.createBlock());
        auto value = input;
        builder.jump(chain[0], {});
        for (auto const [position, link]: core::views::enumerate(chain))
        {
            builder.switchTo(link);
            value = fixture.emit(Operation::I32Add, Type::I32, std::array { value, input });
            if (position + 1 < chain.size())
                builder.jump(chain[position + 1], {});
            else
                builder.ret(std::array { value });
        }
        builder.switchTo(dead);
        builder.jump(chain[2], {});
        passes.run(fixture.function, rowOf(Pass::SimplifyControlFlow));
        CHECK(fixture.function.blockCount() == 1);
        CHECK(countOf(fixture.function, Operation::I32Add) == 4);
        CHECK(countOf(fixture.function, Operation::Jump) == 0);
    }
    SECTION("edges into forwarding blocks go where they forward, with the arguments they would pass")
    {
        // A switch whose targets all forward through f1(a, b) -> f2(b) -> exit(b); exit returns its
        // parameter.
        auto fixture = Fixture {};
        auto& builder = fixture.builder;
        auto const input = builder.addParameter(fixture.entry, Type::I32);
        auto const forwardFirst = builder.createBlock();
        auto const forwardSecond = builder.createBlock();
        auto const exit = builder.createBlock();
        std::ignore = builder.addParameter(forwardFirst, Type::I32);
        auto const b = builder.addParameter(forwardFirst, Type::I32);
        auto const relayed = builder.addParameter(forwardSecond, Type::I32);
        auto const result = builder.addParameter(exit, Type::I32);
        auto const one = fixture.constant(Type::I32, 1);
        auto const two = fixture.constant(Type::I32, 2);
        auto const firstCase = std::array { one, two };
        auto const secondCase = std::array { two, input };
        auto const fallback = std::array { input, one };
        auto const cases =
            std::to_array<BranchTo>({ { forwardFirst, firstCase }, { forwardFirst, secondCase } });
        builder.switchOn(input, cases, BranchTo { forwardFirst, fallback });
        builder.switchTo(forwardFirst);
        builder.jump(forwardSecond, std::array { b });
        builder.switchTo(forwardSecond);
        builder.jump(exit, std::array { relayed });
        builder.switchTo(exit);
        builder.ret(std::array { result });
        passes.run(fixture.function, rowOf(Pass::SimplifyControlFlow));
        auto const& function = fixture.function;
        CHECK(function.blockCount() == 2);
        REQUIRE(countOf(function, Operation::Switch) == 1);
        auto const& switched = function.instruction(listed(function, Operation::Switch)[0]);
        auto const targets = function.targets(switched);
        REQUIRE(targets.size() == 3);
        // Fallback first: it passed (input, one), so b = one; then (one, two) and (two, input). The function
        // is compacted, so the values are told by what defines them.
        auto const expected = std::to_array<std::string_view>({ "1", "2", "input" });
        auto const entryParameter = function.operands(function.block(BlockId { 0 }).parameters)[0];
        auto const constants = listed(function, Operation::I32Const);
        auto const nameOf = [&](ValueId value) -> std::string {
            if (value == entryParameter)
                return "input";
            for (auto const index: constants)
                if (function.operands(function.instruction(index).results)[0] == value)
                    return std::to_string(function.immediates(function.instruction(index).immediates)[0]);
            return "?";
        };
        for (auto const [position, target]: core::views::enumerate(targets))
        {
            CHECK(target.block == BlockId { 1 });
            REQUIRE(target.arguments.count == 1);
            CHECK(nameOf(function.operands(target.arguments)[0]) == expected[position]);
        }
    }
    SECTION("a block holding a poll is not threaded, nor a block whose parameter another block uses")
    {
        auto fixture = Fixture {};
        auto& builder = fixture.builder;
        auto const input = builder.addParameter(fixture.entry, Type::I32);
        auto const polling = builder.createBlock();
        auto const relaying = builder.createBlock();
        auto const user = builder.createBlock();
        auto const exit = builder.createBlock();
        auto const countdown = builder.addParameter(polling, Type::I64);
        auto const shared = builder.addParameter(relaying, Type::I32);
        builder.branch(
            input, polling, std::array { fixture.constant(Type::I64, 9) }, relaying, std::array { input });
        builder.switchTo(polling);
        std::ignore = builder.emit(Operation::Poll,
                                   std::array { Type::I64 },
                                   std::array { countdown },
                                   std::array { uint64_t { std::to_underlying(PollSite::LoopBackEdge) } });
        builder.jump(exit, {});
        builder.switchTo(relaying);
        builder.jump(user, {});
        builder.switchTo(user);
        builder.ret(std::array { shared });
        builder.switchTo(exit);
        builder.ret(std::array { input });
        passes.run(fixture.function, rowOf(Pass::SimplifyControlFlow));
        auto const& function = fixture.function;
        CHECK(countOf(function, Operation::Poll) == 1);
        // The branch still goes to the polling block and to the block whose parameter `user` reads (merged
        // with user, its only successor).
        REQUIRE(countOf(function, Operation::Branch) == 1);
        auto const targets = function.targets(function.instruction(listed(function, Operation::Branch)[0]));
        for (auto const& target: targets)
            CHECK(function.block(target.block).parameters.count == 1);
    }
    SECTION("a block that would pass more values than it takes is not threaded")
    {
        // widen(v): jump exit(v, v): threading an edge into it would grow the edge, so the branch keeps going
        // to it; the jump chain after it still merges.
        auto fixture = Fixture {};
        auto& builder = fixture.builder;
        auto const input = builder.addParameter(fixture.entry, Type::I32);
        auto const widen = builder.createBlock();
        auto const other = builder.createBlock();
        auto const exit = builder.createBlock();
        auto const value = builder.addParameter(widen, Type::I32);
        auto const first = builder.addParameter(exit, Type::I32);
        std::ignore = builder.addParameter(exit, Type::I32);
        builder.branch(input, widen, std::array { input }, other, {});
        builder.switchTo(widen);
        builder.jump(exit, std::array { value, value });
        builder.switchTo(other);
        builder.jump(exit, std::array { input, input });
        builder.switchTo(exit);
        builder.ret(std::array { first });
        passes.run(fixture.function, rowOf(Pass::SimplifyControlFlow));
        auto const& function = fixture.function;
        auto const targets = function.targets(function.instruction(listed(function, Operation::Branch)[0]));
        REQUIRE(targets.size() == 2);
        CHECK(function.block(targets[0].block).parameters.count == 1);
        CHECK(targets[0].arguments.count == 1);
    }
}

namespace
{

/// Where the values of a function come from, told apart by that rather than by their IDs, which passes and
/// compaction renumber: a constant's bits (a foldable operation on constants, evaluated through FoldingTable,
/// counts as its result), a parameter of the entry by position, a block parameter that every edge passes one
/// origin (itself aside) as that origin, and anything else as its operation over its operands' origins.
class Origins
{
  public:
    explicit Origins(Function const& function):
        _function { function },
        _definitions { optimization::definitionsOf(function, _memory) },
        _places { optimization::parameterPlacesOf(function, _memory) },
        _incoming { optimization::incomingEdgesOf(function, _memory) }
    {
    }

    /// The origin of @p value.
    std::string of(ValueId value)
    {
        auto const key = std::to_underlying(value);
        if (auto const known = _known.find(key); known != _known.end())
            return known->second;
        if (!_visiting.insert(key).second)
            return {}; // a parameter passed itself around a loop
        auto origin = compute(value);
        _visiting.erase(key);
        _known.emplace(key, origin);
        return origin;
    }

  private:
    std::string compute(ValueId value)
    {
        auto const definition = _definitions[std::to_underlying(value)];
        if (definition != isoline::compiler::intermediate::Unnumbered)
        {
            auto const& instruction = _function.instruction(definition);
            if (auto const bits = optimization::constantOf(_function, _definitions, value))
                return "c" + std::to_string(*bits);
            auto operands = std::vector<std::string> {};
            auto bits = std::vector<uint64_t> {};
            for (auto const operand: _function.operands(instruction.operands))
            {
                operands.push_back(of(operand));
                if (operands.back().starts_with('c'))
                    bits.push_back(std::stoull(operands.back().substr(1)));
            }
            if (auto const row = foldingRowOf(instruction.operation); row && bits.size() == operands.size())
                if (auto const folded = row->kernel(bits, instruction.type))
                    return "c" + std::to_string(*folded);
            auto text = std::string { intermediate::nameOf(instruction.operation) } + "(";
            for (auto const& operand: operands)
                text += operand + ",";
            return text + ")";
        }
        auto const block = _places.block[std::to_underlying(value)];
        auto const position = _places.position[std::to_underlying(value)];
        if (block == 0)
            return "p" + std::to_string(position);
        auto passed = std::set<std::string> {};
        for (auto const& edge: _incoming.into(BlockId { block }))
        {
            auto const argument = _function.operands(isoline::compiler::intermediate::OperandRange {
                .begin = edge.arguments + position, .count = 1 })[0];
            if (auto origin = of(argument); !origin.empty())
                passed.insert(std::move(origin));
        }
        if (passed.size() == 1)
            return *passed.begin();
        auto text = std::string { "merge(" };
        for (auto const& origin: passed)
            text += origin + ",";
        return text + ")";
    }

    std::pmr::unsynchronized_pool_resource _memory {};
    Function const& _function;
    std::pmr::vector<uint32_t> _definitions;
    optimization::ParameterPlaces _places;
    optimization::IncomingEdges _incoming;
    std::map<uint32_t, std::string> _known {};
    std::set<uint32_t> _visiting {};
};

} // namespace

namespace
{

/// The guards and accesses of a function, in block order: each operation, its immediates, and for a guard the
/// origins of its operands (Origins).
using GuardSignature = std::vector<std::tuple<Operation, std::vector<uint64_t>, std::vector<std::string>>>;

/// Whether folding ran before EliminateBoundsChecks: until it does, an index computed from constants is no
/// constant, and rule (a) proves nothing.
enum class IndexFolded : uint8_t
{
    No,  ///< The pass ran alone.
    Yes, ///< FoldConstants ran first.
};

/// What EliminateBoundsChecks does to passesKeepEveryGuard's memory function, as its signature shows it: of
/// the three checks in block order (the first, the second after a call, the narrow third), each whose
/// constant index ends within @p minimums' first, once @p folded, becomes inBoundsAddress of its base; else
/// the third, which the second dominates with nothing between, shares the second's address and goes, unless
/// the second's static end passes 2^64 (translation never emits that check, and the pass takes no proof from
/// it). Counts each.
void eliminateExpectedChecks(GuardSignature& signature,
                             PassStatistics& counts,
                             Type indexType,
                             uint64_t bits,
                             std::span<uint64_t const> minimums,
                             IndexFolded folded)
{
    auto position = std::size_t { 0 };
    auto checks = 0;
    auto secondKept = false;
    while (position < signature.size())
    {
        auto& [operation, immediates, operands] = signature[position];
        if (operation != Operation::CheckMemory)
        {
            ++position;
            continue;
        }
        auto const ordinal = checks++;
        auto const effective =
            minimums.empty() || folded == IndexFolded::No
                ? std::nullopt
                : intermediate::guardMemoryAccess(indexType, bits, immediates[1], immediates[2], minimums[0]);
        if (effective)
        {
            operation = Operation::InBoundsAddress;
            immediates = { 0, *effective, immediates[2] };
            operands = { operands[1] };
            counts.add(PassStatistic::ChecksProvedByMinimum);
        }
        else if (ordinal == 1)
            secondKept =
                !intermediate::staticEndOverflows(immediates[1], immediates[2]); // proves nothing else
        else if (ordinal == 2 && secondKept)
        {
            signature.erase(signature.begin() + static_cast<std::ptrdiff_t>(position));
            counts.add(PassStatistic::ChecksSharingDominatingAddress);
            continue;
        }
        ++position;
    }
}

} // namespace

TEST_CASE("Optimization.passesKeepEveryGuard")
{
    // The guards' lowering contract (intermediate/Guards.hpp) on every edge row: each pass, then the whole
    // table, keeps every checkMemory, checkTable and checkDispatch with its immediates, and its access, and
    // the function verifies (the guard still covers the access). A constant index is foldable around them; a
    // call between two equal checks must keep both. EliminateBoundsChecks alone may change a checkMemory,
    // and exactly as Guards.hpp's rules allow, each counted: the narrower check a wider one dominates with
    // no call between shares its address, and, given the memory's minimum, a check whose constant index ends
    // within it becomes inBoundsAddress. Every other pass keeps them all, and counts nothing.
    using isoline::compiler::intermediate::testing::DispatchGuardEdges;
    using isoline::compiler::intermediate::testing::MemoryGuardEdges;
    using isoline::compiler::intermediate::testing::TableGuardEdges;
    auto const guards = std::to_array({ Operation::CheckMemory,
                                        Operation::GuardAddress,
                                        Operation::InBoundsAddress,
                                        Operation::CheckTable,
                                        Operation::CheckDispatch });
    auto const accesses = std::to_array(
        { Operation::Load, Operation::Store, Operation::LoadTableElement, Operation::LoadDispatchType });
    /// The guards and accesses of @p function with their immediates, the guards with the origins of their
    /// index, base and bound (Origins), in block order.
    auto const signatureOf = [&](Function const& function) {
        auto origins = Origins { function };
        auto found = GuardSignature {};
        for (auto const block: std::views::iota(uint32_t { 0 }, function.blockCount()))
            for (auto const index: function.block(BlockId { block }).instructions)
            {
                auto const& instruction = function.instruction(index);
                auto const isGuard = std::ranges::contains(guards, instruction.operation);
                if (!isGuard && !std::ranges::contains(accesses, instruction.operation))
                    continue;
                auto const immediates = function.immediates(instruction.immediates);
                auto operands = std::vector<std::string> {};
                if (isGuard)
                    for (auto const operand: function.operands(instruction.operands))
                        operands.push_back(origins.of(operand));
                found.emplace_back(instruction.operation,
                                   std::vector<uint64_t>(immediates.begin(), immediates.end()),
                                   std::move(operands));
            }
        return found;
    };
    using Signature = GuardSignature;
    /// What EliminateBoundsChecks does to a signature: the signature it leaves, and what it counts.
    using Elimination = std::function<void(Signature&, PassStatistics&, IndexFolded)>;
    auto const checkEveryPass =
        [&](
            std::function<void(Fixture&)> const& build,
            std::span<uint64_t const> minimums = {},
            Elimination const& eliminated = [](Signature&, PassStatistics&, IndexFolded) {}) {
            auto const checkRun = [&](std::span<PassRow const> rows) {
                auto fixture = Fixture {};
                build(fixture);
                REQUIRE(verify(fixture.function).has_value());
                auto expected = signatureOf(fixture.function);
                auto expectedCounts = PassStatistics {};
                if (std::ranges::contains(rows, Pass::EliminateBoundsChecks, &PassRow::pass))
                    eliminated(expected,
                               expectedCounts,
                               std::ranges::contains(rows, Pass::FoldConstants, &PassRow::pass)
                                   ? IndexFolded::Yes
                                   : IndexFolded::No);
                auto const counted = Passes {}.run(fixture.function, rows, minimums);
                CHECK(signatureOf(fixture.function) == expected);
                for (auto const& statistic: PassStatisticTable)
                {
                    INFO(statistic.name);
                    CHECK(counted.count(statistic.statistic) == expectedCounts.count(statistic.statistic));
                }
            };
            for (auto const& row: PassTable)
            {
                INFO("pass " << row.name);
                checkRun(std::span { &row, 1 });
            }
            checkRun(PassTable);
        };
    auto const constantIndex = [](Fixture& fixture, Type type, uint64_t bits) {
        // An index computed from constants, which folding turns into one.
        auto const half = fixture.constant(type, bits ^ 0x5A);
        return fixture.emit(type == Type::I32 ? Operation::I32Xor : Operation::I64Xor,
                            type,
                            std::array { half, fixture.constant(type, 0x5A) });
    };
    for (auto const& edge: MemoryGuardEdges)
    {
        INFO("memory edge index " << edge.index << " offset " << edge.offset << " size " << edge.size);
        auto const bits = edge.indexType == Type::I32 ? edge.index & 0xFFFF'FFFF : edge.index;
        auto const minimum = std::array { edge.bound };
        // The checks in block order: the first, the second (after the call), the narrow third. Given the
        // minimum, each whose constant index ends within it becomes inBoundsAddress of its base; else the
        // narrow third, which the second dominates with nothing between, shares the second's address.
        auto const eliminated = [&](std::span<uint64_t const> minimums) {
            return [&, minimums](GuardSignature& signature, PassStatistics& counts, IndexFolded folded) {
                eliminateExpectedChecks(signature, counts, edge.indexType, bits, minimums, folded);
            };
        };
        auto const build = [&](Fixture& fixture) {
            // The base and the bound reach the checks through a merge and a loop header, as context values
            // do: entry(base, bound, c) -> left | right -> merge(b, d) -> header(b, d) ... -> header | done.
            auto& builder = fixture.builder;
            auto const entryBase = builder.emit(
                Operation::LoadContext,
                std::array { Type::Pointer },
                {},
                std::array<uint64_t, 2> {
                    std::to_underlying(isoline::compiler::intermediate::ContextValue::Memory0Base),
                    std::to_underlying(Type::Pointer) })[0];
            auto const entryBound = builder.addParameter(fixture.entry, Type::I64);
            auto const condition = builder.addParameter(fixture.entry, Type::I32);
            auto const left = builder.createBlock();
            auto const right = builder.createBlock();
            auto const merge = builder.createBlock();
            auto const header = builder.createBlock();
            auto const mergedBase = builder.addParameter(merge, Type::Pointer);
            auto const mergedBound = builder.addParameter(merge, Type::I64);
            auto const base = builder.addParameter(header, Type::Pointer);
            auto const bound = builder.addParameter(header, Type::I64);
            auto const entryValues = std::array { entryBase, entryBound };
            auto const mergedValues = std::array { mergedBase, mergedBound };
            auto const headerValues = std::array { base, bound };
            builder.branch(condition, left, {}, right, {});
            for (auto const side: { left, right })
            {
                builder.switchTo(side);
                builder.jump(merge, entryValues);
            }
            builder.switchTo(merge);
            builder.jump(header, mergedValues);
            builder.switchTo(header);
            auto const index = constantIndex(fixture, edge.indexType, bits);
            auto const checkOf = [&](uint64_t size) {
                return builder.emit(Operation::CheckMemory,
                                    std::array { Type::Pointer },
                                    std::array { index, base, bound },
                                    std::array<uint64_t, 3> { 0, edge.offset, size })[0];
            };
            auto const loadType = edge.size == 8 ? Type::I64 : Type::I32;
            auto const first = checkOf(edge.size);
            auto const loaded =
                builder.emit(Operation::Load,
                             std::array { loadType },
                             std::array { first },
                             std::array<uint64_t, 4> { 0, edge.size, 0, std::to_underlying(loadType) })[0];
            std::ignore = loaded;
            // A call (a stub that may grow the memory) between two equal checks: both stay.
            std::ignore = builder.emit(Operation::CallStub, {}, {}, std::array { uint64_t { 0 } });
            std::ignore = builder.emit(
                Operation::LoadContext,
                std::array { Type::I64 },
                {},
                std::array<uint64_t, 2> {
                    std::to_underlying(isoline::compiler::intermediate::ContextValue::StepCountdown),
                    std::to_underlying(Type::I64) });
            auto const second = checkOf(edge.size);
            std::ignore = builder.emit(Operation::Store,
                                       {},
                                       std::array { second, loaded },
                                       std::array<uint64_t, 2> { 0, edge.size });
            // A narrower check of the same index on a branch folding takes: the wider one dominates it, with
            // no call between, so EliminateBoundsChecks lets it share the wider one's address.
            auto const narrow = builder.createBlock();
            auto const exit = builder.createBlock();
            builder.branch(fixture.constant(Type::I32, 1), narrow, {}, exit, {});
            builder.switchTo(narrow);
            auto const third = checkOf(1);
            std::ignore = builder.emit(Operation::Load,
                                       std::array { Type::I32 },
                                       std::array { third },
                                       std::array<uint64_t, 4> { 0, 1, 0, std::to_underlying(Type::I32) });
            builder.jump(exit, {});
            builder.switchTo(exit);
            auto const done = builder.createBlock();
            builder.branch(condition, header, headerValues, done, {});
            builder.switchTo(done);
            builder.ret({});
        };
        checkEveryPass(build, {}, eliminated({}));
        checkEveryPass(build, minimum, eliminated(minimum));
    }
    for (auto const& edge: TableGuardEdges)
    {
        INFO("table edge index " << edge.index << " size " << edge.size);
        checkEveryPass([&](Fixture& fixture) {
            auto const bits = edge.indexType == Type::I32 ? edge.index & 0xFFFF'FFFF : edge.index;
            auto const index = constantIndex(fixture, edge.indexType, bits);
            auto const element = fixture.builder.emit(
                Operation::CheckTable,
                std::array { Type::Pointer },
                std::array { index },
                std::array<uint64_t, 2> { 0, std::to_underlying(TrapCode::OutOfBoundsTableAccess) })[0];
            std::ignore = fixture.builder.emit(Operation::LoadTableElement,
                                               std::array { Type::Ref },
                                               std::array { element },
                                               std::array<uint64_t, 5> { 0, 0, 1, 4, 8 });
            fixture.builder.ret({});
        });
    }
    for (auto const& edge: DispatchGuardEdges)
    {
        INFO("dispatch edge reference " << edge.reference << " rows " << edge.rows);
        checkEveryPass([&](Fixture& fixture) {
            auto const reference = fixture.builder.addParameter(fixture.entry, Type::Ref);
            auto const index = constantIndex(fixture, Type::I32, edge.reference >> 32U);
            auto const row = fixture.builder.emit(
                Operation::CheckDispatch,
                std::array { Type::Pointer },
                std::array { reference, index },
                std::array { uint64_t { std::to_underlying(TrapCode::UndefinedElement) } })[0];
            std::ignore = fixture.builder.emit(
                Operation::LoadDispatchType, std::array { Type::I32 }, std::array { row });
            fixture.builder.ret({});
        });
    }
}

TEST_CASE("Optimization.everyCompiledOpcodeSurvivesThePasses")
{
    // Every instruction the compiler compiles, on constant operands where it takes any (so folding has work:
    // zero divisors trap, branches on constants are taken), in both modes: every pass leaves the function
    // well-formed (verified after each), and every reachable poll stays.
    // Each also inside a loop, after a br_if back to it, where every trap site needs the polled countdown
    // stored (intermediate::verify(): translateOf() checks it before the passes, runPasses() after each).
    using isoline::compiler::translation::testing::Placement;
    auto samples = 0;
    for (auto const placement: { Placement::Body, Placement::InLoop })
        for (auto const mode: { ExecutionMode::Production, ExecutionMode::Deterministic })
            for (auto const& sample: isoline::compiler::translation::testing::everyCompiledOpcode(placement))
            {
                INFO(sample.info->text << (mode == ExecutionMode::Deterministic ? ", deterministic" : "")
                                       << (placement == Placement::InLoop ? ", in a loop" : ""));
                auto const translated = translateOf(sample.module, Options { .mode = mode });
                REQUIRE(translated->function.has_value());
                auto& function = *translated->function;
                auto const entryPolls = pollsAt(function, PollSite::FunctionEntry);
                Passes {}.run(function);
                CHECK(pollsAt(function, PollSite::FunctionEntry) == entryPolls);
                CHECK(countOf(function, Operation::EnterFrame) == 1);
                ++samples;
            }
    CHECK(samples == 4 * 201);
}

TEST_CASE("Optimization.verifiesAfterEveryPass")
{
    // A pass row that leaves a block without its terminator, followed by a healthy one. After each pass, the
    // run is refused naming the broken row; verified only at the end, it is refused naming the end. Either
    // way an error, never an abort, and the function is not compacted.
    auto const breakTerminator = [](Function& function, PassContext&) -> PassOutcome {
        function.remove(*function.terminator(BlockId { 0 }));
        return {};
    };
    auto const rows = std::to_array<PassRow>({
        { .pass = Pass::FoldConstants, .name = "broken", .run = breakTerminator },
        PassTable[0],
    });
    struct Row
    {
        Verification verification;
        std::string_view pass;
    };
    for (auto const& row:
         std::to_array<Row>({ { .verification = Verification::AfterEachPass, .pass = "broken" },
                              { .verification = Verification::AtEnd, .pass = "end" } }))
    {
        INFO(row.pass);
        auto fixture = Fixture {};
        fixture.builder.ret({});
        auto passes = Passes {};
        passes.config = CompileConfig { TargetDescription {}, ExecutionMode::Production };
        auto context = passes.contextFor(fixture.memory, row.verification);
        context.codeIndex = 5;
        auto const ran = runPasses(fixture.function, context, rows);
        REQUIRE_FALSE(ran.has_value());
        CHECK(ran.error()
              == isoline::compiler::CompileError {
                  .code = CompileErrorCode::InternalVerificationFailed,
                  .function = 5,
                  .pass = row.pass,
                  .rule = isoline::compiler::intermediate::VerifierErrorCode::MissingTerminator });
    }
    // Every row of the table keeps a well-formed function well-formed (verified after each).
    auto healthy = Fixture {};
    healthy.builder.ret({});
    Passes {}.run(healthy.function);
}

TEST_CASE("Optimization.cancellationStopsThePasses")
{
    auto passes = Passes {};
    passes.cancellation.cancel();
    auto fixture = Fixture {};
    fixture.builder.ret({});
    auto context = passes.contextFor(fixture.memory);
    auto const ran = runPasses(fixture.function, context);
    REQUIRE_FALSE(ran.has_value());
    CHECK(ran.error().code == CompileErrorCode::Cancelled);
    CHECK(ran.error().function == 0U);
}

namespace
{

/// A function of @p depth nested loops, each header passing its parameter on and taking it back from its
/// latch: every parameter is trivial, and the chain of them is @p depth long.
void nestedLoops(Fixture& fixture, uint32_t depth)
{
    auto& builder = fixture.builder;
    auto const condition = builder.addParameter(fixture.entry, Type::I32);
    auto headers = std::vector<BlockId> {};
    auto latches = std::vector<BlockId> {};
    auto parameters = std::vector<ValueId> {};
    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, depth))
    {
        headers.push_back(builder.createBlock());
        parameters.push_back(builder.addParameter(headers.back(), Type::I32));
    }
    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, depth))
        latches.push_back(builder.createBlock());
    auto const exit = builder.createBlock();
    builder.jump(headers[0], std::array { fixture.constant(Type::I32, 5) });
    for (auto const level: std::views::iota(uint32_t { 0 }, depth))
    {
        builder.switchTo(headers[level]);
        if (level + 1 < depth)
            builder.jump(headers[level + 1], std::array { parameters[level] });
        else
            builder.jump(latches[level], {});
    }
    for (auto const level: std::views::iota(uint32_t { 0 }, depth))
    {
        builder.switchTo(latches[level]);
        builder.branch(condition,
                       headers[level],
                       std::array { parameters[level] },
                       level == 0 ? exit : latches[level - 1],
                       {});
    }
    builder.switchTo(exit);
    builder.ret(std::array { parameters[0] });
}

/// A straight line of @p length i32.add, each of the last and a constant: all fold, then all are dead but
/// the last, which is returned.
void constantChain(Fixture& fixture, uint32_t length)
{
    auto value = fixture.constant(Type::I32, 1);
    auto const one = fixture.constant(Type::I32, 1);
    for ([[maybe_unused]] auto const step: std::views::iota(uint32_t { 0 }, length))
        value = fixture.emit(Operation::I32Add, Type::I32, std::array { value, one });
    fixture.builder.ret(std::array { value });
}

/// A straight line of @p length i32.add of a parameter, of which only the last is used: none folds, all but
/// the last are kept (each is the next one's operand).
void parameterChain(Fixture& fixture, uint32_t length)
{
    auto const input = fixture.builder.addParameter(fixture.entry, Type::I32);
    auto value = input;
    for ([[maybe_unused]] auto const step: std::views::iota(uint32_t { 0 }, length))
        value = fixture.emit(Operation::I32Add, Type::I32, std::array { value, input });
    fixture.builder.ret(std::array { value });
}

/// A switch of @p width cases into a chain of @p width forwarding blocks, then a chain of @p width blocks
/// that each add and jump on: threading redirects every case, merging folds the chain into one block.
void forwardingAndChains(Fixture& fixture, uint32_t width)
{
    auto& builder = fixture.builder;
    auto const input = builder.addParameter(fixture.entry, Type::I32);
    auto forwards = std::vector<BlockId> {};
    auto links = std::vector<BlockId> {};
    for ([[maybe_unused]] auto const position: std::views::iota(uint32_t { 0 }, width))
        forwards.push_back(builder.createBlock());
    for ([[maybe_unused]] auto const position: std::views::iota(uint32_t { 0 }, width))
        links.push_back(builder.createBlock());
    auto const forwarded = builder.addParameter(forwards[0], Type::I32);
    auto cases = std::vector<BranchTo> {};
    auto const arguments = std::array { input };
    for ([[maybe_unused]] auto const position: std::views::iota(uint32_t { 0 }, width))
        cases.push_back(BranchTo { forwards[0], arguments });
    builder.switchOn(input, cases, BranchTo { forwards[0], arguments });
    auto carried = forwarded;
    for (auto const position: std::views::iota(uint32_t { 0 }, width))
    {
        builder.switchTo(forwards[position]);
        if (position + 1 < width)
        {
            auto const next = builder.addParameter(forwards[position + 1], Type::I32);
            builder.jump(forwards[position + 1], std::array { carried });
            carried = next;
        }
        else
            builder.jump(links[0], {});
    }
    auto value = carried;
    for (auto const position: std::views::iota(uint32_t { 0 }, width))
    {
        builder.switchTo(links[position]);
        value = fixture.emit(Operation::I32Add, Type::I32, std::array { value, input });
        if (position + 1 < width)
            builder.jump(links[position + 1], {});
        else
            builder.ret(std::array { value });
    }
}

/// A loop whose header takes @p width parameters, each passed back as it is and never used, and a chain of
/// @p width unused adds in its body: all dead.
void deadLoop(Fixture& fixture, uint32_t width)
{
    auto& builder = fixture.builder;
    auto const condition = builder.addParameter(fixture.entry, Type::I32);
    auto const header = builder.createBlock();
    auto const exit = builder.createBlock();
    auto initial = std::vector<ValueId> {};
    auto parameters = std::vector<ValueId> {};
    for (auto const position: std::views::iota(uint32_t { 0 }, width))
    {
        initial.push_back(fixture.constant(Type::I32, position));
        parameters.push_back(builder.addParameter(header, Type::I32));
    }
    builder.jump(header, initial);
    builder.switchTo(header);
    auto value = parameters[0];
    for (auto const parameter: parameters)
        value = fixture.emit(Operation::I32Add, Type::I32, std::array { value, parameter });
    builder.branch(condition, header, parameters, exit, {});
    builder.switchTo(exit);
    builder.ret({});
}

/// One loop header of a parameter that @p width + 1 edges pass back as it is, from a switch: a parameter as
/// wide as Tarjan's walk and the group's decision get.
void wideLoopHeader(Fixture& fixture, uint32_t width)
{
    auto& builder = fixture.builder;
    auto const input = builder.addParameter(fixture.entry, Type::I32);
    auto const header = builder.createBlock();
    auto const exit = builder.createBlock();
    auto const carried = builder.addParameter(header, Type::I32);
    builder.jump(header, std::array { fixture.constant(Type::I32, 3) });
    builder.switchTo(header);
    auto const passed = std::array { carried };
    auto cases = std::vector<BranchTo>(width, BranchTo { header, passed });
    builder.switchOn(input, cases, BranchTo { exit, {} });
    builder.switchTo(exit);
    builder.ret(std::array { carried });
}

/// A shape a pass is timed on.
struct TimedShape
{
    std::string_view name;                         ///< What it is.
    std::vector<PassRow> rows;                     ///< The passes timed on it.
    std::function<void(Fixture&, uint32_t)> build; ///< Builds it at a size.
};

/// The shapes that would make each pass quadratic if it were, and the whole table on the deepest one.
std::vector<TimedShape> timedShapes()
{
    auto const alone = [](Pass pass) {
        auto const row = rowOf(pass);
        return std::vector<PassRow>(row.begin(), row.end());
    };
    return {
        { "nested loops of trivial parameters", alone(Pass::RemoveTrivialParameters), nestedLoops },
        { "a loop header passed back by a wide switch",
          alone(Pass::RemoveTrivialParameters),
          wideLoopHeader },
        { "a chain of constant adds", alone(Pass::FoldConstants), constantChain },
        { "forwarding blocks and jump chains", alone(Pass::SimplifyControlFlow), forwardingAndChains },
        { "a loop of dead parameters and adds", alone(Pass::EliminateDeadCode), deadLoop },
        { "nested loops through every pass",
          std::vector<PassRow>(PassTable.begin(), PassTable.end()),
          nestedLoops },
    };
}

/// The fastest run of @p shape's passes over its function at each of @p sizes, in seconds, interleaved (each
/// size once per round, five rounds) so that a load burst slows every size alike.
std::vector<double> secondsOf(TimedShape const& shape, std::span<uint32_t const> sizes)
{
    auto best = std::vector<double>(sizes.size(), std::numeric_limits<double>::max());
    for ([[maybe_unused]] auto const round: std::views::iota(0, 5))
        for (auto const [position, size]: core::views::enumerate(sizes))
        {
            auto fixture = Fixture {};
            shape.build(fixture, size);
            auto passes = Passes {};
            auto context = passes.contextFor(fixture.memory, Verification::AtEnd);
            auto const start = std::chrono::steady_clock::now();
            auto const ran = runPasses(fixture.function, context, shape.rows);
            auto const seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            REQUIRE(ran.has_value());
            best[position] = std::min(best[position], seconds);
        }
    return best;
}

} // namespace

TEST_CASE("Optimization.everyPassPollsTheCancellation")
{
    // Each pass on its own, cancelled before it starts, on a function of more work than one interval: it
    // stops with Cancelled, naming the function, rather than running to its end (the bounds-check pass, the
    // identity until P5 Task 8, does no work). Besides many blocks, the work of one walk (a loop header
    // 70'000 edges wide) and of one block (70'000 instructions), so that a poll per block alone does not
    // suffice.
    struct Shape
    {
        std::string_view name;
        void (*build)(Fixture&, uint32_t);
        std::vector<Pass> passes;
    };
    auto const everyPass = std::vector<Pass> {
        Pass::RemoveTrivialParameters, Pass::FoldConstants, Pass::SimplifyControlFlow, Pass::EliminateDeadCode
    };
    auto const shapes = std::to_array<Shape>({
        { "nested loops", nestedLoops, everyPass },
        { "a wide loop header", wideLoopHeader, { Pass::RemoveTrivialParameters } },
        { "one long block", constantChain, { Pass::FoldConstants } },
    });
    for (auto const& shape: shapes)
        for (auto const pass: shape.passes)
        {
            auto const row = rowOf(pass);
            INFO(shape.name << ", " << row[0].name);
            auto fixture = Fixture {};
            shape.build(fixture, 70'000);
            auto passes = Passes {};
            passes.cancellation.cancel();
            auto context = passes.contextFor(fixture.memory);
            context.codeIndex = 7;
            auto const ran = row[0].run(fixture.function, context);
            REQUIRE_FALSE(ran.has_value());
            CHECK(ran.error().code == CompileErrorCode::Cancelled);
            CHECK(ran.error().function == 7U);
        }
}

TEST_CASE("Optimization.deepNestingUsesNoRecursion")
{
    // 100'000 nested loops through every pass, verified after each, under the default stack: every walk is an
    // explicit worklist. The parameters all collapse to the entry's constant.
    auto fixture = Fixture {};
    nestedLoops(fixture, 100'000);
    Passes {}.run(fixture.function);
    for (auto const block: std::views::iota(uint32_t { 1 }, fixture.function.blockCount()))
        REQUIRE(fixture.function.block(BlockId { block }).parameters.count == 0);
}

TEST_CASE("Optimization.passMemoryIsLinear")
{
    // What each pass allocates from the compile's arena (its budget), temporaries and pool growth together,
    // on the shapes everyPassIsLinear times: doubling the size at most doubles it, give or take a container's
    // doubling (2.5).
    for (auto const& shape: timedShapes())
    {
        auto bytes = std::array<uint64_t, 2> {};
        for (auto const [position, size]: core::views::enumerate(std::to_array<uint32_t>({ 10'000, 20'000 })))
        {
            auto fixture = Fixture {};
            shape.build(fixture, size);
            auto passes = Passes {};
            auto context = passes.contextFor(fixture.memory, Verification::AtEnd);
            auto const before = fixture.memory.allocated();
            REQUIRE(runPasses(fixture.function, context, shape.rows).has_value());
            bytes[position] = fixture.memory.allocated() - before;
        }
        INFO(shape.name << ": " << bytes[0] << " bytes at 10'000, " << bytes[1] << " at 20'000");
        CHECK(static_cast<double>(bytes[1]) / static_cast<double>(bytes[0]) < 2.5);
    }
}

TEST_CASE("Optimization.everyPassIsLinear", "[.timing]")
{
    // Each pass on a shape that a quadratic worklist, a rescan per change or a recomputed analysis per change
    // would blow up: doubling the size at most triples the time. Its own ctest row, run serially.
    auto const sizes = std::to_array<uint32_t>({ 20'000, 40'000 });
    for (auto const& shape: timedShapes())
    {
        auto const seconds = secondsOf(shape, sizes);
        INFO(shape.name << ": " << seconds[0] << " s at " << sizes[0] << ", " << seconds[1] << " s at "
                        << sizes[1]);
        CHECK(seconds[1] / seconds[0] < 3.0);
    }
}

TEST_CASE("Optimization.linearOnLongChains", "[.perf]")
{
    // A 1'000'000-instruction straight line through every pass in a release build, without the verifier as a
    // release compile runs them: under 2 s, for a chain that folds away and one that stays.
    for (auto const builder: { constantChain, parameterChain })
    {
        auto fixture = Fixture {};
        builder(fixture, 1'000'000);
        auto passes = Passes {};
        auto context = passes.contextFor(fixture.memory, Verification::AtEnd);
        auto const start = std::chrono::steady_clock::now();
        auto const ran = runPasses(fixture.function, context);
        auto const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        REQUIRE(ran.has_value());
        INFO("all passes: " << seconds << " s, " << instructionsOf(fixture.function) << " instructions left");
        CHECK(seconds < 2.0);
    }
}
