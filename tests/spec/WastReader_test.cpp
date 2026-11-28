// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "WastReader.hpp"
#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/text/TextLimits.hpp>

using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::V128;
using isoline::runtime::Value;
using isoline::spectest::ActionKind;
using isoline::spectest::Command;
using isoline::spectest::CommandKind;
using isoline::spectest::ExpectedValue;
using isoline::spectest::LaneShape;
using isoline::spectest::ModuleForm;
using isoline::spectest::parseWast;
using isoline::text::TextLimits;
using Pattern = ExpectedValue::Pattern;

namespace
{

/// The commands of @p script, which must parse.
std::vector<Command> commandsOf(std::string_view script)
{
    auto commands = parseWast(script, TextLimits {});
    if (!commands)
        FAIL(commands.error().line << ": " << commands.error().message);
    return std::move(*commands);
}

/// The error @p script is refused with, which must be one.
isoline::spectest::ParseError errorOf(std::string_view script)
{
    auto const commands = parseWast(script, TextLimits {});
    REQUIRE_FALSE(commands.has_value());
    return commands.error();
}

ExpectedValue exactly(Value value)
{
    return ExpectedValue { .pattern = Pattern::Exact, .value = value };
}

ExpectedValue pattern(Pattern kind)
{
    return ExpectedValue { .pattern = kind, .value = {} };
}

/// The expected values of a script holding one assert_return.
std::vector<ExpectedValue> expectedOf(std::string_view results)
{
    auto const commands = commandsOf("(assert_return (invoke \"f\") " + std::string { results } + ")");
    REQUIRE(commands.size() == 1);
    return commands[0].expected;
}

/// The arguments of a script holding one invoke.
std::vector<Value> argumentsOf(std::string_view arguments)
{
    auto const commands = commandsOf("(invoke \"f\" " + std::string { arguments } + ")");
    REQUIRE(commands.size() == 1);
    return commands[0].action.args;
}

constexpr auto FuncHeap = isoline::binary::HeapType { .kind = isoline::binary::AbstractHeapType::Func };
constexpr auto ExternHeap = isoline::binary::HeapType { .kind = isoline::binary::AbstractHeapType::Extern };

} // namespace

TEST_CASE("WastReader.commands")
{
    auto const commands =
        commandsOf("(module $m (func))\n(assert_return (invoke $m \"f\" (i32.const 1)) (i32.const 2))");
    REQUIRE(commands.size() == 2);
    CHECK(commands[0].kind == CommandKind::Module);
    CHECK(commands[0].line == 1);
    CHECK(commands[0].name == "m");
    CHECK(commands[0].source.form == ModuleForm::Text);
    CHECK(commands[0].source.text == "(module $m (func))");
    CHECK(commands[1].kind == CommandKind::AssertReturn);
    CHECK(commands[1].line == 2);
    CHECK(commands[1].action.kind == ActionKind::Invoke);
    CHECK(commands[1].action.module == "m");
    CHECK(commands[1].action.field == "f");
    CHECK(commands[1].action.args == std::vector<Value> { I32 { 1 } });
    CHECK(commands[1].expected == std::vector { exactly(I32 { 2 }) });
}

TEST_CASE("WastReader.multiLineCommands")
{
    // A command's line is that of its own `(`, wherever its action or module is.
    auto const commands = commandsOf("(assert_return\n"
                                     "  (invoke \"f\")\n"
                                     "  (i32.const 1))\n"
                                     "(assert_invalid\n"
                                     "\n"
                                     "  (module (func (result i32)))\n"
                                     "  \"type mismatch\")");
    REQUIRE(commands.size() == 2);
    CHECK(commands[0].kind == CommandKind::AssertReturn);
    CHECK(commands[0].line == 1);
    CHECK(commands[1].kind == CommandKind::AssertInvalid);
    CHECK(commands[1].line == 4);
    CHECK(commands[1].text == "type mismatch");
    CHECK(commands[1].source.text == "(module (func (result i32)))");
    // The module's own position, which shifts a text error's into the script.
    CHECK(commands[1].source.position.line == 6);
    CHECK(commands[1].source.position.column == 3);
}

TEST_CASE("WastReader.crLfLineEnds")
{
    // A script Git checked out with CR LF reads as its LF original: a CR LF ends one line, so the lines
    // the runner reports are the editor's, and a comment ends at the CR.
    auto const commands = commandsOf("(assert_return ;; a comment\r\n"
                                     "  (invoke \"f\")\r\n"
                                     "  (i32.const 1))\r\n"
                                     "\r\n"
                                     "(assert_invalid\r\n"
                                     "  (module (func (result i32)))\r\n"
                                     "  \"type mismatch\")\r\n");
    REQUIRE(commands.size() == 2);
    CHECK(commands[0].kind == CommandKind::AssertReturn);
    CHECK(commands[0].line == 1);
    CHECK(commands[0].expected == std::vector { exactly(I32 { 1 }) });
    CHECK(commands[1].kind == CommandKind::AssertInvalid);
    CHECK(commands[1].line == 5);
    CHECK(commands[1].text == "type mismatch");
    CHECK(commands[1].source.text == "(module (func (result i32)))");
    CHECK(commands[1].source.position.line == 6);
    CHECK(commands[1].source.position.column == 3);
}

TEST_CASE("WastReader.twoCommandsOnOneLine")
{
    auto const commands = commandsOf(R"((assert_return (invoke "a")) (assert_return (invoke "b")))");
    REQUIRE(commands.size() == 2);
    CHECK(commands[0].action.field == "a");
    CHECK(commands[0].line == 1);
    CHECK(commands[1].action.field == "b");
    CHECK(commands[1].line == 1);
}

TEST_CASE("WastReader.binaryAndQuote")
{
    auto const commands = commandsOf("(module binary \"\\00asm\" \"\\01\\00\\00\\00\")\n"
                                     "(module $q quote \"(func)\" \" (memory 1)\")");
    REQUIRE(commands.size() == 2);
    CHECK(commands[0].source.form == ModuleForm::Binary);
    CHECK(commands[0].source.text == std::string { "\0asm\1\0\0\0", 8 });
    CHECK(commands[1].source.form == ModuleForm::Quote);
    CHECK(commands[1].source.text == "(func) (memory 1)");
    CHECK(commands[1].name == "q");
    CHECK(commands[1].source.position.line == 2);
}

TEST_CASE("WastReader.v128Patterns")
{
    auto const expected = expectedOf("(v128.const f32x4 nan:canonical 1 nan:arithmetic 0)");
    REQUIRE(expected.size() == 1);
    CHECK(expected[0].pattern == Pattern::Lanes);
    CHECK(expected[0].laneShape == LaneShape::F32x4);
    CHECK(
        expected[0].lanes
        == std::array { Pattern::CanonicalNan32, Pattern::Exact, Pattern::ArithmeticNan32, Pattern::Exact });
    CHECK(expected[0].value == Value { V128 { .lanes = { 0x3f80'0000'0000'0000, 0 } } });

    auto const doubles = expectedOf("(v128.const f64x2 -0 nan:arithmetic)");
    REQUIRE(doubles.size() == 1);
    CHECK(doubles[0].pattern == Pattern::Lanes);
    CHECK(doubles[0].laneShape == LaneShape::F64x2);
    CHECK(doubles[0].lanes.at(0) == Pattern::Exact);
    CHECK(doubles[0].lanes.at(1) == Pattern::ArithmeticNan64);
    CHECK(doubles[0].value == Value { V128 { .lanes = { 0x8000'0000'0000'0000, 0 } } });
}

TEST_CASE("WastReader.v128Exact")
{
    // Without a NaN pattern a v128 is exactly its bits, lane 0 the lowest, a lane's integer truncated
    // to the lane's width.
    CHECK(expectedOf("(v128.const i8x16 -1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0x80)")
          == std::vector { exactly(V128 { .lanes = { 0xff, 0x8000'0000'0000'0000 } }) });
    CHECK(expectedOf("(v128.const i16x8 1 2 3 4 5 6 7 8)")
          == std::vector { exactly(V128 { .lanes = { 0x0004'0003'0002'0001, 0x0008'0007'0006'0005 } }) });
    CHECK(argumentsOf("(v128.const i32x4 1 2 3 -1)")
          == std::vector<Value> { V128 { .lanes = { 0x0000'0002'0000'0001, 0xffff'ffff'0000'0003 } } });
    CHECK(argumentsOf("(v128.const i64x2 1 -2)")
          == std::vector<Value> { V128 { .lanes = { 1, 0xffff'ffff'ffff'fffe } } });
    CHECK(argumentsOf("(v128.const f32x4 1 0 0 -0)")
          == std::vector<Value> { V128 { .lanes = { 0x3f80'0000, 0x8000'0000'0000'0000 } } });
}

TEST_CASE("WastReader.scalarValues")
{
    CHECK(argumentsOf("(i32.const -1) (i64.const 0x8000_0000_0000_0000) (f32.const 1.5) (f64.const -inf)")
          == std::vector<Value> { I32 { 0xffff'ffff },
                                  I64 { 0x8000'0000'0000'0000 },
                                  F32 { 0x3fc0'0000 },
                                  F64 { 0xfff0'0000'0000'0000 } });
    CHECK(expectedOf("(f32.const nan:canonical) (f32.const nan:arithmetic) (f64.const nan:canonical) "
                     "(f64.const nan:arithmetic) (f32.const nan:0x200000)")
          == std::vector { pattern(Pattern::CanonicalNan32),
                           pattern(Pattern::ArithmeticNan32),
                           pattern(Pattern::CanonicalNan64),
                           pattern(Pattern::ArithmeticNan64),
                           exactly(F32 { 0x7fa0'0000 }) });
}

TEST_CASE("WastReader.referenceValues")
{
    auto const nullOf = [](isoline::binary::AbstractHeapType heap) {
        return Ref { .kind = RefKind::Null, .heap = { .kind = heap }, .address = 0 };
    };
    using isoline::binary::AbstractHeapType;
    CHECK(
        argumentsOf("(ref.null func) (ref.null nofunc) (ref.null extern) (ref.null noextern) (ref.null exn) "
                    "(ref.null noexn) (ref.extern 3) (ref.func 4)")
        == std::vector<Value> { nullOf(AbstractHeapType::Func),
                                nullOf(AbstractHeapType::NoFunc),
                                nullOf(AbstractHeapType::Extern),
                                nullOf(AbstractHeapType::NoExtern),
                                nullOf(AbstractHeapType::Exn),
                                nullOf(AbstractHeapType::NoExn),
                                Ref { .kind = RefKind::Extern, .heap = ExternHeap, .address = 3 },
                                Ref { .kind = RefKind::Func, .heap = FuncHeap, .address = 4 } });
    CHECK(expectedOf("(ref.null) (ref.func) (ref.extern) (ref.exn)")
          == std::vector { pattern(Pattern::AnyNullRef),
                           pattern(Pattern::AnyFunctionRef),
                           pattern(Pattern::AnyExternRef),
                           pattern(Pattern::AnyExceptionRef) });
}

TEST_CASE("WastReader.either")
{
    auto const expected = expectedOf("(either (i32.const 1) (f32.const nan:canonical))");
    REQUIRE(expected.size() == 1);
    CHECK(expected[0].pattern == Pattern::Either);
    CHECK(expected[0].alternatives == std::vector { exactly(I32 { 1 }), pattern(Pattern::CanonicalNan32) });
}

TEST_CASE("WastReader.malformedCustom")
{
    auto const commands = commandsOf("(assert_malformed_custom (module quote \"(@custom)\") \"@custom "
                                     "annotation: missing section name\")");
    REQUIRE(commands.size() == 1);
    CHECK(commands[0].kind == CommandKind::AssertMalformedCustom);
    CHECK(commands[0].source.form == ModuleForm::Quote);
    CHECK(commands[0].source.text == "(@custom)");
    CHECK(commands[0].text == "@custom annotation: missing section name");
}

TEST_CASE("WastReader.everyCommand")
{
    auto const commands =
        commandsOf("(module definition $d (func))\n"
                   "(module instance $i $d)\n"
                   "(module instance $d)\n"
                   "(register \"r\" $i)\n"
                   "(register \"s\")\n"
                   "(get $i \"g\")\n"
                   "(assert_trap (invoke \"t\") \"unreachable\")\n"
                   "(assert_trap (module (start 0)) \"out of bounds\")\n"
                   "(assert_exhaustion (invoke \"e\") \"call stack exhausted\")\n"
                   "(assert_exception (invoke \"x\" (i32.const 1)))\n"
                   "(assert_malformed (module binary \"\") \"unexpected end\")\n"
                   "(assert_unlinkable (module (import \"a\" \"b\" (func))) \"unknown import\")\n"
                   "(assert_invalid_custom (module) \"x\")\n"
                   "(assert_return (get \"g\") (i32.const 0))");
    auto const kinds = commands | std::views::transform(&Command::kind);
    CHECK(std::ranges::equal(kinds,
                             std::array { CommandKind::ModuleDefinition,
                                          CommandKind::ModuleInstance,
                                          CommandKind::ModuleInstance,
                                          CommandKind::Register,
                                          CommandKind::Register,
                                          CommandKind::Invoke,
                                          CommandKind::AssertTrap,
                                          CommandKind::AssertTrapModule,
                                          CommandKind::AssertExhaustion,
                                          CommandKind::AssertException,
                                          CommandKind::AssertMalformed,
                                          CommandKind::AssertUnlinkable,
                                          CommandKind::AssertInvalidCustom,
                                          CommandKind::AssertReturn }));
    REQUIRE(commands.size() == 14);
    CHECK(commands[0].name == "d");
    CHECK(commands[1].name == "i");
    CHECK(commands[1].definition == "d");
    CHECK_FALSE(commands[2].name.has_value());
    CHECK(commands[2].definition == "d");
    CHECK(commands[3].text == "r");
    CHECK(commands[3].name == "i");
    CHECK_FALSE(commands[4].name.has_value());
    CHECK(commands[5].action.kind == ActionKind::Get);
    CHECK(commands[5].action.module == "i");
    CHECK(commands[6].text == "unreachable");
    CHECK(commands[7].source.text == "(module (start 0))");
    CHECK(commands[7].text == "out of bounds");
    CHECK(commands[9].action.args == std::vector<Value> { I32 { 1 } });
    CHECK(commands[13].action.kind == ActionKind::Get);
}

TEST_CASE("WastReader.annotationsAreIgnored")
{
    // Between commands and inside a command's own list; a module's stay in its text.
    auto const commands = commandsOf(R"((@a top) (register (@b) "r") (module (@custom "c" "") (func)))");
    REQUIRE(commands.size() == 2);
    CHECK(commands[0].text == "r");
    CHECK(commands[1].source.text == "(module (@custom \"c\" \"\") (func))");
}

TEST_CASE("WastReader.inlineModule")
{
    // A script of bare module fields is one module, its text the whole script.
    constexpr auto Script = std::string_view { ";; fields\n(func) (memory 0)" };
    auto const commands = commandsOf(Script);
    REQUIRE(commands.size() == 1);
    CHECK(commands[0].kind == CommandKind::Module);
    CHECK(commands[0].line == 2);
    CHECK(commands[0].source.form == ModuleForm::Text);
    CHECK(commands[0].source.text == Script);
    CHECK(commands[0].source.position.line == 1);
    // A script of nothing holds no command.
    CHECK(commandsOf(" ;; nothing\n").empty());
}

TEST_CASE("WastReader.namesAreDecoded")
{
    // Strings and identifiers mean what they denote, escapes and quotes resolved.
    auto const commands = commandsOf(R"((register "a\62" $"c d") (invoke "\u{e9}"))");
    REQUIRE(commands.size() == 2);
    CHECK(commands[0].text == "ab");
    CHECK(commands[0].name == "c d");
    CHECK(commands[1].action.field == "\xc3\xa9");
}

TEST_CASE("WastReader.refusesMalformedScripts")
{
    struct Malformed
    {
        std::string_view script;
        std::size_t line;
        std::string_view message;
    };
    for (auto const& malformed: std::to_array<Malformed>({
             { .script = "(module", .line = 1, .message = "unexpected token" },
             { .script = "\nfoo", .line = 2, .message = "unknown command 'foo'" },
             { .script = "(module)\n(script)", .line = 2, .message = "unknown command '(…)'" },
             { .script = "()", .line = 1, .message = "unknown command '(…)'" },
             { .script = "(register)", .line = 1, .message = "expected a name in 'register'" },
             { .script = R"((register "a" "b"))",
               .line = 1,
               .message = "expected ')' in 'register', got '\"b\"'" },
             { .script = R"((register "\ff"))", .line = 1, .message = "malformed UTF-8 encoding" },
             { .script = "(module instance)",
               .line = 1,
               .message = "a module instance must name its definition" },
             { .script = "(module instance $a $b $c)",
               .line = 1,
               .message = "expected ')' in 'module', got '$c'" },
             { .script = R"((module $"\ff"))", .line = 1, .message = "malformed UTF-8 encoding" },
             { .script = R"((module instance $"\ff"))", .line = 1, .message = "malformed UTF-8 encoding" },
             { .script = R"((module instance $a $"\ff"))", .line = 1, .message = "malformed UTF-8 encoding" },
             { .script = "(module binary \"\" x)",
               .line = 1,
               .message = "expected a string in 'module', got 'x'" },
             { .script = R"((assert_invalid (invoke "f") "m"))", .line = 1, .message = "expected a module" },
             { .script = "(assert_invalid (module instance $d) \"m\")",
               .line = 1,
               .message = "an assertion on a module instance is not supported" },
             { .script = "(assert_invalid (module binary x) \"m\")",
               .line = 1,
               .message = "expected a string" },
             { .script = "(assert_invalid (module))",
               .line = 1,
               .message = "expected a string in 'assert_invalid'" },
             { .script = "(assert_trap (module) \"m\" x)", .line = 1, .message = "expected ')'" },
             { .script = "(assert_trap (module instance $d) \"m\")",
               .line = 1,
               .message = "an assertion on a module instance is not supported" },
             { .script = "(assert_return (module))", .line = 1, .message = "expected an action" },
             { .script = "(assert_return\n(invoke $\"\\ff\" \"f\"))",
               .line = 2,
               .message = "malformed UTF-8 encoding" },
             { .script = "(assert_return (invoke \"f\" 1))",
               .line = 1,
               .message = "expected a value in 'invoke', got '1'" },
             { .script = "(assert_return (invoke \"f\" (i32.add)))",
               .line = 1,
               .message = "expected a value" },
             { .script = "(assert_return (get \"g\" (i32.const 1)))",
               .line = 1,
               .message = "expected ')' in 'get'" },
             { .script = "(assert_return (invoke \"f\") (ref.i31))",
               .line = 1,
               .message = "expected a value" },
             { .script = "(assert_return (invoke \"f\") (ref.host 1))",
               .line = 1,
               .message = "expected a value" },
             { .script = "(assert_return (invoke \"f\") 7)", .line = 1, .message = "expected a value" },
             { .script = "(assert_trap (invoke \"f\"))", .line = 1, .message = "expected a string" },
             { .script = R"((assert_exhaustion (get "f") "m" "n"))", .line = 1, .message = "expected ')'" },
             { .script = R"((assert_exception (invoke "f") "m"))", .line = 1, .message = "expected ')'" },
             { .script = "(assert_exception (module))", .line = 1, .message = "expected an action" },
             // Values.
             { .script = "(invoke \"f\" (i32.const))",
               .line = 1,
               .message = "expected a number in 'i32.const'" },
             { .script = "(invoke \"f\" (i32.const 1 2))",
               .line = 1,
               .message = "expected ')' in 'i32.const'" },
             { .script = "(invoke \"f\" (i32.const 0x1_0000_0000))",
               .line = 1,
               .message = "constant out of range '0x1_0000_0000'" },
             { .script = "(invoke \"f\" (i32.const inf))", .line = 1, .message = "unknown operator 'inf'" },
             { .script = "(invoke \"f\" (i32.const $x))",
               .line = 1,
               .message = "expected a number in 'i32.const', got '$x'" },
             { .script = "(invoke \"f\" (f32.const 1e999))",
               .line = 1,
               .message = "constant out of range '1e999'" },
             { .script = "(invoke \"f\" (f64.const i32))", .line = 1, .message = "unknown operator 'i32'" },
             { .script = "(invoke \"f\" (f32.const nan:canonical))",
               .line = 1,
               .message = "a pattern is no argument: '(f32.const nan:canonical)'" },
             { .script = "(invoke \"f\" (f32.const nan:canonical 1))",
               .line = 1,
               .message = "a pattern is no argument" },
             { .script = "(assert_return (invoke \"f\") (f32.const nan:canonical 1))",
               .line = 1,
               .message = "expected ')' in 'f32.const', got '1'" },
             { .script = "(assert_return (invoke \"f\") (f64.const 1 2))",
               .line = 1,
               .message = "expected ')' in 'f64.const'" },
             { .script = "(invoke \"f\" (v128.const))",
               .line = 1,
               .message = "expected a v128 shape in 'v128.const'" },
             { .script = "(invoke \"f\" (v128.const i8x8 0))",
               .line = 1,
               .message = "expected a v128 shape" },
             { .script = R"((invoke "f" (v128.const "i8x16")))",
               .line = 1,
               .message = "expected a v128 shape" },
             { .script = "(invoke \"f\" (v128.const i64x2 0))",
               .line = 1,
               .message = "wrong number of lane literals" },
             { .script = "(invoke \"f\" (v128.const i64x2 0 0 0))",
               .line = 1,
               .message = "wrong number of lane literals" },
             { .script = "(invoke \"f\" (v128.const i8x16 256 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0))",
               .line = 1,
               .message = "constant out of range '256'" },
             { .script = "(invoke \"f\" (v128.const f64x2 0 x))",
               .line = 1,
               .message = "expected a number in 'v128.const', got 'x'" },
             { .script = "(invoke \"f\" (v128.const f32x4 0 0 0 nan:arithmetic))",
               .line = 1,
               .message = "a pattern is no argument" },
             { .script = "(invoke \"f\" (v128.const i32x4 0 0 0 nan:arithmetic))",
               .line = 1,
               .message = "unknown operator" },
             { .script = "(invoke \"f\" (ref.null))",
               .line = 1,
               .message = "a pattern is no argument: '(ref.null)'" },
             { .script = "(invoke \"f\" (ref.null any))",
               .line = 1,
               .message = "expected a heap type of func, extern or exn" },
             { .script = "(invoke \"f\" (ref.null $t))", .line = 1, .message = "expected a heap type" },
             { .script = "(invoke \"f\" (ref.null func extern))",
               .line = 1,
               .message = "expected ')' in 'ref.null'" },
             { .script = "(invoke \"f\" (ref.extern))", .line = 1, .message = "a pattern is no argument" },
             { .script = "(invoke \"f\" (ref.extern -1))", .line = 1, .message = "unknown operator '-1'" },
             { .script = "(invoke \"f\" (ref.func 0x1_0000_0000))",
               .line = 1,
               .message = "constant out of range" },
             { .script = "(invoke \"f\" (ref.func 1 2))",
               .line = 1,
               .message = "expected ')' in 'ref.func'" },
             { .script = "(invoke \"f\" (ref.exn))", .line = 1, .message = "a pattern is no argument" },
             { .script = "(assert_return (invoke \"f\") (ref.exn 1))",
               .line = 1,
               .message = "expected ')' in 'ref.exn'" },
             { .script = "(invoke \"f\" (either (i32.const 1)))",
               .line = 1,
               .message = "a pattern is no argument" },
             { .script = "(assert_return (invoke \"f\") (either))",
               .line = 1,
               .message = "expected a value in 'either'" },
             { .script = "(assert_return (invoke \"f\") (either (either (i32.const 1))))",
               .line = 1,
               .message = "an either within an either" },
             { .script = "(assert_return (invoke \"f\") (either (i32.const inf)))",
               .line = 1,
               .message = "unknown operator 'inf'" },
         }))
    {
        INFO(malformed.script);
        auto const error = errorOf(malformed.script);
        CHECK(error.line == malformed.line);
        INFO(error.message);
        CHECK(error.message.starts_with(malformed.message));
    }
}

TEST_CASE("WastReader.limitsApply")
{
    // The script is read under the text limits: its nesting is capped before any list is read.
    auto const deep = std::string(64, '(') + std::string(64, ')');
    auto const error = parseWast(deep, TextLimits { .maxNesting = 8 });
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().message == "nesting limit exceeded");
    auto const tooLong = parseWast("(invoke \"f\" (i32.const 12345))", TextLimits { .maxNumberLength = 4 });
    REQUIRE_FALSE(tooLong.has_value());
    CHECK(tooLong.error().message == "number too long '12345'");
}

TEST_CASE("WastReader.ordinalsMatchWasmTools")
{
    // ordinals.commands is what wasm-tools json-from-wast makes of ordinals.wast, `<ordinal> <type>`
    // per command: the reader must give one command per JSON command, in the same order.
    auto const directory = std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "spec" / "ordinals";
    auto const read = [](std::filesystem::path const& path) {
        auto text = core::readFileAsString(path);
        REQUIRE_FALSE(text.empty());
        return text;
    };
    // The JSON type each command kind is, as wasm-tools names it.
    constexpr auto JsonTypes = std::to_array<std::pair<CommandKind, std::string_view>>({
        { CommandKind::Module, "module" },
        { CommandKind::ModuleDefinition, "module_definition" },
        { CommandKind::ModuleInstance, "module_instance" },
        { CommandKind::Register, "register" },
        { CommandKind::Invoke, "action" },
        { CommandKind::AssertReturn, "assert_return" },
        { CommandKind::AssertTrap, "assert_trap" },
        { CommandKind::AssertTrapModule, "assert_uninstantiable" },
        { CommandKind::AssertExhaustion, "assert_exhaustion" },
        { CommandKind::AssertInvalid, "assert_invalid" },
        { CommandKind::AssertMalformed, "assert_malformed" },
        { CommandKind::AssertUnlinkable, "assert_unlinkable" },
        { CommandKind::AssertException, "assert_exception" },
        { CommandKind::AssertInvalidCustom, "assert_invalid_custom" },
        { CommandKind::AssertMalformedCustom, "assert_malformed_custom" },
    });
    auto const commands = commandsOf(read(directory / "ordinals.wast"));
    auto listed = std::string {};
    for (auto const [ordinal, command]: core::views::enumerate(commands))
    {
        auto const type =
            std::ranges::find(JsonTypes, command.kind, &std::pair<CommandKind, std::string_view>::first);
        REQUIRE(type != JsonTypes.end());
        listed += std::to_string(ordinal) + ' ' + std::string { type->second } + '\n';
    }
    CHECK(listed == read(directory / "ordinals.commands"));
}
