// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
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
#include <isoline/compiler/translation/testing/Translation.hpp>
#include <isoline/numerics/Float.hpp>
#include <isoline/runtime/CountingArena.hpp>
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
using isoline::compiler::translation::ModuleIndex;
using isoline::compiler::translation::NumericRunTable;
using isoline::compiler::translation::translate;
using isoline::compiler::translation::TranslationInput;
using isoline::compiler::translation::TruncationBoundTable;
using isoline::runtime::ExecutionMode;
using isoline::validate::ValidatedModule;
using namespace isoline::compiler::translation::testing;

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
        CHECK(text.contains("b2():\n"));
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
    auto const merge = lineWith(lines, ":ref", 1); // past the entry, whose parameter 1 is an externref
    REQUIRE(merge.has_value());
    auto const parameter = std::string { lines[*merge].substr(lines[*merge].find('(') + 1) };
    auto const name = parameter.substr(0, parameter.find(':'));
    REQUIRE(name.starts_with("v"));
    CHECK(text.contains("call_import " + name + " [function=0]"));
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

namespace
{

/// A resource that cancels a compile once it has handed out more than a threshold of bytes: the one way
/// a single-threaded test reaches a compile between two of its cancellation polls.
class CancellingResource final: public std::pmr::memory_resource
{
  public:
    CancellingResource(CompileCancellation& cancellation, std::size_t threshold) noexcept:
        _cancellation { cancellation }, _threshold { threshold }
    {
    }

  private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        _allocated += bytes;
        if (_allocated > _threshold)
            _cancellation.cancel();
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

    CompileCancellation& _cancellation;
    std::size_t _threshold;
    std::size_t _allocated = 0;
};

/// A body of @p count `i32.const 1; drop`: one IR instruction each.
Bytes constants(uint32_t count)
{
    auto body = Bytes {};
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
        body.insert(body.end(), { 0x41, 0x01, 0x1A });
    return body;
}

/// Translates function 0 of @p builder's module on @p memory with @p limits and @p cancellation.
std::expected<Function, CompileError> translateOn(ModuleBuilder const& builder,
                                                  std::pmr::memory_resource& memory,
                                                  CompileCancellation const& cancellation,
                                                  CompileLimits const& limits = {})
{
    auto loaded = ValidatedModule::load(builder.build(), FeatureSet::all(), isoline::binary::DecodeLimits {});
    REQUIRE(loaded.has_value());
    auto const module = std::move(*loaded);
    auto const index = ModuleIndex { module->module(), memory };
    return translate(TranslationInput { .module = *module,
                                        .index = index,
                                        .codeIndex = 0,
                                        .mode = ExecutionMode::Production,
                                        .limits = limits,
                                        .cancellation = cancellation },
                     memory);
}

/// A switch of @p cases: as many nested blocks, a br_table to each, and case i assigning local i and
/// returning it. Each case's code follows the end of its block, which only the table's edge reaches.
ModuleBuilder switchOf(uint32_t cases)
{
    auto body = Bytes {};
    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, cases))
        body.insert(body.end(), { 0x02, 0x40 });
    body = code({ body, localGet(0), { 0x0E }, encodeU32(cases - 1) });
    for (auto const label: std::views::iota(uint32_t { 0 }, cases))
        body = code({ body, encodeU32(label) });
    for (auto const label: std::views::iota(uint32_t { 0 }, cases))
        body = code({ body,
                      { 0x0B },
                      i32Const(static_cast<int32_t>(label)),
                      localSet(label),
                      localGet(label),
                      { 0x0F } });
    return oneFunction(FuncType { .params = { I32 }, .results = { I32 } }, std::vector(cases - 1, I32), body);
}

} // namespace

TEST_CASE("Translator.cancellationIsPolledEveryIntervalOfIR")
{
    // 200'000 IR instructions; the compile is cancelled once the arena has handed out 1 MiB, well after
    // the first poll: translation stops at a later one. With the threshold out of reach it completes.
    auto const builder = oneFunction(FuncType { .params = {}, .results = {} }, {}, constants(200'000));
    {
        auto cancellation = CompileCancellation {};
        auto memory = CancellingResource { cancellation, std::size_t { 1 } << 20 };
        auto const cancelled = translateOn(builder, memory, cancellation);
        REQUIRE_FALSE(cancelled.has_value());
        CHECK(cancelled.error() == CompileError { .code = CompileErrorCode::Cancelled, .function = 0 });
    }
    {
        auto cancellation = CompileCancellation {};
        auto memory = CancellingResource { cancellation, std::numeric_limits<std::size_t>::max() };
        auto const translated = translateOn(builder, memory, cancellation);
        REQUIRE(translated.has_value());
        CHECK(translated->instructionCount() > 200'000);
    }
}

TEST_CASE("Translator.switchMergesStayLinear")
{
    // A br_table switch of n cases, case i assigning local i: the table's edges pass no locals (nothing is
    // modified on their path), so no case's block takes a parameter, and the operand pool grows linearly.
    // Pinned: at most 5 pool entries per case, at n = 500 and at 2n alike (measured 4.01). The design
    // this replaced passed each block's assigned set on its table edge: n / 2 entries per case.
    auto const entriesPerCase = [](uint32_t cases) {
        auto const translated = translateOf(switchOf(cases));
        auto const& function = *translated->function;
        for (auto const block: std::views::iota(uint32_t { 1 }, function.blockCount()))
            CHECK(function.block(BlockId { block }).parameters.count == 0);
        return static_cast<double>(function.operandPoolSize()) / cases;
    };
    auto const small = entriesPerCase(500);
    auto const large = entriesPerCase(1'000);
    INFO("pool entries per case: " << small << " at 500 cases, " << large << " at 1'000");
    CHECK(small <= 5);
    CHECK(large <= 5);
    CHECK(large / small < 1.1);
}

TEST_CASE("Translator.branchArgumentsAreCharged", "[.hostile]")
{
    // A loop assigning 5'000 locals, then k × `local.get 0; br_if 0`: each back edge passes the 5'000, so
    // the function asks for 5'000 k branch arguments from 3 k bytes. It is refused TooManyBranchArguments
    // at the cap's edge, whatever k: the arena's peak is the same at k and 2k (pinned: within 5 %). Run
    // capped (tests/CMakeLists.txt), on an arena that refuses past 1 GiB.
    constexpr auto Locals = uint32_t { 5'000 };
    auto const peakFor = [](uint32_t branches) {
        auto inner = Bytes {};
        for (auto const local: std::views::iota(uint32_t { 1 }, Locals + 1))
            inner = code({ inner, i32Const(0), localSet(local) });
        inner.reserve(inner.size() + (std::size_t { 3 } * branches));
        for ([[maybe_unused]] auto const branch: std::views::iota(uint32_t { 0 }, branches))
            inner.insert(inner.end(), { 0x20, 0x00, 0x0D, 0x00 });
        auto const builder =
            oneFunction(FuncType { .params = { I32 }, .results = {} }, std::vector(Locals, I32), loop(inner));
        auto arena =
            isoline::runtime::CountingArena { uint64_t { 1 } << 30, *std::pmr::new_delete_resource() };
        auto cancellation = CompileCancellation {};
        auto const refused = translateOn(builder, arena, cancellation);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == CompileErrorCode::TooManyBranchArguments);
        CHECK(refused.error().limit == 16'000'000);
        return static_cast<double>(arena.peak());
    };
    auto const small = peakFor(100'000);
    auto const large = peakFor(200'000);
    INFO("arena peak: " << small << " bytes at 100'000 branches, " << large << " at 200'000");
    CHECK(large / small < 1.05);
}

TEST_CASE("Translator.deadPathModificationsAreUndone")
{
    SECTION("a block's end reached only by an edge before the assignment")
    {
        // block { br_if 0; local 1 = 5; return 7 } local.get 1: after the block, local 1 is its zero.
        auto const translated = translateOf(oneFunction(
            FuncType { .params = { I32 }, .results = { I32 } },
            { I32 },
            code({ isoline::binary::testing::block(
                       code({ localGet(0), brIf(0), i32Const(5), localSet(1), i32Const(7), { 0x0F } })),
                   localGet(1) })));
        auto const text = translated->text();
        INFO(text);
        CHECK(text.contains("  v2 = i32.const [value=0]\n"));
        CHECK(text.ends_with("  return v2\n"));
    }
    SECTION("an else arm starts from the if's entry values")
    {
        // if { local 1 = 5 } else { local 2 = local 1 } local.get 2: the else arm reads local 1's zero,
        // so local 2 is the zero on both arms and takes no parameter; local 1 takes one.
        auto const translated = translateOf(oneFunction(FuncType { .params = { I32 }, .results = { I32 } },
                                                        { I32, I32 },
                                                        code({ localGet(0),
                                                               { 0x04, 0x40 },
                                                               i32Const(5),
                                                               localSet(1),
                                                               { 0x05 },
                                                               localGet(1),
                                                               localSet(2),
                                                               { 0x0B },
                                                               localGet(2) })));
        auto const text = translated->text();
        INFO(text);
        CHECK(text.contains(
            "b1():\n  v3 = i32.const [value=5]\n  jump b3(v3)\nb2():\n  jump b3(v2)\nb3(v4:i32):\n"));
        CHECK(text.ends_with("  return v2\n"));
    }
}

TEST_CASE("Translator.branchAndMergeChargesArePinned")
{
    // block { local 1 = 5; br_if 0 (local 0); [local 1 = 6] }: each forward edge is charged its results
    // (none) and the one local its path modified; the merge, when local 1 differs, one parameter per edge.
    auto const builderOf = [](Bytes const& tail) {
        return oneFunction(
            FuncType { .params = { I32 }, .results = {} },
            { I32 },
            isoline::binary::testing::block(code({ i32Const(5), localSet(1), localGet(0), brIf(0), tail })));
    };
    auto const chargedUpTo = [](ModuleBuilder const& builder, uint64_t limit) {
        auto limits = CompileLimits {};
        limits.maxBranchArguments = limit;
        return translateOf(builder, Options { .limits = limits });
    };
    SECTION("the forward edges: the br_if's and the fallthrough's, one local each")
    {
        auto const same = builderOf({});
        CHECK(refusalOf(*chargedUpTo(same, 1))
              == CompileError { .code = CompileErrorCode::TooManyBranchArguments,
                                .function = 0,
                                .requested = 2,
                                .limit = 1 });
        CHECK(chargedUpTo(same, 2)->function.has_value());
    }
    SECTION("the merge: one parameter for each of the two edges")
    {
        auto const differing = builderOf(code({ i32Const(6), localSet(1) }));
        CHECK(refusalOf(*chargedUpTo(differing, 3))
              == CompileError { .code = CompileErrorCode::TooManyBranchArguments,
                                .function = 0,
                                .requested = 4,
                                .limit = 3 });
        CHECK(chargedUpTo(differing, 4)->function.has_value());
    }
}

namespace
{

/// block { if (local 0) { k × (local.get i; local.set i); br 1 } end; k × (local.get 0; br_if 0) } end:
/// one edge passes k locals, k edges pass none, into one end.
ModuleBuilder wideUnionOf(uint32_t locals)
{
    auto arm = Bytes {};
    for (auto const local: std::views::iota(uint32_t { 1 }, locals + 1))
        arm = code({ arm, localGet(local), localSet(local) });
    auto body = code({ { 0x02, 0x40 }, localGet(0), { 0x04, 0x40 }, arm, br(1), { 0x0B } });
    body.reserve(body.size() + (std::size_t { 4 } * locals) + 1);
    for ([[maybe_unused]] auto const branch: std::views::iota(uint32_t { 0 }, locals))
        body.insert(body.end(), { 0x20, 0x00, 0x0D, 0x00 });
    body.push_back(0x0B);
    return oneFunction(FuncType { .params = { I32 }, .results = {} }, std::vector(locals, I32), body);
}

/// The fastest translation of function 0 of each of @p builders, in seconds, from runs that interleave
/// them (each builder once per round, five rounds): a load burst on the machine then slows every builder
/// alike rather than the one whose runs it happens to overlap.
std::vector<double> interleavedTranslationSeconds(std::span<ModuleBuilder const> builders)
{
    auto modules = std::vector<std::shared_ptr<ValidatedModule const>> {};
    for (auto const& builder: builders)
    {
        auto loaded =
            ValidatedModule::load(builder.build(), FeatureSet::all(), isoline::binary::DecodeLimits {});
        REQUIRE(loaded.has_value());
        modules.push_back(std::move(*loaded));
    }
    auto best = std::vector<double>(modules.size(), std::numeric_limits<double>::max());
    for ([[maybe_unused]] auto const round: std::views::iota(0, 5))
        for (auto const [position, module]: core::views::enumerate(modules))
        {
            auto memory = std::pmr::monotonic_buffer_resource {};
            auto const index = ModuleIndex { module->module(), memory };
            auto const cancellation = CompileCancellation {};
            auto const limits = CompileLimits {};
            auto const start = std::chrono::steady_clock::now();
            auto const translated = translate(TranslationInput { .module = *module,
                                                                 .index = index,
                                                                 .codeIndex = 0,
                                                                 .mode = ExecutionMode::Production,
                                                                 .limits = limits,
                                                                 .cancellation = cancellation },
                                              memory);
            auto const seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            REQUIRE(translated.has_value());
            best[position] = std::min(best[position], seconds);
        }
    return best;
}

} // namespace

TEST_CASE("Translator.sealTimeIsLinear", "[.timing]")
{
    // One edge passing k locals and k edges passing none, into one end: the seal costs what the edges
    // passed, k, not edges × union, k². Pinned: doubling k at most triples the translation time. Its own
    // ctest row, run serially (tests/CMakeLists.txt).
    auto const builders = std::to_array({ wideUnionOf(10'000), wideUnionOf(20'000) });
    auto const seconds = interleavedTranslationSeconds(builders);
    auto const small = seconds[0];
    auto const large = seconds[1];
    INFO("translation: " << small << " s at k = 10'000, " << large << " s at k = 20'000");
    CHECK(large / small < 3.0);
}

namespace
{

/// One function of every construct whose translation reserves, charges or caps: if with and without an
/// else (a call in one arm, so the context values differ at the merge), a block of a type index with a
/// br_if, nested blocks whose br_table reaches the function and whose lists both name one local, a loop of
/// a type index with a br_if, a loop a br_table reaches, a return, and a block whose end no path reaches.
ModuleBuilder everyConstruct()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { I32 }, .results = {} });
    builder.type(FuncType { .params = { I32 }, .results = { I32 } });
    builder.function(0,
                     { I32, I32 },
                     Bytes {
                         0x20, 0x00, 0x04, 0x40,                         // local.get 0; if
                         0x20, 0x00, 0x21, 0x01,                         //   local.set 1
                         0x20, 0x00, 0x10, 0x01, 0x1A,                   //   call 1; drop
                         0x05, 0x41, 0x02, 0x21, 0x01, 0x0B,             // else local.set 1 end
                         0x20, 0x00, 0x04, 0x40, 0x41, 0x03, 0x21, 0x02, // if local.set 2
                         0x0B,                                           // end
                         0x41, 0x07, 0x02, 0x01,                         // block (type 1)
                         0x21, 0x02, 0x20, 0x02, 0x20, 0x00, 0x0D, 0x00, //   br_if 0
                         0x1A, 0x20, 0x01, 0x0B, 0x1A,                   // end; drop
                         0x02, 0x40, 0x20, 0x00, 0x21, 0x02,             // block local.set 2
                         0x02, 0x40, 0x20, 0x01, 0x21, 0x02,             //   block local.set 2
                         0x20, 0x00, 0x0E, 0x02, 0x00, 0x01, 0x02, 0x0B, //     br_table 0 1 2 end
                         0x20, 0x00, 0x0D, 0x00, 0x0C, 0x00, 0x0B,       //   br_if 0; br 0 end
                         0x41, 0x03, 0x03, 0x01,                         // loop (type 1)
                         0x20, 0x00, 0x21, 0x01, 0x20, 0x00, 0x0D, 0x00, //   br_if 0
                         0x0B, 0x1A,                                     // end; drop
                         0x02, 0x40, 0x03, 0x40,                         // block loop
                         0x20, 0x00, 0x0E, 0x01, 0x00, 0x01, 0x0B, 0x0B, //   br_table 0 1 end end
                         0x20, 0x00, 0x04, 0x40, 0x0F, 0x0B,             // if return end
                         0x02, 0x40, 0x00, 0x0B,                         // block unreachable end
                     });
    builder.function(1, {}, localGet(0));
    return builder;
}

} // namespace

TEST_CASE("Translator.refusesAtEveryCapItReaches")
{
    // Every cap swept from 0 up to the value the function needs: each run translates or refuses with the
    // cap's own code, the first refusal at each site of the body. The translation at the full caps verifies.
    auto const builder = everyConstruct();
    REQUIRE(translateOf(builder)->function.has_value());
    struct Sweep
    {
        CompileErrorCode code;
        std::function<void(CompileLimits&, uint64_t)> set;
        uint64_t last;
    };
    auto const sweeps = std::to_array<Sweep>({
        { CompileErrorCode::PoolTooLarge,
          [](CompileLimits& limits, uint64_t value) {
              limits.maxPoolEntries = static_cast<decltype(limits.maxPoolEntries)>(value);
          },
          1'400 },
        { CompileErrorCode::TooManyBranchArguments,
          [](CompileLimits& limits, uint64_t value) {
              limits.maxBranchArguments = static_cast<decltype(limits.maxBranchArguments)>(value);
          },
          200 },
        { CompileErrorCode::TooManyBlockParameters,
          [](CompileLimits& limits, uint64_t value) {
              limits.maxBlockParameters = static_cast<decltype(limits.maxBlockParameters)>(value);
          },
          20 },
        { CompileErrorCode::TooManyInstructions,
          [](CompileLimits& limits, uint64_t value) {
              limits.maxFunctionInstructions = static_cast<decltype(limits.maxFunctionInstructions)>(value);
          },
          300 },
    });
    for (auto const& sweep: sweeps)
    {
        auto translated = 0;
        for (auto const value: std::views::iota(uint64_t { 0 }, sweep.last + 1))
        {
            auto limits = CompileLimits {};
            sweep.set(limits, value);
            auto const result = translateOf(builder, Options { .limits = limits });
            if (result->function)
            {
                ++translated;
                continue;
            }
            INFO("cap " << value);
            CHECK(result->error->code == sweep.code);
        }
        // The largest values translate.
        CHECK(translated > 0);
    }
}

TEST_CASE("Translator.deferredTypesAreRefusedWhereTheyAppear")
{
    // A v128 or a reference to a concrete type at each position translation meets one: refused
    // UnsupportedFeature, never translated as a word it cannot represent.
    auto const typed =
        ValueType { .kind = isoline::binary::ValueKind::Ref,
                    .nullable = isoline::binary::Nullable::Yes,
                    .heap = { .kind = isoline::binary::AbstractHeapType::Concrete, .typeIndex = 0 } };
    struct Case
    {
        std::string_view name;
        std::function<ModuleBuilder()> build;
    };
    auto const cases = std::vector<Case> {
        { "a v128 parameter",
          [&] {
              return oneFunction(FuncType { .params = { isoline::binary::V128 }, .results = {} }, {}, {});
          } },
        { "a v128 result",
          [&] {
              return oneFunction(
                  FuncType { .params = {}, .results = { isoline::binary::V128 } }, {}, { 0x00 });
          } },
        { "an opcode of a deferred feature",
          [&] { return oneFunction(FuncType { .params = {}, .results = {} }, {}, { 0x12, 0x00 }); } },
        { "a v128 global",
          [&] {
              auto builder = oneFunction(FuncType { .params = {}, .results = {} }, {}, { 0x23, 0x00, 0x1A });
              builder.global(isoline::binary::GlobalType { .type = isoline::binary::V128 },
                             code({ { 0xFD, 0x0C }, Bytes(16, 0x00) }));
              return builder;
          } },
        { "global.set of a typed global",
          [&] {
              auto builder =
                  oneFunction(FuncType { .params = {}, .results = {} }, {}, { 0xD2, 0x00, 0x24, 0x00 });
              builder.global(isoline::binary::GlobalType { .type = typed,
                                                           .mutability = isoline::binary::Mutability::Var },
                             { 0xD0, 0x00 });
              builder.element({ 0x03, 0x00, 0x01, 0x00 });
              return builder;
          } },
        { "ref.null of a concrete type",
          [&] { return oneFunction(FuncType { .params = {}, .results = {} }, {}, { 0xD0, 0x00, 0x1A }); } },
        { "a call of a v128 result",
          [&] {
              auto builder = ModuleBuilder {};
              builder.type(FuncType { .params = {}, .results = {} });
              builder.type(FuncType { .params = {}, .results = { isoline::binary::V128 } });
              builder.function(0, {}, { 0x10, 0x01, 0x1A }).function(1, {}, { 0x00 });
              return builder;
          } },
        { "a call of a typed parameter",
          [&] {
              auto builder = ModuleBuilder {};
              builder.type(FuncType { .params = {}, .results = {} });
              builder.type(FuncType { .params = { typed }, .results = {} });
              builder.function(0, {}, { 0xD2, 0x00, 0x10, 0x01 }).function(1, {}, {});
              builder.element({ 0x03, 0x00, 0x01, 0x00 });
              return builder;
          } },
        { "a call_indirect of a v128 result",
          [&] {
              auto builder = ModuleBuilder {};
              builder.type(FuncType { .params = {}, .results = {} });
              builder.type(FuncType { .params = {}, .results = { isoline::binary::V128 } });
              builder.function(0, {}, { 0x41, 0x00, 0x11, 0x01, 0x00, 0x1A });
              builder.table(
                  isoline::binary::TableType { .limits = Limits { .min = 1, .max = std::nullopt } });
              return builder;
          } },
        { "a call_indirect of a typed parameter",
          [&] {
              auto builder = ModuleBuilder {};
              builder.type(FuncType { .params = {}, .results = {} });
              builder.type(FuncType { .params = { typed }, .results = {} });
              builder.function(0, {}, { 0xD2, 0x00, 0x41, 0x00, 0x11, 0x01, 0x00 });
              builder.table(
                  isoline::binary::TableType { .limits = Limits { .min = 1, .max = std::nullopt } });
              builder.element({ 0x03, 0x00, 0x01, 0x00 });
              return builder;
          } },
        { "a block of a v128 result",
          [&] {
              return oneFunction(
                  FuncType { .params = {}, .results = {} }, {}, { 0x02, 0x7B, 0x00, 0x0B, 0x1A });
          } },
        { "a block of a typed parameter",
          [&] {
              auto builder = ModuleBuilder {};
              builder.type(FuncType { .params = {}, .results = {} });
              builder.type(FuncType { .params = { typed }, .results = {} });
              builder.function(0, {}, { 0xD2, 0x00, 0x02, 0x01, 0x1A, 0x0B });
              builder.element({ 0x03, 0x00, 0x01, 0x00 });
              return builder;
          } },
    };
    for (auto const& entry: cases)
    {
        INFO(entry.name);
        auto const translated = translateOf(entry.build());
        CHECK(refusalOf(*translated).code == CompileErrorCode::UnsupportedFeature);
    }
}

TEST_CASE("Translator.hintsOfDeadCodeAreSkipped")
{
    // block { br 0; local.get 0; if nop end } local.get 0; if nop end: the first if is dead code, never
    // asked for its hint, so the second's lookup steps over it. Its unlikely hint makes its then arm cold.
    auto builder = oneFunction(FuncType { .params = { I32 }, .results = {} },
                               {},
                               { 0x02,
                                 0x40,
                                 0x0C,
                                 0x00,
                                 0x20,
                                 0x00,
                                 0x04,
                                 0x40,
                                 0x01,
                                 0x0B,
                                 0x0B,
                                 0x20,
                                 0x00,
                                 0x04,
                                 0x40,
                                 0x01,
                                 0x0B });
    auto section = encodeName(isoline::binary::BranchHintSectionName);
    section.insert(section.end(), { 0x01, 0x00, 0x02, 0x07, 0x01, 0x00, 0x0E, 0x01, 0x00 });
    builder.rawSection(0, std::move(section));
    auto const translated = translateOf(builder);
    REQUIRE(translated->module->module().branchHints.size() == 2);
    auto cold = 0;
    for (auto const block: std::views::iota(uint32_t { 0 }, translated->function->blockCount()))
        cold += translated->function->block(BlockId { block }).hotness == Hotness::Cold ? 1 : 0;
    CHECK(cold == 1);
}

TEST_CASE("Translator.truncationBoundsOnlyForTruncations")
{
    using isoline::compiler::translation::truncationBoundsOf;
    CHECK(truncationBoundsOf(Operation::I32TruncF32S).has_value());
    CHECK_FALSE(truncationBoundsOf(Operation::I32TruncSatF32S).has_value());
    CHECK_FALSE(truncationBoundsOf(Operation::I32Add).has_value());
}

TEST_CASE("Translator.wideConstructsReserveBeforeTheyEmit")
{
    // Constructs whose emission is wide where nothing before them emits much: the end of an if without an
    // else and a br out of a block, each of 1'000 values (pushed by local.get, which emits nothing), a
    // function's end of 1'000 results, and a call of 1'000 arguments after which nothing reserves (an
    // unreachable follows). Each reserves its own width, so a pool cap swept by one up to what the function
    // needs refuses at that construct first, just below the smallest cap that translates.
    constexpr auto Width = uint32_t { 1'000 };
    auto pushEvery = Bytes {};
    for ([[maybe_unused]] auto const value: std::views::iota(uint32_t { 0 }, Width))
        pushEvery.insert(pushEvery.end(), { 0x20, 0x00 });
    auto dropEvery = Bytes(Width, 0x1A);
    // The then arm modifies some locals, so that its end passes more than the else arm's: its reservation
    // is then the widest of the function.
    constexpr auto Modified = uint32_t { 50 };
    auto setSome = Bytes {};
    for (auto const local: std::views::iota(uint32_t { 1 }, Modified + 1))
        setSome = code({ setSome, localGet(0), localSet(local) });
    // Type 0 takes the i32 every function does, type 1 is Width values to Width values.
    auto const wide = [&](Bytes const& body, std::vector<ValueType> results = {}) {
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = { I32 }, .results = std::move(results) });
        builder.type(FuncType { .params = std::vector(Width, I32), .results = std::vector(Width, I32) });
        builder.function(0, std::vector(Modified, I32), body);
        return builder;
    };
    auto callee = ModuleBuilder {};
    callee.type(FuncType { .params = { I32 }, .results = {} });
    callee.type(FuncType { .params = std::vector(Width, I32), .results = {} });
    callee.function(0, {}, code({ pushEvery, call(1), unreachable() }));
    callee.function(1, {}, {});
    auto const builders = std::to_array<ModuleBuilder>({
        wide(code({ pushEvery, localGet(0), { 0x04, 0x01 }, setSome, { 0x0B }, dropEvery })),
        wide(code({ pushEvery, { 0x02, 0x01 }, br(0), { 0x0B }, dropEvery })),
        wide(pushEvery, std::vector(Width, I32)),
        callee,
    });
    for (auto const& builder: builders)
    {
        auto const translatesAt = [&builder](uint64_t cap) {
            auto limits = CompileLimits {};
            limits.maxPoolEntries = cap;
            auto const result = translateOf(builder, Options { .limits = limits });
            if (!result->function)
                CHECK(result->error->code == CompileErrorCode::PoolTooLarge);
            return result->function.has_value();
        };
        // The smallest cap that translates, by bisection, then every cap of the 64 below it refuses.
        auto low = uint64_t { 0 };
        auto high = uint64_t { 1 } << 16U;
        REQUIRE(translatesAt(high));
        while (low + 1 < high)
        {
            auto const middle = low + ((high - low) / 2);
            (translatesAt(middle) ? high : low) = middle;
        }
        CHECK(high > Width);
        for (auto const cap: std::views::iota(high - 64, high))
            CHECK_FALSE(translatesAt(cap));
    }
}
