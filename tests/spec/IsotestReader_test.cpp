// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "IsotestReader.hpp"
#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>

using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Value;
using isoline::spectest::ActionKind;
using isoline::spectest::CommandKind;
using isoline::spectest::ExpectedValue;
using isoline::spectest::parseIsotest;

namespace
{

ExpectedValue exactly(Value value)
{
    return ExpectedValue { .pattern = ExpectedValue::Pattern::Exact, .value = value };
}

ExpectedValue pattern(ExpectedValue::Pattern kind)
{
    return ExpectedValue { .pattern = kind, .value = {} };
}

constexpr auto ExternHeap = isoline::binary::HeapType { .kind = isoline::binary::AbstractHeapType::Extern };
constexpr auto FuncHeap = isoline::binary::HeapType { .kind = isoline::binary::AbstractHeapType::Func };

} // namespace

TEST_CASE("IsotestReader.parsesAssertReturn")
{
    auto const script = parseIsotest("assert_return invoke 616464 - i32:1 i32:2 -> i32:3\n");
    REQUIRE(script.has_value());
    REQUIRE(script->size() == 1);
    auto const& command = script->front();
    CHECK(command.kind == CommandKind::AssertReturn);
    CHECK(command.line == 1);
    CHECK(command.action.kind == ActionKind::Invoke);
    CHECK(command.action.field == "add");
    CHECK_FALSE(command.action.module.has_value());
    CHECK(command.action.args == std::vector<Value> { I32 { 1 }, I32 { 2 } });
    CHECK(command.expected == std::vector { exactly(I32 { 3 }) });
}

TEST_CASE("IsotestReader.parsesNanPatterns")
{
    auto const script = parseIsotest("assert_return invoke 66 - -> f32:nan:canonical f32:nan:arithmetic "
                                     "f64:nan:canonical f64:nan:arithmetic\n");
    REQUIRE(script.has_value());
    REQUIRE(script->size() == 1);
    CHECK(script->front().expected
          == std::vector { pattern(ExpectedValue::Pattern::CanonicalNan32),
                           pattern(ExpectedValue::Pattern::ArithmeticNan32),
                           pattern(ExpectedValue::Pattern::CanonicalNan64),
                           pattern(ExpectedValue::Pattern::ArithmeticNan64) });
}

TEST_CASE("IsotestReader.rejectsGarbage")
{
    auto const script = parseIsotest("frobnicate\n");
    REQUIRE_FALSE(script.has_value());
    CHECK(script.error().line == 1);
}

TEST_CASE("IsotestReader.parsesEveryValueForm")
{
    auto const script = parseIsotest("invoke 66 4d f64:3ff0000000000000 i64:ffffffffffffffff f32:7fc00000 "
                                     "ref.null:func ref.null:extern ref.extern:7 ref.func:2 ->\n"
                                     "assert_return get 67 - -> ref.func\n");
    REQUIRE(script.has_value());
    REQUIRE(script->size() == 2);
    auto const& invoke = script->front();
    CHECK(invoke.kind == CommandKind::Invoke);
    CHECK(invoke.action.module == std::string { "M" });
    CHECK(invoke.action.args
          == std::vector<Value> {
              F64 { 0x3ff0'0000'0000'0000 },
              I64 { 0xffff'ffff'ffff'ffff },
              F32 { 0x7fc0'0000 },
              Ref { .kind = RefKind::Null, .heap = FuncHeap, .address = 0 },
              Ref { .kind = RefKind::Null, .heap = ExternHeap, .address = 0 },
              Ref { .kind = RefKind::Extern, .heap = ExternHeap, .address = 7 },
              Ref { .kind = RefKind::Func, .heap = FuncHeap, .address = 2 },
          });
    auto const& get = script->back();
    CHECK(get.line == 2);
    CHECK(get.action.kind == ActionKind::Get);
    CHECK(get.expected == std::vector { pattern(ExpectedValue::Pattern::AnyFunctionRef) });
}

TEST_CASE("IsotestReader.parsesModuleCommands")
{
    auto const script = parseIsotest("module a.0.wasm 4d\n"
                                     "register 6d 4d\n"
                                     "register 6e\n"
                                     "\n"
                                     "assert_trap invoke 66 - -> 756e726561636861626c65\n"
                                     "assert_trap module a.1.wasm 6f6f62\n"
                                     "assert_exhaustion invoke  - -> 63\n"
                                     "assert_invalid a.2.wasm 74\n"
                                     "assert_malformed a.3.wasm 6d\n"
                                     "assert_unlinkable a.4.wasm 75\n"
                                     "assert_uninstantiable a.5.wasm 7a\n"
                                     "skip 74657874\n");
    REQUIRE(script.has_value());
    REQUIRE(script->size() == 11);
    auto const& commands = *script;
    CHECK(commands[0].kind == CommandKind::Module);
    CHECK(commands[0].file == "a.0.wasm");
    CHECK(commands[0].name == std::string { "M" });
    CHECK(commands[1].kind == CommandKind::Register);
    CHECK(commands[1].text == "m");
    CHECK(commands[1].name == std::string { "M" });
    CHECK_FALSE(commands[2].name.has_value());
    CHECK(commands[3].kind == CommandKind::AssertTrap);
    CHECK(commands[3].line == 5);
    CHECK(commands[3].text == "unreachable");
    CHECK(commands[4].kind == CommandKind::AssertTrapModule);
    CHECK(commands[4].file == "a.1.wasm");
    CHECK(commands[4].text == "oob");
    CHECK(commands[5].kind == CommandKind::AssertExhaustion);
    CHECK(commands[5].action.field.empty()); // the empty name: the empty token between two spaces
    CHECK(commands[6].kind == CommandKind::AssertInvalid);
    CHECK(commands[7].kind == CommandKind::AssertMalformed);
    CHECK(commands[8].kind == CommandKind::AssertUnlinkable);
    CHECK(commands[9].kind == CommandKind::AssertUninstantiable);
    CHECK(commands[10].kind == CommandKind::Skip);
    CHECK(commands[10].text == "text");
}

TEST_CASE("IsotestReader.rejectsMalformedLines")
{
    for (auto const* line: {
             "module\n",                                     // no file
             "module a.wasm 4d extra\n",                     // too many tokens
             "assert_return invoke 6 - -> i32:1\n",          // odd hex
             "assert_return invoke 66 - i32:1\n",            // no arrow
             "assert_return invoke 66 - -> i32:xyz\n",       // bad bits
             "assert_return invoke 66 - -> i32:100000000\n", // too wide
             "assert_return call 66 - ->\n",                 // unknown action
             "assert_trap invoke 66 - -> 61 62\n",           // two messages
             "invoke 66 - ref.func ->\n",                    // a pattern is no argument
             "skip\n",                                       // no reason
         })
    {
        INFO(line);
        auto const script = parseIsotest(std::string { "skip 61\n" } + line);
        REQUIRE_FALSE(script.has_value());
        CHECK(script.error().line == 2);
    }
}
