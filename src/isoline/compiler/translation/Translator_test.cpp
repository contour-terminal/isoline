// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/CompileLimits.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Printer.hpp>
#include <isoline/compiler/intermediate/Verifier.hpp>
#include <isoline/compiler/translation/NumericRules.hpp>
#include <isoline/compiler/translation/Translator.hpp>
#include <isoline/numerics/Float.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidatedModule.hpp>

using isoline::FeatureSet;
using isoline::binary::ExternRef;
using isoline::binary::F32;
using isoline::binary::FuncType;
using isoline::binary::I32;
using isoline::binary::I64;
using isoline::binary::Limits;
using isoline::binary::ValueType;
using isoline::binary::testing::br;
using isoline::binary::testing::brIf;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::call;
using isoline::binary::testing::code;
using isoline::binary::testing::drop;
using isoline::binary::testing::encodeName;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::localSet;
using isoline::binary::testing::loop;
using isoline::binary::testing::ModuleBuilder;
using isoline::binary::testing::unreachable;
using isoline::compiler::CompileCancellation;
using isoline::compiler::CompileError;
using isoline::compiler::CompileErrorCode;
using isoline::compiler::CompileLimits;
using isoline::compiler::intermediate::BlockId;
using isoline::compiler::intermediate::Function;
using isoline::compiler::intermediate::Hotness;
using isoline::compiler::intermediate::Operation;
using isoline::compiler::intermediate::print;
using isoline::compiler::intermediate::verify;
using isoline::compiler::translation::ModuleIndex;
using isoline::compiler::translation::NumericRunTable;
using isoline::compiler::translation::translate;
using isoline::compiler::translation::TranslationInput;
using isoline::compiler::translation::TruncationBoundTable;
using isoline::runtime::ExecutionMode;
using isoline::validate::ValidatedModule;

namespace
{

/// Whether a test's compile is cancelled before it starts.
enum class Cancellation : uint8_t
{
    Running,   ///< Not cancelled.
    Cancelled, ///< Cancelled.
};

/// How a test translates.
struct Options
{
    ExecutionMode mode = ExecutionMode::Production;    ///< The mode.
    CompileLimits limits {};                           ///< The caps.
    uint32_t codeIndex = 0;                            ///< The function.
    Cancellation cancellation = Cancellation::Running; ///< Whether it is cancelled.
};

/// One function's translation, with the arena and module it lives on.
struct Translated
{
    std::pmr::monotonic_buffer_resource memory {};
    std::shared_ptr<ValidatedModule const> module {};
    std::optional<Function> function {};
    std::optional<CompileError> error {};

    /// The function's text; fails the test unless it translated.
    [[nodiscard]] std::string text() const
    {
        REQUIRE(function.has_value());
        return print(*function);
    }
};

/// Translates function @p options.codeIndex of @p builder's module, which must validate; a translated
/// function must verify.
std::unique_ptr<Translated> translateOf(ModuleBuilder const& builder, Options const& options = {})
{
    auto result = std::make_unique<Translated>();
    auto loaded = ValidatedModule::load(builder.build(), FeatureSet::all(), isoline::binary::DecodeLimits {});
    REQUIRE(loaded.has_value());
    result->module = std::move(*loaded);
    auto const index = ModuleIndex { result->module->module(), result->memory };
    auto cancellation = CompileCancellation {};
    if (options.cancellation == Cancellation::Cancelled)
        cancellation.cancel();
    auto translated = translate(TranslationInput { .module = *result->module,
                                                   .index = index,
                                                   .codeIndex = options.codeIndex,
                                                   .mode = options.mode,
                                                   .limits = options.limits,
                                                   .cancellation = cancellation },
                                result->memory);
    if (!translated)
    {
        result->error = translated.error();
        return result;
    }
    auto const verified = verify(*translated);
    INFO((verified ? std::string {} : describe(verified.error())) << "\n" << print(*translated));
    REQUIRE(verified.has_value());
    result->function.emplace(std::move(*translated));
    return result;
}

/// The refusal of @p translated; fails the test if it translated.
CompileError refusalOf(Translated const& translated)
{
    REQUIRE(translated.error.has_value());
    return *translated.error;
}

/// A module of the function type @p type and one function of it with @p locals and @p body.
ModuleBuilder oneFunction(FuncType type, std::vector<ValueType> locals, Bytes body)
{
    auto builder = ModuleBuilder {};
    builder.type(std::move(type)).function(0, std::move(locals), std::move(body));
    return builder;
}

/// The lines of @p text.
std::vector<std::string_view> linesOf(std::string const& text)
{
    auto lines = std::vector<std::string_view> {};
    for (auto const line: std::views::split(text, '\n'))
        lines.emplace_back(line.begin(), line.end());
    return lines;
}

/// The index of the first line of @p lines at or after @p from that contains @p needle; none if none.
std::optional<std::size_t> lineWith(std::vector<std::string_view> const& lines,
                                    std::string_view needle,
                                    std::size_t from = 0)
{
    for (auto const index: std::views::iota(from, lines.size()))
        if (lines[index].contains(needle))
            return index;
    return std::nullopt;
}

/// How often @p needle occurs in @p text.
std::size_t countOf(std::string const& text, std::string_view needle)
{
    auto count = std::size_t { 0 };
    auto position = text.find(needle);
    while (position != std::string::npos)
    {
        ++count;
        position = text.find(needle, position + 1);
    }
    return count;
}

/// The instructions of @p function that are @p operation, by index.
std::vector<uint32_t> instructionsOf(Function const& function, Operation operation)
{
    auto found = std::vector<uint32_t> {};
    for (auto const index: std::views::iota(uint32_t { 0 }, function.instructionCount()))
        if (function.instruction(index).operation == operation)
            found.push_back(index);
    return found;
}

} // namespace

TEST_CASE("Translator.entryPollsThenEntersTheFrame")
{
    // The callee's entry in the reference's order: the poll, the frame, then the three context values,
    // then each declared local's zero, a reference local the null of its own heap (extern is heap 1,
    // func heap 0). The exit stores the countdown and leaves the frame before returning.
    auto builder = oneFunction(FuncType { .params = { I32 }, .results = { I32 } },
                               { I64, ExternRef, isoline::binary::FuncRef, I64 },
                               localGet(0));
    builder.memory(Limits { .min = 1, .max = std::nullopt });
    auto const translated = translateOf(builder);
    auto const cost = std::to_string(translated->module->frameCost(0));
    CHECK(translated->text()
          == "b0(v0:i32):\n"
             "  poll_stored [site=0]\n"
             "  enter_frame [frame_cost="
                 + cost
                 + "]\n"
                   "  v1 = load_context [field=step_countdown, type=i64]\n"
                   "  v2 = load_context [field=memory0_base, type=ptr]\n"
                   "  v3 = load_context [field=memory0_bound, type=i64]\n"
                   "  v4 = i64.const [value=0]\n"
                   "  v5 = ref.null [heap=1]\n"
                   "  v6 = ref.null [heap=0]\n"
                   "  store_context v1 [field=step_countdown]\n"
                   "  leave_frame [frame_cost="
                 + cost
                 + "]\n"
                   "  return v0\n");
}

TEST_CASE("Translator.loopHeaderHasParametersOnlyForAssignedLocals")
{
    // Of three locals the loop assigns only local 1: its header takes local 1 and the countdown (no
    // memory, so no base or bound), and the back edge polls in a block of its own.
    auto const translated =
        translateOf(oneFunction(FuncType { .params = { I32 }, .results = {} },
                                { I32, I32 },
                                loop(code({ i32Const(7), localSet(1), localGet(0), brIf(0) }))));
    CHECK(translated->text()
          == "b0(v0:i32):\n"
             "  poll_stored [site=0]\n"
             "  enter_frame [frame_cost="
                 + std::to_string(translated->module->frameCost(0))
                 + "]\n"
                   "  v1 = load_context [field=step_countdown, type=i64]\n"
                   "  v2 = i32.const [value=0]\n"
                   "  jump b1(v2, v1)\n"
                   "b1(v3:i32, v4:i64):\n"
                   "  v5 = i32.const [value=7]\n"
                   "  branch v0, b3(), b2()\n"
                   "b2():\n"
                   "  store_context v4 [field=step_countdown]\n"
                   "  leave_frame [frame_cost="
                 + std::to_string(translated->module->frameCost(0))
                 + "]\n"
                   "  return\n"
                   "b3():\n"
                   "  v6 = poll v4 [site=1]\n"
                   "  jump b1(v5, v6)\n");
}

TEST_CASE("Translator.trivialMergeHasNoParameter")
{
    SECTION("nothing differs: the end takes no parameter")
    {
        auto const translated = translateOf(oneFunction(
            FuncType { .params = { I32 }, .results = { I32 } },
            {},
            code({ isoline::binary::testing::block(code({ localGet(0), brIf(0) })), localGet(0) })));
        auto const text = translated->text();
        CHECK(countOf(text, "jump b2(v0") == 0);
        CHECK(text.find("b2():\n"));
    }
    SECTION("an assigned local that differs takes one, one that does not none")
    {
        // Local 1 is 5 on the br_if's edge and 6 on the fallthrough; local 2 is 9 on both.
        auto const translated =
            translateOf(oneFunction(FuncType { .params = { I32 }, .results = { I32 } },
                                    { I32, I32 },
                                    code({ isoline::binary::testing::block(code({ i32Const(9),
                                                                                  localSet(2),
                                                                                  i32Const(5),
                                                                                  localSet(1),
                                                                                  localGet(0),
                                                                                  brIf(0),
                                                                                  i32Const(6),
                                                                                  localSet(1) })),
                                           localGet(1),
                                           localGet(2),
                                           { 0x6A } })));
        auto const text = translated->text();
        INFO(text);
        // b1 is the br_if's continuation, b2 the end: one parameter, for local 1; local 2 is the 9.
        CHECK(text.contains("  v3 = i32.const [value=9]\n"
                            "  v4 = i32.const [value=5]\n"
                            "  branch v0, b2(v4), b1()\n"
                            "b1():\n"
                            "  v5 = i32.const [value=6]\n"
                            "  jump b2(v5)\n"
                            "b2(v6:i32):\n"
                            "  v7 = i32.add v6, v3\n"));
    }
}

TEST_CASE("Translator.everyLoopEdgePolls")
{
    // br_if, br_table (twice to the loop) and br, each to the loop: every edge into the header but the
    // entry's comes from a block that polls at LoopBackEdge, and no block that branches or switches does.
    auto const translated = translateOf(oneFunction(
        FuncType { .params = { I32 }, .results = {} },
        {},
        loop(code(
            { localGet(0),
              brIf(0),
              isoline::binary::testing::block(code({ localGet(0), { 0x0E, 0x03, 0x01, 0x00, 0x01, 0x01 } })),
              br(0) }))));
    auto const& function = *translated->function;
    auto const header = BlockId { 1 };
    auto edges = 0;
    for (auto const index: std::views::iota(uint32_t { 1 }, function.blockCount()))
    {
        auto const block = BlockId { index };
        auto const successors = function.successors(block);
        if (!std::ranges::contains(successors, header))
            continue;
        ++edges;
        INFO("block " << index << "\n" << print(function));
        auto const& instructions = function.block(block).instructions;
        CHECK(std::ranges::any_of(instructions, [&](uint32_t instruction) {
            auto const& polled = function.instruction(instruction);
            return polled.operation == Operation::Poll
                   && function.immediates(polled.immediates)[0]
                          == std::to_underlying(isoline::runtime::PollSite::LoopBackEdge);
        }));
        CHECK(function.instruction(*function.terminator(block)).operation == Operation::Jump);
    }
    CHECK(edges == 3); // br_if's, br_table's (one for both of its entries) and br's
    CHECK(instructionsOf(function, Operation::Poll).size() == 3);
    CHECK_FALSE(function.block(header).instructions.empty());
    CHECK(std::ranges::none_of(function.block(header).instructions, [&](uint32_t instruction) {
        return function.instruction(instruction).operation == Operation::Poll;
    }));
}

TEST_CASE("Translator.stepCountdownIsALoopParameter")
{
    // (loop (call $f) (br 0)): the header takes the countdown, stores it before the call and reloads it
    // after; the back edge polls what the call left.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).importFunction("env", "f", 0).function(0, {}, loop(code({ call(0), br(0) })));
    auto const translated = translateOf(builder);
    auto const text = translated->text();
    CHECK(text.contains("b1(v1:i64):\n"
                        "  store_context v1 [field=step_countdown]\n"
                        "  call_import [function=0]\n"
                        "  v2 = load_context [field=step_countdown, type=i64]\n"
                        "  jump b2()\n"
                        "b2():\n"
                        "  v3 = poll v2 [site=1]\n"
                        "  jump b1(v3)\n"));
}

TEST_CASE("Translator.deterministicCanonicalizes")
{
    auto const builder = oneFunction(FuncType { .params = { F32, F32 }, .results = { F32 } },
                                     {},
                                     code({ localGet(0), localGet(1), { 0x92 }, { 0x8B } }));
    auto const deterministic = translateOf(builder, Options { .mode = ExecutionMode::Deterministic });
    auto const production = translateOf(builder, Options { .mode = ExecutionMode::Production });
    // f32.add may make a NaN and is canonicalized; f32.abs keeps its operand's bits and is not.
    CHECK(deterministic->text().contains("  v3 = f32.add v0, v1\n"
                                         "  v4 = canonicalize_nan v3\n"
                                         "  v5 = f32.abs v4\n"));
    CHECK(countOf(deterministic->text(), "canonicalize_nan") == 1);
    CHECK(countOf(production->text(), "canonicalize_nan") == 0);
}

TEST_CASE("Translator.divisionChecks")
{
    SECTION("div_s: a zero divisor, then the minimum divided by -1")
    {
        auto const translated =
            translateOf(oneFunction(FuncType { .params = { I32, I32 }, .results = { I32 } },
                                    {},
                                    code({ localGet(0), localGet(1), { 0x6D } })));
        CHECK(translated->text().contains("  v3 = i32.eqz v1\n"
                                          "  trap_if v3 [trap=1]\n"
                                          "  v4 = i32.const [value=2147483648]\n"
                                          "  v5 = i32.const [value=4294967295]\n"
                                          "  v6 = i32.eq v0, v4\n"
                                          "  v7 = i32.eq v1, v5\n"
                                          "  v8 = i32.and v6, v7\n"
                                          "  trap_if v8 [trap=2]\n"
                                          "  v9 = i32.div_s v0, v1\n"));
    }
    SECTION("rem_s, div_u and rem_u: a zero divisor only")
    {
        for (auto const opcode: { uint8_t { 0x6F }, uint8_t { 0x6E }, uint8_t { 0x82 } })
        {
            auto const type = opcode == 0x82 ? I64 : I32;
            auto const translated =
                translateOf(oneFunction(FuncType { .params = { type, type }, .results = { type } },
                                        {},
                                        code({ localGet(0), localGet(1), { opcode } })));
            auto const text = translated->text();
            INFO(text);
            CHECK(countOf(text, "trap_if") == 1);
            CHECK(countOf(text, "[trap=1]") == 1);
        }
    }
    SECTION("a trap site stores a countdown a loop's poll changed, once")
    {
        // In the loop body the countdown is the header's parameter: the first trap site stores it, the
        // second finds it stored.
        auto const translated = translateOf(oneFunction(FuncType { .params = { I32, I32 }, .results = {} },
                                                        {},
                                                        loop(code({ localGet(0),
                                                                    localGet(1),
                                                                    { 0x6E },
                                                                    drop(),
                                                                    localGet(0),
                                                                    localGet(1),
                                                                    { 0x6E },
                                                                    drop() }))));
        CHECK(countOf(translated->text(), "store_context") == 2); // the trap site's, and the exit's
    }
}

TEST_CASE("Translator.truncationChecksMatchTheReference")
{
    // A trapping truncation checks for a NaN, then for an operand outside its TruncationBoundTable row's
    // open interval. Each bound traps IntegerOverflow in the reference's kernel, and the float just
    // inside it converts: the interval is exactly what numerics::truncTrapping converts.
    using isoline::numerics::truncTrapping;
    using isoline::runtime::TrapCode;
    namespace runtime = isoline::runtime;
    using Kernel = std::function<std::optional<TrapCode>(uint64_t)>;
    auto const trapOf = [](auto const& result) -> std::optional<TrapCode> {
        if (result)
            return std::nullopt;
        return result.error().code;
    };
    auto const kernels = std::vector<std::pair<Operation, Kernel>> {
        { Operation::I32TruncF32S,
          [&](uint64_t bits) {
              return trapOf(
                  truncTrapping<runtime::I32, int32_t>(runtime::F32 { static_cast<uint32_t>(bits) }));
          } },
        { Operation::I32TruncF32U,
          [&](uint64_t bits) {
              return trapOf(
                  truncTrapping<runtime::I32, uint32_t>(runtime::F32 { static_cast<uint32_t>(bits) }));
          } },
        { Operation::I32TruncF64S,
          [&](uint64_t bits) {
              return trapOf(truncTrapping<runtime::I32, int32_t>(runtime::F64 { bits }));
          } },
        { Operation::I32TruncF64U,
          [&](uint64_t bits) {
              return trapOf(truncTrapping<runtime::I32, uint32_t>(runtime::F64 { bits }));
          } },
        { Operation::I64TruncF32S,
          [&](uint64_t bits) {
              return trapOf(
                  truncTrapping<runtime::I64, int64_t>(runtime::F32 { static_cast<uint32_t>(bits) }));
          } },
        { Operation::I64TruncF32U,
          [&](uint64_t bits) {
              return trapOf(
                  truncTrapping<runtime::I64, uint64_t>(runtime::F32 { static_cast<uint32_t>(bits) }));
          } },
        { Operation::I64TruncF64S,
          [&](uint64_t bits) {
              return trapOf(truncTrapping<runtime::I64, int64_t>(runtime::F64 { bits }));
          } },
        { Operation::I64TruncF64U,
          [&](uint64_t bits) {
              return trapOf(truncTrapping<runtime::I64, uint64_t>(runtime::F64 { bits }));
          } },
    };
    REQUIRE(kernels.size() == TruncationBoundTable.size());
    for (auto const& row: TruncationBoundTable)
    {
        INFO(isoline::compiler::intermediate::nameOf(row.operation));
        auto const kernel = std::ranges::find(kernels, row.operation, &std::pair<Operation, Kernel>::first);
        REQUIRE(kernel != kernels.end());
        // Both bounds' bits step inward by one: the lower bound is negative, the upper positive.
        CHECK(kernel->second(row.lower) == TrapCode::IntegerOverflow);
        CHECK(kernel->second(row.lower - 1) == std::nullopt);
        CHECK(kernel->second(row.upper) == TrapCode::IntegerOverflow);
        CHECK(kernel->second(row.upper - 1) == std::nullopt);
    }
    // And translation emits the checks in the reference's order: the NaN first.
    auto const translated = translateOf(
        oneFunction(FuncType { .params = { F32 }, .results = { I32 } }, {}, code({ localGet(0), { 0xA8 } })));
    CHECK(translated->text().contains("  v2 = f32.ne v0, v0\n"
                                      "  trap_if v2 [trap=3]\n"
                                      "  v3 = f32.const [value=3472883713]\n"
                                      "  v4 = f32.const [value=1325400064]\n"
                                      "  v5 = f32.gt v0, v3\n"
                                      "  v6 = f32.lt v0, v4\n"
                                      "  v7 = i32.and v5, v6\n"
                                      "  v8 = i32.eqz v7\n"
                                      "  trap_if v8 [trap=2]\n"
                                      "  v9 = i32.trunc_f32_s v0\n"));
}

TEST_CASE("Translator.everyNumericOperationTranslates")
{
    // Every opcode of NumericRunTable but the constants, in a function of its operand types: it
    // translates to its operation and verifies, in both modes.
    auto const operandTypeOf = [](isoline::compiler::intermediate::OperandType type) {
        using isoline::compiler::intermediate::OperandType;
        switch (type)
        {
            case OperandType::I32: return I32;
            case OperandType::I64: return I64;
            case OperandType::F32: return F32;
            case OperandType::F64: return isoline::binary::F64;
            default: FAIL("not a numeric operand"); return I32;
        }
    };
    auto const resultTypeOf = [](isoline::compiler::intermediate::Type type) {
        using isoline::compiler::intermediate::Type;
        switch (type)
        {
            case Type::I32: return I32;
            case Type::I64: return I64;
            case Type::F32: return F32;
            case Type::F64: return isoline::binary::F64;
            default: FAIL("not a numeric result"); return I32;
        }
    };
    auto count = 0;
    for (auto const& run: NumericRunTable)
        for (auto const value: std::views::iota(std::size_t { std::to_underlying(run.first) },
                                                std::size_t { std::to_underlying(run.last) } + 1))
        {
            auto const operation = static_cast<Operation>(std::to_underlying(run.operation) + value
                                                          - std::to_underlying(run.first));
            auto const& row = isoline::compiler::intermediate::operationRow(operation);
            if (row.immediates == isoline::compiler::intermediate::ImmediateSchema::Value)
                continue;
            auto const& info = isoline::binary::OpcodeTable[value];
            INFO(info.text);
            auto type = FuncType {};
            auto body = Bytes {};
            for (auto const [position, operand]: core::views::enumerate(
                     isoline::compiler::intermediate::operandSignatureRow(row.operands).fixed()))
            {
                type.params.push_back(operandTypeOf(operand));
                body = code({ body, localGet(static_cast<uint32_t>(position)) });
            }
            type.results.push_back(resultTypeOf(*isoline::compiler::intermediate::fixedTypeOf(row.result)));
            body = info.prefix == isoline::binary::NoPrefix
                       ? code({ body, { static_cast<uint8_t>(info.code) } })
                       : code({ body, { info.prefix }, encodeU32(info.code) });
            for (auto const mode: { ExecutionMode::Production, ExecutionMode::Deterministic })
            {
                auto const translated = translateOf(oneFunction(type, {}, body), Options { .mode = mode });
                CHECK(instructionsOf(*translated->function, operation).size() == 1);
            }
            ++count;
        }
    // Every opcode of the runs but the four constants.
    auto const opcodes = std::ranges::fold_left(NumericRunTable, 0, [](int sum, auto const& run) {
        return sum + std::to_underlying(run.last) - std::to_underlying(run.first) + 1;
    });
    CHECK(count == opcodes - 4);
}

TEST_CASE("Translator.refNullKeepsItsHeap")
{
    // ref.null extern through an if's merge parameter and into a call: heap 1 all the way, never func's 0.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { ExternRef }, .results = {} })
        .type(FuncType { .params = { I32, ExternRef }, .results = {} })
        .importFunction("env", "g", 0)
        .function(1,
                  {},
                  code({ localGet(0),
                         { 0x04 },
                         isoline::binary::testing::encodeValueType(ExternRef),
                         { 0xD0, 0x6F },
                         { 0x05 },
                         localGet(1),
                         { 0x0B },
                         call(0) }));
    auto const translated = translateOf(builder);
    auto const text = translated->text();
    INFO(text);
    CHECK(countOf(text, "ref.null [heap=1]") == 1);
    CHECK(countOf(text, "heap=0") == 0);
    auto const lines = linesOf(text);
    auto const merge = lineWith(lines, ":ref");
    REQUIRE(merge.has_value());
    auto const parameter = std::string { lines[*merge].substr(lines[*merge].contains('(') + 1) };
    auto const name = parameter.substr(0, parameter.find(':'));
    CHECK(text.find("call_import " + name + " [function=0]"));
}

TEST_CASE("Translator.callWithAThousandArguments")
{
    // A call of 1'000 arguments verifies and holds them all in the operand pool; a pool cap below them
    // refuses it by name, before the Builder's own bound.
    auto type = FuncType {};
    auto body = Bytes {};
    for (auto const local: std::views::iota(uint32_t { 0 }, uint32_t { 1'000 }))
    {
        type.params.push_back(I32);
        body = code({ body, localGet(local) });
    }
    body = code({ body, call(0) });
    auto const builder = oneFunction(type, {}, body);
    auto const translated = translateOf(builder);
    auto const calls = instructionsOf(*translated->function, Operation::Call);
    REQUIRE(calls.size() == 1);
    CHECK(translated->function->instruction(calls[0]).operands.count == 1'000);
    CHECK(translated->function->operandPoolSize() >= 2'000);

    auto limits = CompileLimits {};
    limits.maxPoolEntries = 2'500;
    auto const refused = translateOf(builder, Options { .limits = limits });
    CHECK(refusalOf(*refused).code == CompileErrorCode::PoolTooLarge);
    CHECK(refusalOf(*refused).function == 0);
    CHECK(refusalOf(*refused).limit == 2'500);
}

TEST_CASE("Translator.deepNestingIsIterative")
{
    // 100'000 nested blocks, each falling through to its end: translation keeps its constructs on the
    // heap and the result verifies (ASan, the default stack).
    constexpr auto Depth = uint32_t { 100'000 };
    auto body = Bytes {};
    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, Depth))
        body.insert(body.end(), { 0x02, 0x40 });
    body.insert(body.end(), Depth, 0x0B);
    auto const translated = translateOf(oneFunction(FuncType {}, {}, body));
    CHECK(translated->function->blockCount() == Depth + 1);
}

TEST_CASE("Translator.deadCodeEmitsNothing")
{
    // After unreachable, br and return, nothing until the construct's end.
    auto const translated =
        translateOf(oneFunction(FuncType { .params = {}, .results = { I32 } },
                                {},
                                code({ isoline::binary::testing::block(code({ br(0), i32Const(11), drop() })),
                                       unreachable(),
                                       i32Const(12),
                                       { 0x0F },
                                       i32Const(13) })));
    auto const text = translated->text();
    INFO(text);
    CHECK(countOf(text, "i32.const") == 0);
    CHECK(text.contains("  trap [trap=0]\n"));
    // The trap site after the block's merge stores the countdown: the merge knows nothing stored.
    CHECK(text.contains("  store_context v0 [field=step_countdown]\n  trap [trap=0]\n"));
}

TEST_CASE("Translator.branchHintsMakeTheUnlikelySuccessorCold")
{
    // `local.get 0 if nop else nop end`: the if sits at offset 3 from the locals declaration (one byte,
    // no locals). An unlikely hint makes the then arm cold, a likely one the else arm.
    auto const hinted = [](uint8_t likelihood) {
        auto builder = oneFunction(FuncType { .params = { I32 }, .results = {} },
                                   {},
                                   code({ localGet(0), { 0x04, 0x40, 0x01, 0x05, 0x01, 0x0B } }));
        auto section = encodeName(isoline::binary::BranchHintSectionName);
        section.insert(section.end(), { 0x01, 0x00, 0x01, 0x03, 0x01, likelihood });
        builder.rawSection(0, std::move(section));
        return builder;
    };
    auto const unlikely = translateOf(hinted(0x00));
    REQUIRE(unlikely->module->module().branchHints.size() == 1);
    CHECK(unlikely->function->block(BlockId { 1 }).hotness == Hotness::Cold);
    CHECK(unlikely->function->block(BlockId { 2 }).hotness == Hotness::Normal);
    auto const likely = translateOf(hinted(0x01));
    CHECK(likely->function->block(BlockId { 1 }).hotness == Hotness::Normal);
    CHECK(likely->function->block(BlockId { 2 }).hotness == Hotness::Cold);
}

TEST_CASE("Translator.globalsSelectAndReferences")
{
    // global.get and global.set through their cells, typed select, ref.func and ref.is_null.
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { I32 }, .results = { I32 } })
        .global(isoline::binary::GlobalType { .type = I64, .mutability = isoline::binary::Mutability::Var },
                Bytes { 0x42, 0x00 })
        .function(0,
                  {},
                  code({ { 0x23, 0x00 },
                         { 0x24, 0x00 },
                         { 0xD2, 0x00 },
                         { 0xD1 },
                         localGet(0),
                         localGet(0),
                         { 0x1C, 0x01, 0x7F },
                         localGet(0),
                         localGet(0),
                         { 0x1B } }));
    builder.exportFunction("f", 0);
    auto const translated = translateOf(builder);
    auto const text = translated->text();
    INFO(text);
    CHECK(text.contains("= load_global [global=0, type=i64]"));
    CHECK(text.contains("store_global v"));
    CHECK(text.contains("= ref.func [function=0]"));
    CHECK(text.contains("= ref.is_null v"));
    CHECK(countOf(text, "= select v") == 2);
}

TEST_CASE("Translator.refusesPastItsCaps")
{
    auto const builder = oneFunction(FuncType { .params = { I32 }, .results = {} },
                                     { I32 },
                                     loop(code({ i32Const(1), localSet(1), localGet(0), brIf(0) })));
    SECTION("a body of more units than maxFunctionBodyUnits")
    {
        auto limits = CompileLimits {};
        limits.maxFunctionBodyUnits = 3;
        auto const refused = translateOf(builder, Options { .limits = limits });
        CHECK(refusalOf(*refused)
              == CompileError {
                  .code = CompileErrorCode::FunctionTooLarge, .function = 0, .requested = 8, .limit = 3 });
    }
    SECTION("more IR instructions than maxFunctionInstructions")
    {
        auto limits = CompileLimits {};
        limits.maxFunctionInstructions = 4;
        CHECK(refusalOf(*translateOf(builder, Options { .limits = limits })).code
              == CompileErrorCode::TooManyInstructions);
        limits.maxFunctionInstructions = 13;
        CHECK(translateOf(builder, Options { .limits = limits })->function.has_value());
    }
    SECTION("a loop header of more parameters than maxBlockParameters")
    {
        // The header takes local 1 and the countdown.
        auto limits = CompileLimits {};
        limits.maxBlockParameters = 1;
        CHECK(refusalOf(*translateOf(builder, Options { .limits = limits }))
              == CompileError { .code = CompileErrorCode::TooManyBlockParameters,
                                .function = 0,
                                .requested = 2,
                                .limit = 1 });
        limits.maxBlockParameters = 2;
        CHECK(translateOf(builder, Options { .limits = limits })->function.has_value());
    }
    SECTION("more joinings than maxTotalBlockParameters")
    {
        auto limits = CompileLimits {};
        limits.maxTotalBlockParameters = 0;
        CHECK(refusalOf(*translateOf(builder, Options { .limits = limits }))
              == CompileError { .code = CompileErrorCode::TooManyBlockParameters,
                                .function = 0,
                                .requested = 1,
                                .limit = 0 });
    }
    SECTION("a cancelled compile")
    {
        CHECK(refusalOf(*translateOf(builder, Options { .cancellation = Cancellation::Cancelled }))
              == CompileError { .code = CompileErrorCode::Cancelled, .function = 0 });
    }
    SECTION("a deferred feature")
    {
        // A v128 local: SIMD is P5b's.
        auto const simd = oneFunction(FuncType {}, { isoline::binary::V128 }, {});
        CHECK(refusalOf(*translateOf(simd)).code == CompileErrorCode::UnsupportedFeature);
    }
}
