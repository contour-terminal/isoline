// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleEquality.hpp>
#include <isoline/binary/testing/OpcodeRowModules.hpp>
#include <isoline/text/FieldReader.hpp>
#include <isoline/text/InstructionReader.hpp>
#include <isoline/text/Instructions.hpp>
#include <isoline/text/ModuleReader.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>
#include <isoline/text/testing/ReadModule.hpp>
#include <isoline/validate/ValidationError.hpp>
#include <isoline/validate/Validator.hpp>

using isoline::FeatureSet;
using isoline::binary::AbstractHeapType;
using isoline::binary::AddressType;
using isoline::binary::ConstExpr;
using isoline::binary::ExternKind;
using isoline::binary::FuncType;
using isoline::binary::Module;
using isoline::binary::NameAssoc;
using isoline::binary::NameKind;
using isoline::binary::NameSection;
using isoline::binary::Opcode;
using isoline::binary::SegmentMode;
using isoline::binary::ValueType;
using isoline::text::compileText;
using isoline::text::ModuleReader;
using isoline::text::NodeId;
using isoline::text::parseSExpressions;
using isoline::text::specMessage;
using isoline::text::TextError;
using isoline::text::TextErrorCode;
using isoline::text::TextLimits;

namespace
{

/// The module @p source reads as, which it must.
Module readModule(std::string_view source, TextLimits limits = {})
{
    auto module = isoline::text::testing::readModuleText(source, limits);
    INFO(source);
    if (!module)
        INFO(specMessage(module.error()));
    REQUIRE(module.has_value());
    return std::move(*module);
}

/// The error @p source is refused with, which it must be.
TextError readError(std::string_view source, TextLimits limits = {})
{
    auto module = isoline::text::testing::readModuleText(source, limits);
    INFO(source);
    REQUIRE_FALSE(module.has_value());
    return std::move(module.error());
}

/// The names of @p module's name section, its last custom section.
NameSection namesOf(Module const& module)
{
    REQUIRE_FALSE(module.customs.empty());
    REQUIRE(module.customs.back().name == "name");
    auto names = isoline::binary::decodeNameSection(module.customs.back().bytes);
    REQUIRE(names.has_value());
    return std::move(*names);
}

/// The direct name map of @p kind.
std::vector<NameAssoc> const& namesOf(NameSection const& names, NameKind kind)
{
    return names.direct[static_cast<std::size_t>(kind)];
}

/// A function type.
FuncType signature(std::vector<ValueType> params, std::vector<ValueType> results = {})
{
    return FuncType { .params = std::move(params), .results = std::move(results) };
}

/// Whether @p expression is the constant expression `t.const 0`.
bool isZero(ConstExpr const& expression, Opcode opcode)
{
    return expression.size() == 2 && expression[0].opcode == opcode && expression[0].c == 0
           && expression[1].opcode == Opcode::End;
}

} // namespace

TEST_CASE("ModuleReader.bareFields")
{
    auto const bare = readModule("(func) (memory 1)");
    auto const whole = readModule("(module (func) (memory 1))");
    CHECK(isoline::binary::testing::sameModule(bare, whole));
    CHECK(bare.functions.size() == 1);
    CHECK(bare.memories.size() == 1);
    CHECK(readModule("").types.empty());
}

TEST_CASE("ModuleReader.lineEnds")
{
    // A newline is LF, CR or CR LF (the text format's `newline`), so a file Git checked out with CR LF
    // reads as its LF original does, line comments ended included.
    auto const lf = readModule("(module ;; a comment\n  (func $f (result i32)\n    i32.const 1))\n");
    constexpr auto Endings = std::to_array<std::pair<std::string_view, std::string_view>>({
        { "CR LF", "\r\n" },
        { "CR", "\r" },
    });
    for (auto const& [name, ending]: Endings)
    {
        INFO(name);
        auto const other = readModule(
            std::format("(module ;; a comment{0}  (func $f (result i32){0}    i32.const 1)){0}", ending));
        CHECK(isoline::binary::testing::sameModule(other, lf));
    }
}

TEST_CASE("ModuleReader.implicitTypesAppend")
{
    using isoline::binary::I32;
    using isoline::binary::I64;
    auto const module = readModule("(type (func (param i32))) (func (param i64)) (func (param i32))");
    CHECK(module.types == std::vector { signature({ I32 }), signature({ I64 }) });
    CHECK(module.functions == std::vector<uint32_t> { 1, 0 });

    // Explicit types come first, wherever they are written; implicit ones follow in order of use.
    auto const late =
        readModule("(func (result i64)) (func (param i32)) (func (result i64)) (type (func (param i32)))");
    CHECK(late.types == std::vector { signature({ I32 }), signature({}, { I64 }) });
    CHECK(late.functions == std::vector<uint32_t> { 1, 0, 1 });

    // An explicit type that repeats an earlier one is still a type of its own.
    auto const repeated = readModule("(type (func)) (type (func)) (func)");
    CHECK(repeated.types.size() == 2);
    CHECK(repeated.functions == std::vector<uint32_t> { 0 });
}

TEST_CASE("ModuleReader.manyImplicitTypesLinear")
{
    // 100'000 functions with distinct parameter lists: the binary spelling of each one's ordinal, as
    // i32 for 0 and i64 for 1, over 17 parameters. A scan of the types per use would take minutes.
    constexpr auto FunctionCount = 100'000U;
    constexpr auto ParamCount = 17U;
    auto source = std::string {};
    source.reserve(std::size_t { FunctionCount } * ((ParamCount * 4) + 20));
    for (auto const ordinal: std::views::iota(0U, FunctionCount))
    {
        source += "(func (param";
        for (auto const bit: std::views::iota(0U, ParamCount))
            source += ((ordinal >> bit) & 1U) != 0 ? " i64" : " i32";
        source += "))";
    }
    auto const module = readModule(source);
    CHECK(module.types.size() == FunctionCount);
    CHECK(module.functions.size() == FunctionCount);
    CHECK(module.functions.back() == FunctionCount - 1);
}

TEST_CASE("ModuleReader.inlineTypeMismatch")
{
    auto const error = readError("(type $t (func)) (func (type $t) (param i32))");
    CHECK(error.code == TextErrorCode::InlineFunctionType);
    CHECK(specMessage(error) == "inline function type does not match explicit type");
    CHECK(error.position.column == 30);

    auto const matching =
        readModule("(type $t (func (param i32) (result i64))) (func (type $t) (param i32) (result i64))");
    CHECK(matching.types.size() == 1);
    CHECK(matching.functions == std::vector<uint32_t> { 0 });
}

TEST_CASE("ModuleReader.inlineTypeUseNeedsItsType")
{
    // func.wast:438-457: a bare (type N) is the validator's to check; one whose inline signature must
    // be compared with type N needs it at once.
    auto const prefix = std::string { "(func $f (result f64)) (func $g (param i32)) (func $h (result f64))"
                                      "(type $t (func (param i32)))" };
    auto const bare = readModule(prefix + "(func (type 2))");
    CHECK(bare.types.size() == 2);
    CHECK(bare.functions.back() == 2);
    auto const validated = isoline::validate::validateModule(bare, FeatureSet::all());
    REQUIRE_FALSE(validated.has_value());
    CHECK(isoline::validate::specMessage(validated.error()).starts_with("unknown type"));

    auto const inlined = readError(prefix + "(func (type 2) (param i32))");
    CHECK(inlined.code == TextErrorCode::UnknownIdentifier);
    CHECK(specMessage(inlined) == "unknown type 2");

    // Named locals need the type's parameter count, and so the type.
    CHECK(specMessage(readError("(func (type 9) (local $x i32))")) == "unknown type 9");
    CHECK(readModule("(func (type 9) (local i32))").code[0].localCount == 1);
    auto const named = readModule("(type (func (param i32 i64))) (func (type 0) (local i32) (local $x f32))");
    auto const names = namesOf(named);
    auto const& locals = names.indirect[static_cast<std::size_t>(NameKind::Local)];
    REQUIRE(locals.size() == 1);
    CHECK(locals[0].names == std::vector { NameAssoc { .index = 3, .name = "x" } });
}

TEST_CASE("ModuleReader.importAfterDefinition")
{
    struct Row
    {
        std::string_view source;
        std::string_view message;
    };
    auto const rows = std::vector<Row> {
        { .source = R"wat((func) (import "m" "f" (func)))wat",
          .message = "import after function definition" },
        { .source = R"wat((table 1 funcref) (import "m" "f" (func)))wat",
          .message = "import after table definition" },
        { .source = R"wat((memory 1) (func (import "m" "f")))wat",
          .message = "import after memory definition" },
        { .source = R"wat((global i32) (table (import "m" "f") 1 funcref))wat",
          .message = "import after global definition" },
        { .source = R"wat((tag) (memory (import "m" "f") 1))wat", .message = "import after tag definition" },
        { .source = R"wat((func) (global (import "m" "f") i32))wat",
          .message = "import after function definition" },
        { .source = R"wat((func) (tag (import "m" "f")))wat", .message = "import after function definition" },
        { .source = R"wat((func) (import "m" (item "f" (func))))wat",
          .message = "import after function definition" },
        // The latest definition an import follows is reported, at the first import after it.
        { .source = R"wat((memory 1) (import "a" "b" (func)) (func) (import "c" "d" (func)))wat",
          .message = "import after function definition" },
        // A start field's check runs after every later field's.
        { .source = R"wat((start 0) (func) (import "a" "b" (func)) (start 0))wat",
          .message = "import after function definition" },
    };
    for (auto const& row: rows)
    {
        auto const error = readError(row.source);
        CHECK(error.code == TextErrorCode::ImportAfterDefinition);
        CHECK(specMessage(error) == row.message);
    }
    CHECK(readError(R"wat((memory 1) (import "a" "b" (func)) (func) (import "c" "d" (func)))wat")
              .position.column
          == 43);

    // An import of no items imports nothing, so it follows no definition.
    CHECK(readModule(R"wat((func) (import "m" (func)))wat").imports.empty());
    CHECK(readModule(R"wat((import "m" "f" (func)) (func) (export "f" (func 0)) (start 0))wat").imports.size()
          == 1);
}

TEST_CASE("ModuleReader.duplicateAndUnknown")
{
    auto const duplicate = readError("(func $f) (func $f)");
    CHECK(duplicate.code == TextErrorCode::DuplicateIdentifier);
    CHECK(specMessage(duplicate).starts_with("duplicate func"));
    CHECK(specMessage(duplicate) == "duplicate function $f");
    CHECK(duplicate.position.column == 17);

    auto const unknown = readError("(start $g)");
    CHECK(unknown.code == TextErrorCode::UnknownIdentifier);
    CHECK(specMessage(unknown).starts_with("unknown func"));
    CHECK(specMessage(unknown) == "unknown function $g");

    struct Row
    {
        std::string_view source;
        std::string_view message;
    };
    auto const rows = std::vector<Row> {
        { .source = "(type $x (func)) (type $x (func))", .message = "duplicate type $x" },
        { .source = R"wat((import "m" "f" (func $x)) (func $x))wat", .message = "duplicate function $x" },
        { .source = "(table $x 1 funcref) (table $x 1 funcref)", .message = "duplicate table $x" },
        { .source = "(memory $x 1) (memory $x 1)", .message = "duplicate memory $x" },
        { .source = "(global $x i32) (global $x i32)", .message = "duplicate global $x" },
        { .source = "(tag $x) (tag $x)", .message = "duplicate tag $x" },
        { .source = "(elem $x func) (elem $x func)", .message = "duplicate elem segment $x" },
        { .source = "(data $x) (data $x)", .message = "duplicate data segment $x" },
        { .source = R"wat((import "m" (item "a" (global $x i32)) (item "b" (global $x i32))))wat",
          .message = "duplicate global $x" },
        { .source = "(func (param $x i32) (local $x i32))", .message = "duplicate local $x" },
        { .source = "(func (param $x i32) (param $x i32))", .message = "duplicate local $x" },
        { .source = R"wat((export "e" (func $x)))wat", .message = "unknown function $x" },
        { .source = R"wat((export "e" (table $x)))wat", .message = "unknown table $x" },
        { .source = R"wat((export "e" (memory $x)))wat", .message = "unknown memory $x" },
        { .source = R"wat((export "e" (global $x)))wat", .message = "unknown global $x" },
        { .source = R"wat((export "e" (tag $x)))wat", .message = "unknown tag $x" },
        { .source = "(func (type $x))", .message = "unknown type $x" },
        { .source = "(func (param (ref $x)))", .message = "unknown type $x" },
        { .source = "(elem (table $x) (i32.const 0) func)", .message = "unknown table $x" },
        { .source = "(data (memory $x) (i32.const 0))", .message = "unknown memory $x" },
        { .source = "(elem func $x)", .message = "unknown function $x" },
        // Identifiers are printed as the reference prints them.
        { .source = R"wat((start $"a b"))wat", .message = "unknown function $a b" },
        { .source = R"wat((start $"a\"b\\c"))wat", .message = R"wat(unknown function $"a\"b\\c")wat" },
        { .source = R"wat((start $"\té€😀"))wat",
          .message = R"wat(unknown function $"\u{09}\u{e9}\u{20ac}\u{1f600}")wat" },
    };
    for (auto const& row: rows)
    {
        INFO(row.source);
        CHECK(specMessage(readError(row.source)) == row.message);
    }

    // Identifiers name what is defined after them: they are bound before anything is read.
    auto const forward =
        readModule(R"wat((start $f) (export "t" (table $t)) (func $f) (table $t 1 funcref))wat");
    CHECK(forward.start == 0U);
    CHECK(forward.exports.size() == 1);
}

TEST_CASE("ModuleReader.numericIndicesAreKept")
{
    auto const module = readModule(R"wat((start 7) (export "t" (table 9)) (elem (i32.const 0) 5))wat");
    CHECK(module.start == 7U);
    CHECK(module.exports[0].index == 9);
    CHECK(std::get<std::vector<uint32_t>>(module.elements[0].items) == std::vector<uint32_t> { 5 });

    CHECK(specMessage(readError("(start 4294967296)")) == "i32 constant out of range");
    CHECK(specMessage(readError("(memory 18446744073709551616)")) == "i64 constant out of range");
    CHECK(specMessage(readError("(memory 1 18446744073709551616)")) == "i64 constant out of range");
    CHECK(readError("(start 12345)", TextLimits { .maxNumberLength = 3 }).code
          == TextErrorCode::NumberTooLong);
    CHECK(readError("(start -1)").code == TextErrorCode::UnexpectedToken);
    CHECK(readError(R"wat((start "f"))wat").code == TextErrorCode::UnexpectedToken);
}

TEST_CASE("ModuleReader.multipleStart")
{
    auto const error = readError("(func) (start 0) (start 0) (start 0)");
    CHECK(error.code == TextErrorCode::MultipleStart);
    CHECK(specMessage(error) == "multiple start sections");
    CHECK(error.position.column == 25); // The last start field but one.
    CHECK(readModule("(func $f) (start $f)").start == 0U);
}

TEST_CASE("ModuleReader.inlineDataSizesMemory")
{
    auto const module = readModule(R"wat((memory (pagesize 1) (data "xyz")))wat");
    REQUIRE(module.memories.size() == 1);
    CHECK(module.memories[0].limits.min == 3);
    CHECK(module.memories[0].limits.max == 3U);
    CHECK(module.memories[0].pageSizeLog2 == 0);
    REQUIRE(module.data.size() == 1);
    CHECK(module.data[0].mode == SegmentMode::Active);
    CHECK(module.data[0].memory == 0);
    CHECK(isZero(module.data[0].offset, Opcode::I32Const));
    CHECK(module.data[0].bytes.size() == 3);

    auto const invalid = readError("(memory (pagesize 3))");
    CHECK(invalid.code == TextErrorCode::InvalidCustomPageSize);
    CHECK(specMessage(invalid) == "invalid custom page size");
    CHECK(readError("(memory (pagesize 0) (data))").code == TextErrorCode::InvalidCustomPageSize);

    // 64 KiB pages by default; an i64 memory's data starts at an i64 offset; a second memory's data
    // initializes it.
    auto const sized =
        readModule(R"wat((memory (data "ab")) (memory i64 (pagesize 65536) (data "a" "b" "c")))wat");
    CHECK(sized.memories[0].limits.min == 1);
    CHECK(sized.memories[0].pageSizeLog2 == 16);
    CHECK(sized.memories[1].limits.addressType == AddressType::I64);
    CHECK(sized.data[1].memory == 1);
    CHECK(sized.data[1].bytes.size() == 3);
    CHECK(isZero(sized.data[1].offset, Opcode::I64Const));
    CHECK(readModule("(memory (data))").memories[0].limits.min == 0);

    // An inline segment takes its index in source order.
    auto const named = readModule("(data $a) (memory (data)) (data $b)");
    CHECK(namesOf(namesOf(named), NameKind::Data)
          == std::vector { NameAssoc { .index = 0, .name = "a" }, NameAssoc { .index = 2, .name = "b" } });
}

TEST_CASE("ModuleReader.limitsAreU64")
{
    auto const module = readModule("(table 0x1_0000_0000 funcref)");
    REQUIRE(module.tables.size() == 1);
    CHECK(module.tables[0].type.limits.min == uint64_t { 1 } << 32U);
    auto const validated = isoline::validate::validateModule(module, FeatureSet::all());
    REQUIRE_FALSE(validated.has_value());
    CHECK(isoline::validate::specMessage(validated.error()).starts_with("table size"));
}

TEST_CASE("ModuleReader.compactImports")
{
    auto const module = readModule(R"wat((import "env" (item "a" (func)) (item "b" (global $g i32))))wat");
    REQUIRE(module.imports.size() == 2);
    CHECK(module.importModules == std::vector<std::string> { "env" });
    CHECK(module.imports[0].moduleIndex == 0);
    CHECK(module.imports[1].moduleIndex == 0);
    CHECK(module.imports[0].name == "a");
    CHECK(module.imports[1].kind == ExternKind::Global);
    CHECK(namesOf(namesOf(module), NameKind::Global)
          == std::vector { NameAssoc { .index = 0, .name = "g" } });

    auto const shared = readError(R"wat((import "t" (item "a") (item "b") (func $f)))wat");
    CHECK(shared.code == TextErrorCode::IdentifierNotAllowed);
    CHECK(specMessage(shared) == "identifier not allowed");
    CHECK(readError(R"wat((import "test" (func $foo)))wat").code == TextErrorCode::IdentifierNotAllowed);

    // imports-compact.wast:102: an import of no items and no descriptor imports nothing.
    auto const empty = readModule(R"wat((import "not-test") (func))wat");
    CHECK(empty.imports.empty());
    CHECK(empty.importModules.empty());

    // Items sharing a descriptor share its type; an import of no items still defines it.
    auto const sharing =
        readModule(R"wat((import "m" (item "a") (item "b") (func (param i32))) (import "n" "c" (func)))wat"
                   R"wat((import "o" (func (result i64))))wat");
    CHECK(sharing.imports.size() == 3);
    CHECK(sharing.importModules == std::vector<std::string> { "m", "n" });
    CHECK(sharing.imports[1].typeIndex == 0);
    CHECK(sharing.imports[2].moduleIndex == 1);
    CHECK(sharing.types.size() == 3);

    // Each import entry has one module name, and each import its own descriptor's index.
    auto const entries =
        readModule(R"wat((import "a" "x" (memory 1)) (import "b" (item "y" (table 1 funcref))))wat"
                   R"wat((func $f (import "c" "z")) (import "d" (item "w") (global (mut i64))))wat");
    CHECK(entries.importModules == std::vector<std::string> { "a", "b", "c", "d" });
    CHECK(entries.imports[3].moduleIndex == 3);
    CHECK(entries.imports[3].global.mutability == isoline::binary::Mutability::Var);
}

TEST_CASE("ModuleReader.namesFromIdentifiers")
{
    auto const module = readModule("(module $m (func $f (param $x i32)))");
    auto const names = namesOf(module);
    CHECK(names.module == "m");
    CHECK(namesOf(names, NameKind::Function) == std::vector { NameAssoc { .index = 0, .name = "f" } });
    auto const& locals = names.indirect[static_cast<std::size_t>(NameKind::Local)];
    REQUIRE(locals.size() == 1);
    CHECK(locals[0].index == 0);
    CHECK(locals[0].names == std::vector { NameAssoc { .index = 0, .name = "x" } });
    CHECK(module.customs.back().after == isoline::binary::SectionAnchor::Data);

    // No identifiers, no name section.
    CHECK(readModule("(func) (memory 1)").customs.empty());
}

TEST_CASE("ModuleReader.namesOfEverySpace")
{
    // What wasm-tools 1.263.0 names: every space, an imported function's parameters as its locals, an
    // explicit type's and a defined tag's parameters, and no imported tag's.
    auto const module = readModule(R"((module $m (type $t (func (param $a i32) (param i64)))
        (import "m" "x" (func $imp (param $z i32)))
        (import "m" "t" (tag $it (param $ip i32)))
        (func $f (type $t) (param $x i32) (param i64) (local $l f32))
        (func $g (param i32) (local $q i32) (local f64) (local $r i32))
        (tag $e (param $p i32))
        (memory $mem 1) (table $tb 1 funcref) (global $gl i32)
        (elem $el func) (data $d "")))");
    auto const names = namesOf(module);
    CHECK(namesOf(names, NameKind::Type) == std::vector { NameAssoc { .index = 0, .name = "t" } });
    CHECK(namesOf(names, NameKind::Function)
          == std::vector { NameAssoc { .index = 0, .name = "imp" },
                           NameAssoc { .index = 1, .name = "f" },
                           NameAssoc { .index = 2, .name = "g" } });
    CHECK(namesOf(names, NameKind::Table) == std::vector { NameAssoc { .index = 0, .name = "tb" } });
    CHECK(namesOf(names, NameKind::Memory) == std::vector { NameAssoc { .index = 0, .name = "mem" } });
    CHECK(namesOf(names, NameKind::Global) == std::vector { NameAssoc { .index = 0, .name = "gl" } });
    CHECK(namesOf(names, NameKind::Element) == std::vector { NameAssoc { .index = 0, .name = "el" } });
    CHECK(namesOf(names, NameKind::Data) == std::vector { NameAssoc { .index = 0, .name = "d" } });
    CHECK(namesOf(names, NameKind::Tag)
          == std::vector { NameAssoc { .index = 0, .name = "it" }, NameAssoc { .index = 1, .name = "e" } });

    auto const& locals = names.indirect[static_cast<std::size_t>(NameKind::Local)];
    REQUIRE(locals.size() == 3);
    CHECK(locals[0].names == std::vector { NameAssoc { .index = 0, .name = "z" } });
    CHECK(locals[1].names
          == std::vector { NameAssoc { .index = 0, .name = "x" }, NameAssoc { .index = 2, .name = "l" } });
    CHECK(locals[2].names
          == std::vector { NameAssoc { .index = 1, .name = "q" }, NameAssoc { .index = 3, .name = "r" } });
    auto const& params = names.indirect[static_cast<std::size_t>(NameKind::Parameter)];
    REQUIRE(params.size() == 1);
    CHECK(params[0].names == std::vector { NameAssoc { .index = 0, .name = "a" } });
    auto const& tagParams = names.indirect[static_cast<std::size_t>(NameKind::TagParameter)];
    REQUIRE(tagParams.size() == 1);
    CHECK(tagParams[0].index == 1);
    CHECK(tagParams[0].names == std::vector { NameAssoc { .index = 0, .name = "p" } });

    // Locals of one type in a row form one run.
    auto const& code = module.code[1];
    CHECK(code.localCount == 3);
    REQUIRE(code.locals.size() == 3);
    CHECK(code.locals[0].count == 1);
}

TEST_CASE("ModuleReader.scriptModuleForms")
{
    for (auto const* source: { "(module definition $d (func))", "(module $d (func))" })
    {
        INFO(source);
        auto const bytes = compileText(source, TextLimits {});
        REQUIRE(bytes.has_value());
        auto const module = isoline::binary::decodeModule(*bytes, FeatureSet::all());
        REQUIRE(module.has_value());
        CHECK(namesOf(*module).module == "d");
        CHECK(module->code.size() == 1);
    }
    auto const definition = compileText("(module definition (func))", TextLimits {});
    auto const plain = compileText("(module (func))", TextLimits {});
    REQUIRE(definition.has_value());
    REQUIRE(plain.has_value());
    CHECK(*definition == *plain);
}

TEST_CASE("ModuleReader.readsOneModuleNode")
{
    auto const tree = parseSExpressions("(module $a (memory 1)) (func) ()", TextLimits {});
    REQUIRE(tree.has_value());
    auto const reader = ModuleReader { TextLimits {} };
    auto const module = reader.read(*tree, NodeId { 0 });
    REQUIRE(module.has_value());
    CHECK(module->memories.size() == 1);

    auto const notAModule = reader.read(*tree, tree->nodes()[0].nextSibling);
    REQUIRE_FALSE(notAModule.has_value());
    CHECK(notAModule.error().code == TextErrorCode::UnexpectedToken);
    CHECK(notAModule.error().position.column == 25);

    auto const empty = reader.read(*tree, tree->nodes()[tree->nodes()[0].nextSibling].nextSibling);
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == TextErrorCode::UnexpectedToken);
    CHECK(empty.error().position.column == 32); // Its `)`.

    CHECK(readError("(module").code == TextErrorCode::UnexpectedToken);
    CHECK(readError("(module) (module)").code == TextErrorCode::UnexpectedToken);
    // Out of the macro: in a macro argument, MSVC 19.44 warns of this raw string's `\e` as an unrecognized
    // escape (C4129, an error under /WX; measured in CI), as it does not of the same string outside one.
    auto const malformedName = std::string_view { R"wat((module $"\ef"))wat" };
    CHECK(readError(malformedName).code == TextErrorCode::MalformedUtf8);
}

TEST_CASE("ModuleReader.tables")
{
    using isoline::binary::ExternRef;
    using isoline::binary::FuncRef;
    using isoline::binary::NonNullFuncRef;
    auto const module = readModule(R"((func $f)
        (table $t (export "t") (export "u") i64 1 2 externref)
        (table funcref (elem $f 0 $f))
        (table i64 funcref (elem (item ref.func $f) (ref.null func)))
        (table 1 (ref null func) (ref.null func)))");
    REQUIRE(module.tables.size() == 4);
    CHECK(module.tables[0].type.element == ExternRef);
    CHECK(module.tables[0].type.limits.addressType == AddressType::I64);
    CHECK(module.tables[0].type.limits.max == 2U);
    CHECK(module.tables[0].init.empty());
    CHECK(module.exports.size() == 2);
    CHECK(module.exports[1].name == "u");
    CHECK(module.exports[1].kind == ExternKind::Table);

    CHECK(module.tables[1].type.limits.min == 3);
    CHECK(module.tables[1].type.limits.max == 3U);
    REQUIRE(module.elements.size() == 2);
    CHECK(module.elements[0].mode == SegmentMode::Active);
    CHECK(module.elements[0].table == 1);
    CHECK(module.elements[0].type == NonNullFuncRef);
    CHECK(isZero(module.elements[0].offset, Opcode::I32Const));
    CHECK(std::get<std::vector<uint32_t>>(module.elements[0].items) == std::vector<uint32_t> { 0, 0, 0 });

    CHECK(module.tables[2].type.limits.min == 2);
    CHECK(module.elements[1].table == 2);
    CHECK(module.elements[1].type == FuncRef);
    CHECK(isZero(module.elements[1].offset, Opcode::I64Const));
    CHECK(std::get<std::vector<ConstExpr>>(module.elements[1].items).size() == 2);

    CHECK(module.tables[3].type.element.heap.kind == AbstractHeapType::Func);
    REQUIRE(module.tables[3].init.size() == 2);
    CHECK(module.tables[3].init[0].opcode == Opcode::RefNull);
    CHECK(readModule("(table funcref (elem))").tables[0].type.limits.min == 0);
}

TEST_CASE("ModuleReader.inlineElementsTakeTheTableType")
{
    // br_table.wast:1019: function indices in a table of another reference type than a function
    // reference are `ref.func` items of the table's type (the reference's elem_var_list), not a segment of
    // function indices, whose type is (ref func).
    auto const module = readModule("(type $t (func)) (func $tf) (table $t (ref null $t) (elem $tf $tf))");
    REQUIRE(module.tables.size() == 1);
    REQUIRE(module.elements.size() == 1);
    CHECK(module.elements[0].type == module.tables[0].type.element);
    CHECK(module.elements[0].type.heap.kind == isoline::binary::AbstractHeapType::Concrete);
    auto const& items = std::get<std::vector<ConstExpr>>(module.elements[0].items);
    REQUIRE(items.size() == 2);
    REQUIRE(items[1].size() == 2);
    CHECK(items[1][0].opcode == Opcode::RefFunc);
    CHECK(items[1][0].a == 0U);
    CHECK(items[1][1].opcode == Opcode::End);
    CHECK(module.tables[0].type.limits.min == 2);
}

TEST_CASE("ModuleReader.memories")
{
    auto const module = readModule(R"wat((memory (import "a" "b") i64 1 (pagesize 1))
        (memory $m (export "m") 2 3 (pagesize 65536)))wat");
    REQUIRE(module.imports.size() == 1);
    CHECK(module.imports[0].memory.pageSizeLog2 == 0);
    CHECK(module.imports[0].memory.limits.addressType == AddressType::I64);
    REQUIRE(module.memories.size() == 1);
    CHECK(module.memories[0].limits.min == 2);
    CHECK(module.memories[0].limits.max == 3U);
    CHECK(module.memories[0].pageSizeLog2 == 16);
    CHECK(module.exports[0].index == 1);
}

TEST_CASE("ModuleReader.globalsTagsAndExports")
{
    using isoline::binary::I32;
    using isoline::binary::I64;
    auto const module = readModule(R"((global (import "m" "g") i32)
        (tag (import "m" "t") (type 0))
        (global $g (export "g") (mut i64) (i64.const 1))
        (tag $e (export "e") (param i32))
        (export "g2" (global $g)) (export "e2" (tag 1)) (export "m" (memory 0)) (export "f" (func 0)))");
    REQUIRE(module.imports.size() == 2);
    CHECK(module.imports[0].global.type == I32);
    CHECK(module.imports[1].kind == ExternKind::Tag);
    CHECK(module.imports[1].typeIndex == 0);
    REQUIRE(module.globals.size() == 1);
    CHECK(module.globals[0].type.type == I64);
    CHECK(module.globals[0].type.mutability == isoline::binary::Mutability::Var);
    REQUIRE(module.tags.size() == 1);
    CHECK(module.types == std::vector { signature({ I32 }) });
    REQUIRE(module.exports.size() == 6);
    CHECK(module.exports[0].index == 1); // The defined global follows the imported one.
    CHECK(module.exports[1].kind == ExternKind::Tag);
    CHECK(module.exports[1].index == 1);
    CHECK(module.exports[2].index == 1);
    CHECK(module.exports[4].kind == ExternKind::Memory);
}

TEST_CASE("ModuleReader.elementSegments")
{
    using isoline::binary::ExternRef;
    using isoline::binary::NonNullFuncRef;
    auto const module = readModule(R"((func $f) (table $t 1 funcref)
        (elem $p func $f 0)
        (elem declare func $f)
        (elem (table $t) (offset (i32.const 0)) func $f)
        (elem (i32.const 0) $f $f)
        (elem (offset) (ref func) (item ref.func $f))
        (elem externref (ref.null extern))
        (elem (i32.const 0)))");
    REQUIRE(module.elements.size() == 7);
    CHECK(module.elements[0].mode == SegmentMode::Passive);
    CHECK(module.elements[0].type == NonNullFuncRef);
    CHECK(module.elements[1].mode == SegmentMode::Declarative);
    CHECK(module.elements[2].mode == SegmentMode::Active);
    CHECK(module.elements[2].table == 0);
    CHECK(std::get<std::vector<uint32_t>>(module.elements[3].items).size() == 2);
    CHECK(module.elements[4].type == NonNullFuncRef);
    CHECK(std::get<std::vector<ConstExpr>>(module.elements[4].items).size() == 1);
    CHECK(module.elements[5].type == ExternRef);
    CHECK(std::get<std::vector<uint32_t>>(module.elements[6].items).empty());
}

TEST_CASE("ModuleReader.dataSegments")
{
    auto const module = readModule(R"((memory $m 1)
        (data $d "a" "\62c")
        (data (memory $m) (offset (i32.const 0)) "x")
        (data (i32.const 8))
        (data (memory 0) (i32.const 0) "y" "z"))");
    REQUIRE(module.data.size() == 4);
    CHECK(module.data[0].mode == SegmentMode::Passive);
    CHECK(module.data[0].bytes == std::vector { std::byte { 'a' }, std::byte { 'b' }, std::byte { 'c' } });
    CHECK(module.data[1].mode == SegmentMode::Active);
    CHECK(module.data[2].mode == SegmentMode::Active);
    CHECK(module.data[2].bytes.empty());
    CHECK(module.data[3].bytes.size() == 2);
}

TEST_CASE("ModuleReader.valueTypes")
{
    using isoline::binary::ExnRef;
    using isoline::binary::NullExnRef;
    auto const module = readModule(R"((type $t (func))
        (func (param i32 i64 f32 f64 v128 funcref externref exnref nullexnref)
              (param (ref $t) (ref null 0) (ref extern) (ref null exn) (ref noexn))))");
    auto const& params = module.types[1].params;
    REQUIRE(params.size() == 14);
    CHECK(params[7] == ExnRef);
    CHECK(params[8] == NullExnRef);
    CHECK(params[9] == isoline::binary::concreteRef(0, isoline::binary::Nullable::No));
    CHECK(params[10] == isoline::binary::concreteRef(0, isoline::binary::Nullable::Yes));
    CHECK(params[11].heap.kind == AbstractHeapType::Extern);
    CHECK(params[12].heap.kind == AbstractHeapType::Exn);
    CHECK(params[13].heap.kind == AbstractHeapType::NoExn);
}

TEST_CASE("ModuleReader.annotationsAreSkipped")
{
    // Annotations are read apart from the grammar (Task 15): wherever they are, the fields read alike.
    auto const annotated = readModule("(@a) (func (@b) $f (@c x) (param i32)) (@d)");
    auto const plain = readModule("(func $f (param i32))");
    CHECK(isoline::binary::testing::sameModule(annotated, plain));
}

TEST_CASE("ModuleReader.malformedFields")
{
    struct Row
    {
        std::string_view source;
        TextErrorCode code;
    };
    using enum TextErrorCode;
    auto const rows = std::vector<Row> {
        // Fields.
        { .source = "(module ())", .code = UnexpectedToken },
        { .source = "(module func)", .code = UnexpectedToken },
        { .source = "(module (fnc))", .code = UnknownOperator },
        { .source = R"wat((func $"\ef"))wat", .code = MalformedUtf8 },
        { .source = R"wat((start $"\ef"))wat", .code = MalformedUtf8 },
        { .source = R"wat((func (param $"\ef" i32)))wat", .code = MalformedUtf8 },
        // Types.
        { .source = "(type)", .code = UnexpectedToken },
        { .source = "(type x)", .code = UnknownOperator },
        { .source = "(type (fnc))", .code = UnknownOperator },
        { .source = "(type (func (type 0)))", .code = UnexpectedToken },
        { .source = "(type (func) (func))", .code = UnexpectedToken },
        { .source = "(type (func (param foo)))", .code = UnknownOperator },
        { .source = "(type (func (result i32) (param i32)))", .code = UnexpectedToken },
        { .source = "(type (func x))", .code = UnknownOperator },
        // Value types.
        { .source = "(func (param i33))", .code = UnknownOperator },
        { .source = "(func (param func))", .code = UnexpectedToken },
        { .source = "(func (param (foo)))", .code = UnknownOperator },
        { .source = "(func (param $x $y))", .code = UnexpectedToken },
        { .source = "(func (param (ref)))", .code = UnexpectedToken },
        { .source = "(func (param (ref any)))", .code = UnknownOperator },
        { .source = R"wat((func (param (ref "x"))))wat", .code = UnexpectedToken },
        { .source = "(func (param (ref func func)))", .code = UnexpectedToken },
        { .source = "(func (param $x))", .code = UnexpectedToken },
        { .source = "(func (param $x foo))", .code = UnknownOperator },
        { .source = "(func (param $x i32 i32))", .code = UnexpectedToken },
        { .source = "(func (result foo))", .code = UnknownOperator },
        // Type uses.
        { .source = "(func (type))", .code = UnexpectedToken },
        { .source = "(func (type 0 0))", .code = UnexpectedToken },
        // Out of order: refused by the grammar before the unknown type is looked up (func.wast:557).
        { .source = "(func (type $sig) (result i32) (param i32))", .code = UnexpectedToken },
        { .source = "(func (param i32) (type 0))", .code = UnexpectedToken },
        { .source = R"wat((import "a" "b" (func (result i32) (param i32))))wat", .code = UnexpectedToken },
        { .source = "(func (type 5) (param i32))", .code = UnknownIdentifier },
        // Inline exports and imports.
        { .source = "(func (export))", .code = UnexpectedToken },
        { .source = R"wat((func (export "a" "b")))wat", .code = UnexpectedToken },
        { .source = "(func (import))", .code = UnexpectedToken },
        { .source = R"wat((func (import "a")))wat", .code = UnexpectedToken },
        { .source = R"wat((func (import "a" "b" "c")))wat", .code = UnexpectedToken },
        { .source = R"wat((func (import "a" "b") (type)))wat", .code = UnexpectedToken },
        { .source = R"wat((func (import "a" "b") (type 5) (param i32)))wat", .code = UnknownIdentifier },
        { .source = R"wat((func (import "a" "b") (local i32)))wat", .code = UnexpectedToken },
        { .source = R"wat((func (import "a" "b") (fnc)))wat", .code = UnknownOperator },
        { .source = R"wat((func (import "a" "b") x))wat", .code = UnknownOperator },
        // Locals.
        { .source = "(func (local foo))", .code = UnknownOperator },
        { .source = "(func (local $x))", .code = UnexpectedToken },
        { .source = "(func (local $x foo))", .code = UnknownOperator },
        { .source = "(func (local $x i32 i32))", .code = UnexpectedToken },
        { .source = "(func (local $x i32) (local $x i32))", .code = DuplicateIdentifier },
        // Tables.
        { .source = "(table (export) 1 funcref)", .code = UnexpectedToken },
        { .source = R"wat((table (import "a") 1 funcref))wat", .code = UnexpectedToken },
        { .source = R"wat((table (import "a" "b")))wat", .code = UnexpectedToken },
        { .source = R"wat((table (import "a" "b") 1 i32))wat", .code = UnexpectedToken },
        { .source = R"wat((table (import "a" "b") 1 funcref 1))wat", .code = UnexpectedToken },
        { .source = "(table 18446744073709551616 funcref)", .code = ConstantOutOfRange },
        { .source = "(table 1)", .code = UnexpectedToken },
        { .source = "(table 1 (fnc))", .code = UnknownOperator },
        { .source = "(table i32)", .code = UnexpectedToken },
        { .source = "(table funcref)", .code = UnexpectedToken },
        { .source = "(table funcref (fnc))", .code = UnknownOperator },
        { .source = "(table funcref x)", .code = UnknownOperator },
        { .source = "(table funcref (elem) x)", .code = UnknownOperator },
        { .source = "(table funcref (elem $nope))", .code = UnknownIdentifier },
        { .source = "(table funcref (elem (item) 1))", .code = UnexpectedToken },
        { .source = "(table $t 1 funcref) (table $t funcref (elem))", .code = DuplicateIdentifier },
        // Memories.
        { .source = "(memory (export) 1)", .code = UnexpectedToken },
        { .source = R"wat((memory (import "a") 1))wat", .code = UnexpectedToken },
        { .source = R"wat((memory (import "a" "b")))wat", .code = UnexpectedToken },
        { .source = R"wat((memory (import "a" "b") 1 (pagesize 3)))wat", .code = InvalidCustomPageSize },
        { .source = R"wat((memory (import "a" "b") 1 2 3))wat", .code = UnexpectedToken },
        { .source = "(memory 1 (pagesize))", .code = UnexpectedToken },
        { .source = "(memory 1 (pagesize 1 2))", .code = UnexpectedToken },
        { .source = "(memory 1 (pagesize 4294967296))", .code = ConstantOutOfRange },
        { .source = "(memory 1 2 3)", .code = UnexpectedToken },
        { .source = "(memory foo)", .code = UnknownOperator },
        { .source = "(memory)", .code = UnexpectedToken },
        { .source = "(memory (pagesize 1))", .code = UnexpectedToken },
        { .source = "(memory (pagesize 3) (data))", .code = InvalidCustomPageSize },
        { .source = "(memory (fnc))", .code = UnknownOperator },
        { .source = "(memory (data) x)", .code = UnknownOperator },
        { .source = "(memory (data 1))", .code = UnexpectedToken },
        { .source = "(memory $m 1) (memory $m (data))", .code = DuplicateIdentifier },
        // Globals.
        { .source = "(global (export) i32)", .code = UnexpectedToken },
        { .source = R"wat((global (import "a") i32))wat", .code = UnexpectedToken },
        { .source = R"wat((global (import "a" "b")))wat", .code = UnexpectedToken },
        { .source = R"wat((global (import "a" "b") i32 i32))wat", .code = UnexpectedToken },
        { .source = "(global)", .code = UnexpectedToken },
        { .source = "(global (mut))", .code = UnexpectedToken },
        { .source = "(global (mut i32 i32))", .code = UnexpectedToken },
        { .source = "(global (mut foo))", .code = UnknownOperator },
        // Tags.
        { .source = "(tag (export))", .code = UnexpectedToken },
        { .source = R"wat((tag (import "a")))wat", .code = UnexpectedToken },
        { .source = "(tag (type))", .code = UnexpectedToken },
        { .source = "(tag (type 5) (param i32))", .code = UnknownIdentifier },
        { .source = "(tag x)", .code = UnknownOperator },
        // Element segments.
        { .source = "(elem (table) (i32.const 0) func)", .code = UnexpectedToken },
        { .source = "(elem (table 0 0) (i32.const 0) func)", .code = UnexpectedToken },
        { .source = "(elem (table 0))", .code = UnexpectedToken },
        { .source = "(elem (table 0) func)", .code = UnexpectedToken },
        { .source = "(elem (table 0) (i32.const 0))", .code = UnexpectedToken },
        { .source = "(elem foo)", .code = UnknownOperator },
        { .source = "(elem funcref 1)", .code = UnexpectedToken },
        // Data segments.
        { .source = "(data (memory) (i32.const 0))", .code = UnexpectedToken },
        { .source = "(data (memory 0 0) (i32.const 0))", .code = UnexpectedToken },
        { .source = "(data (memory 0))", .code = UnexpectedToken },
        { .source = R"wat((data (memory 0) "x"))wat", .code = UnexpectedToken },
        { .source = "(data 1)", .code = UnexpectedToken },
        // Imports.
        { .source = "(import)", .code = UnexpectedToken },
        { .source = R"wat((import "a" 1))wat", .code = UnexpectedToken },
        { .source = R"wat((import "a" "\ff" (func)))wat", .code = MalformedUtf8 },
        { .source = R"wat((import "a" "b"))wat", .code = UnexpectedToken },
        { .source = R"wat((import "a" "b" x))wat", .code = UnknownOperator },
        { .source = R"wat((import "a" "b" (fnc)))wat", .code = UnknownOperator },
        { .source = R"wat((import "a" "b" (func (type))))wat", .code = UnexpectedToken },
        { .source = R"wat((import "a" "b" (func (type 5) (param i32))))wat", .code = UnknownIdentifier },
        { .source = R"wat((import "a" "b" (func (local i32))))wat", .code = UnexpectedToken },
        { .source = R"wat((import "a" "b" (func foo)))wat", .code = UnknownOperator },
        { .source = R"wat((import "a" "b" (table)))wat", .code = UnexpectedToken },
        { .source = R"wat((import "a" "b" (memory)))wat", .code = UnexpectedToken },
        { .source = R"wat((import "a" "b" (global)))wat", .code = UnexpectedToken },
        { .source = R"wat((import "a" "b" (global i32 i32)))wat", .code = UnexpectedToken },
        { .source = R"wat((import "a" "b" (func) x))wat", .code = UnknownOperator },
        { .source = R"wat((import "a" (item 1)))wat", .code = UnexpectedToken },
        { .source = R"wat((import "a" (item "b" (func)) (item "c")))wat", .code = UnexpectedToken },
        { .source = R"wat((import "a" (item "b" (fnc))))wat", .code = UnknownOperator },
        { .source = R"wat((import "a" (item "b" (func) x)))wat", .code = UnknownOperator },
        { .source = R"wat((import "a" (item "b") (item "c" (func))))wat", .code = UnexpectedToken },
        { .source = R"wat((import "a" (item "b")))wat", .code = UnexpectedToken },
        { .source = R"wat((import "a" (item "b") (fnc)))wat", .code = UnknownOperator },
        { .source = R"wat((import "a" (item "b") (func) x))wat", .code = UnknownOperator },
        // Exports.
        { .source = "(export)", .code = UnexpectedToken },
        { .source = "(export 1 (func 0))", .code = UnexpectedToken },
        { .source = R"wat((export "\ff" (func 0)))wat", .code = MalformedUtf8 },
        { .source = R"wat((export "a"))wat", .code = UnexpectedToken },
        { .source = R"wat((export "a" func))wat", .code = UnexpectedToken },
        { .source = R"wat((export "a" (fnc 0)))wat", .code = UnknownOperator },
        { .source = R"wat((export "a" (func)))wat", .code = UnexpectedToken },
        { .source = R"wat((export "a" (func 0 0)))wat", .code = UnexpectedToken },
        { .source = R"wat((export "a" (func 0) x))wat", .code = UnknownOperator },
        // Start.
        { .source = "(start)", .code = UnexpectedToken },
        { .source = "(start 0 0)", .code = UnexpectedToken },
    };
    for (auto const& row: rows)
    {
        INFO(row.source);
        auto const error = readError(row.source);
        INFO(specMessage(error));
        CHECK(error.code == row.code);
    }
}

namespace
{

/// The fields an instruction test checks, those of binary::Instruction that the reader fills.
struct Fields
{
    Opcode opcode = Opcode::Nop; ///< The instruction.
    uint32_t a = 0;              ///< Its first immediate.
    uint32_t b = 0;              ///< Its second immediate.
    uint64_t c = 0;              ///< Its wide immediate.
    uint8_t lane = 0;            ///< Its lane index.
    isoline::binary::BlockType::Kind blockKind = isoline::binary::BlockType::Kind::Empty; ///< Block form.
    ValueType type {};                                                                    ///< Its value type.

    bool operator==(Fields const&) const = default;
};

/// The fields of @p instruction that Fields holds.
Fields fieldsOf(isoline::binary::Instruction const& instruction)
{
    return Fields { .opcode = instruction.opcode,
                    .a = instruction.a,
                    .b = instruction.b,
                    .c = instruction.c,
                    .lane = instruction.lane,
                    .blockKind = instruction.blockKind,
                    .type = instruction.type };
}

/// The fields of whatever a Fields is printed as, for Catch2's messages.
std::string describe(Fields const& fields)
{
    return std::format("{} a={} b={} c={:#x} lane={} block={} type={}",
                       isoline::text::textName(fields.opcode),
                       fields.a,
                       fields.b,
                       fields.c,
                       fields.lane,
                       static_cast<int>(fields.blockKind),
                       static_cast<int>(fields.type.kind));
}

/// The fields every instruction test module declares, so that each space has a named entity at index 1
/// or 0: `$v` = type 0 [] -> [], `$ii` = type 1 [i32] -> [i32], `$f` = function 0, the tested function
/// is function 1, tables `$t` 0 and `$u` 1, memories `$m` 0 and `$n` 1, global `$g`, tag `$e`,
/// element segment `$s` and data segment `$d`.
constexpr auto InstructionPrelude = std::string_view { R"wat(
    (type $v (func)) (type $ii (func (param i32) (result i32)))
    (func $f) (table $t 1 funcref) (table $u 1 funcref) (memory $m 1) (memory $n 1)
    (global $g (mut i32) i32.const 0) (tag $e) (elem $s func) (data $d ""))wat" };

/// The module of a function with parameter `$p` (i32) and local `$l` (i64), whose body is @p body,
/// after InstructionPrelude.
Module readBody(std::string_view body)
{
    return readModule(std::string { InstructionPrelude } + "(func (param $p i32) (local $l i64) "
                      + std::string { body } + ")");
}

/// The error a function body after InstructionPrelude is refused with.
TextError readBodyError(std::string_view body)
{
    return readError(std::string { InstructionPrelude } + "(func (param $p i32) (local $l i64) "
                     + std::string { body } + ")");
}

/// @p count copies of @p text.
std::string repeated(std::string_view text, std::size_t count)
{
    auto result = std::string {};
    result.reserve(text.size() * count);
    for ([[maybe_unused]] auto const copy: std::views::iota(std::size_t { 0 }, count))
        result += text;
    return result;
}

/// What InstructionReader alone makes of one function body: its code and each instruction's source
/// offset.
struct ReadInstructions
{
    isoline::binary::Function code; ///< The body and its side tables.
    std::vector<uint32_t> offsets;  ///< sourceOffsets(), one per instruction.
};

/// Reads @p source, one list whose keyword is followed by instructions only, with an InstructionReader
/// of no module context; it must read.
ReadInstructions readInstructions(std::string const& source, TextLimits const& limits = {})
{
    auto const tree = parseSExpressions(source, limits);
    REQUIRE(tree.has_value());
    auto fields = isoline::text::FieldReader { *tree, limits };
    auto const locals = isoline::text::Symbols {};
    auto result = ReadInstructions {};
    auto reader = isoline::text::InstructionReader { fields, locals, result.code };
    auto const list = tree->firstTopLevel();
    auto const read =
        reader.read(isoline::text::InstructionNodes { .list = list, .first = fields.bodyOf(list).peek() });
    if (!read)
        INFO(specMessage(read.error()));
    REQUIRE(read.has_value());
    result.offsets.assign(reader.sourceOffsets().begin(), reader.sourceOffsets().end());
    return result;
}

/// The bytes compileText() makes of @p source, which it must compile.
std::vector<std::byte> compileSource(std::string const& source)
{
    auto bytes = compileText(source, TextLimits {});
    INFO(source);
    if (!bytes)
        INFO(specMessage(bytes.error()));
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

/// The bytes compileText() makes of a function body after InstructionPrelude, which it must compile.
std::vector<std::byte> compileBody(std::string_view body)
{
    return compileSource(std::string { InstructionPrelude } + "(func (param $p i32) (local $l i64) "
                         + std::string { body } + ")");
}

} // namespace

TEST_CASE("ModuleReader.flatNestingIsUnbounded")
{
    // Flat block structure takes no parenthesized level, so it reads at any depth.
    constexpr auto Depth = std::size_t { 100'000 };
    auto limits = TextLimits {};
    limits.maxNesting = 16;
    auto const module =
        readModule("(func " + repeated("block ", Depth) + repeated("end ", Depth) + ")", limits);
    auto const& body = module.code.at(0).body;
    REQUIRE(body.size() == (2 * Depth) + 1);
    CHECK(body.front().opcode == Opcode::Block);
    CHECK(body[Depth].opcode == Opcode::End);
    CHECK(body.back().opcode == Opcode::End);
}

TEST_CASE("ModuleReader.deepLabelsAreLinear")
{
    // Every label reference resolves in logarithmic time, whatever the nesting: a search back through the
    // open blocks would take about Depth² / 2 steps here, which the test's timeout does not allow.
    constexpr auto Depth = std::size_t { 100'000 };

    // One label name 100'000 times nested: each `br $l` resolves to the innermost one.
    auto const nested =
        readInstructions("(func " + repeated("block $l ", Depth) + repeated("br $l end ", Depth) + ")");
    auto const& body = nested.code.body;
    REQUIRE(body.size() == (3 * Depth) + 1);
    auto branches =
        body | std::views::filter([](auto const& instruction) { return instruction.opcode == Opcode::Br; });
    CHECK(std::ranges::distance(branches) == static_cast<std::ptrdiff_t>(Depth));
    CHECK(std::ranges::all_of(branches, [](auto const& instruction) { return instruction.a == 0; }));

    // One named block around 100'000 unnamed ones, branching to it from every level.
    auto const outer = readInstructions("(func block $l " + repeated("block br $l ", Depth)
                                        + repeated("end ", Depth + 1) + ")");
    auto const& outerBody = outer.code.body;
    REQUIRE(outerBody.size() == (3 * Depth) + 3);
    for (auto const level: std::views::iota(std::size_t { 0 }, Depth))
    {
        auto const& branch = outerBody[2 + (2 * level)];
        REQUIRE(branch.opcode == Opcode::Br);
        REQUIRE(branch.a == level + 1);
    }

    // Folded blocks resolve alike, nested 500'000 deep under a raised nesting cap: unfolding takes no
    // stack frame per level, and each `br $l` no search through the levels around it.
    constexpr auto FoldedDepth = std::size_t { 500'000 };
    auto limits = TextLimits {};
    limits.maxNesting = 1'000'000;
    auto const folded = readInstructions("(func (block $l " + repeated("(block (br $l) ", FoldedDepth)
                                             + std::string(FoldedDepth + 2, ')'),
                                         limits);
    auto foldedTargets =
        folded.code.body
        | std::views::filter([](auto const& instruction) { return instruction.opcode == Opcode::Br; })
        | std::views::transform(&isoline::binary::Instruction::a);
    REQUIRE(std::ranges::distance(foldedTargets) == static_cast<std::ptrdiff_t>(FoldedDepth));
    CHECK(std::ranges::max(foldedTargets) == FoldedDepth);
}

TEST_CASE("ModuleReader.labels")
{
    auto const mismatching = readError("(func block $a end $b)");
    CHECK(mismatching.code == TextErrorCode::MismatchingLabel);
    CHECK(mismatching.position.column == 20);
    CHECK(readError("(func block end $a)").code == TextErrorCode::MismatchingLabel);
    CHECK(readError("(func i32.const 0 if $a else $b end)").code == TextErrorCode::MismatchingLabel);
    CHECK(readError("(func i32.const 0 if $a else $a end $b)").code == TextErrorCode::MismatchingLabel);
    CHECK(readError("(func i32.const 0 if else $a end)").code == TextErrorCode::MismatchingLabel);

    auto const unknown = readError("(func block $a end br $a)");
    CHECK(unknown.code == TextErrorCode::UnknownLabel);
    CHECK(unknown.position.column == 23);
    CHECK(readError("(func br $x)").code == TextErrorCode::UnknownLabel);

    // From depth 2, the outer of two blocks is label 1.
    auto const outer = readModule("(func block $a block $b br $a end $b end $a)");
    CHECK(outer.code[0].body[2].opcode == Opcode::Br);
    CHECK(outer.code[0].body[2].a == 1);

    // A shadowed label resolves to the innermost block of its name, and the outer one again after it.
    auto const shadowed = readModule("(func block $a block $a br $a end br $a end)");
    auto const& body = shadowed.code[0].body;
    CHECK(body[2].a == 0);
    CHECK(body[4].a == 0);
    auto const quoted = readModule(R"wat((func block $"a" br $a end $a))wat");
    CHECK(quoted.code[0].body[1].a == 0);

    // The labels of an if span its else; loop and try_table bind labels too.
    auto const branches = readModule("(func i32.const 0 if $i else br $i end loop $l br_if $l end"
                                     " try_table $t br $t end)");
    auto const& branchBody = branches.code[0].body;
    CHECK(branchBody[3].a == 0);
    CHECK(branchBody[6].a == 0);
    CHECK(branchBody[9].a == 0);
}

TEST_CASE("ModuleReader.labelNamesCountEveryBlock")
{
    // A label's index in the name section counts every block, loop, if and try_table of its function
    // in source order, named or not (as wasm-tools does).
    auto const module = readModule("(func) (func block block $b end end i32.const 0 if $c end loop end"
                                   " try_table $d end)");
    auto const names = namesOf(module);
    auto const& labels = names.indirect[static_cast<std::size_t>(NameKind::Label)];
    REQUIRE(labels.size() == 1);
    CHECK(labels[0].index == 1);
    CHECK(labels[0].names
          == std::vector<NameAssoc> {
              { .index = 1, .name = "b" }, { .index = 2, .name = "c" }, { .index = 4, .name = "d" } });
    CHECK(readModule("(func block end)").customs.empty());
}

TEST_CASE("ModuleReader.outOfRangeIndicesRead")
{
    // Numeric labels and indices are kept as written: the validator, not the reader, refuses them.
    auto const module = readModule("(func br 7 call 9)");
    auto const& body = module.code.at(0).body;
    REQUIRE(body.size() == 3);
    CHECK(fieldsOf(body[0]) == Fields { .opcode = Opcode::Br, .a = 7 });
    CHECK(fieldsOf(body[1]) == Fields { .opcode = Opcode::Call, .a = 9 });
    auto const validated = isoline::validate::validateModule(module, FeatureSet::all());
    CHECK_FALSE(validated.has_value());
    CHECK(isoline::validate::validateModule(readModule("(func br 0)"), FeatureSet::all()).has_value());
}

TEST_CASE("ModuleReader.memargDefaults")
{
    auto const natural = readModule("(memory 1) (func i64.load i32.load8_s v128.store)");
    auto const& body = natural.code.at(0).body;
    CHECK(fieldsOf(body[0]) == Fields { .opcode = Opcode::I64Load, .a = 3 });
    CHECK(fieldsOf(body[1]) == Fields { .opcode = Opcode::I32Load8S, .a = 0 });
    CHECK(fieldsOf(body[2]) == Fields { .opcode = Opcode::V128Store, .a = 4 });

    auto const notPowerOfTwo = readError("(memory 1) (func i32.load align=3)");
    CHECK(notPowerOfTwo.code == TextErrorCode::AlignmentNotPowerOfTwo);
    CHECK(notPowerOfTwo.position.column == 27);
    CHECK(readError("(memory 1) (func i32.load align=0)").code == TextErrorCode::AlignmentNotPowerOfTwo);

    // An offset is a u64 whatever the memory's address type; the validator refuses it for an i32 memory.
    auto const wide = readModule("(memory 1) (func i32.load offset=0x1_0000_0000 align=1)");
    CHECK(fieldsOf(wide.code[0].body[0]) == Fields { .opcode = Opcode::I32Load, .a = 0, .c = 0x1'0000'0000 });
    CHECK_FALSE(isoline::validate::validateModule(wide, FeatureSet::all()).has_value());
    auto const tooWide = readError("(memory 1) (func i32.load offset=18446744073709551616)");
    CHECK(specMessage(tooWide) == "i64 constant out of range");
}

TEST_CASE("ModuleReader.dataCountRule")
{
    // The data count section follows the text rule: present iff a body names a data segment.
    CHECK(readModule(R"wat((memory 1) (data "a") (data "b") (func data.drop 1))wat").dataCount == 2U);
    CHECK(readModule(R"wat((memory 1) (data "a") (func memory.init 0))wat").dataCount == 1U);
    CHECK(readModule("(memory 1) (func data.drop 0)").dataCount == 0U);
    CHECK_FALSE(readModule(R"wat((memory 1) (data "a") (func nop))wat").dataCount.has_value());
    CHECK_FALSE(readModule("").dataCount.has_value());
}

TEST_CASE("ModuleReader.unknownOperator")
{
    auto const error = readError("(func get_local 0)");
    CHECK(error.code == TextErrorCode::UnknownOperator);
    CHECK(specMessage(error) == "unknown operator get_local");
    CHECK(error.position.column == 7);
    // A keyword of the vocabulary that is no instruction is an unexpected token instead.
    CHECK(readError("(func param)").code == TextErrorCode::UnexpectedToken);
}

TEST_CASE("ModuleReader.v128Shapes")
{
    auto const module = readModule("(func v128.const i16x8 0 1 2 3 4 5 6 -1 v128.const f32x4 1 -0 inf nan"
                                   " i8x16.shuffle 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 31)");
    auto const& body = module.code.at(0).body;
    REQUIRE(module.v128Literals.size() == 3);
    CHECK(body[0].opcode == Opcode::V128Const);
    CHECK(body[0].a == 0);
    auto const bytesOf = [](std::initializer_list<int> values) {
        auto bytes = std::array<std::byte, 16> {};
        std::ranges::transform(
            values, bytes.begin(), [](int value) { return static_cast<std::byte>(value); });
        return bytes;
    };
    CHECK(module.v128Literals[0] == bytesOf({ 0, 0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 0xFF, 0xFF }));
    CHECK(module.v128Literals[1]
          == bytesOf({ 0, 0, 0x80, 0x3F, 0, 0, 0, 0x80, 0, 0, 0x80, 0x7F, 0, 0, 0xC0, 0x7F }));
    CHECK(body[2].opcode == Opcode::I8x16Shuffle);
    CHECK(body[2].a == 2);
    CHECK(module.v128Literals[2] == bytesOf({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 31 }));

    auto const i64 = readModule("(func v128.const i64x2 0x0102030405060708 -1)");
    CHECK(i64.v128Literals[0]
          == bytesOf({ 8, 7, 6, 5, 4, 3, 2, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }));
    auto const f64 = readModule("(func v128.const f64x2 1 0x1p-1074 v128.const i8x16 " + repeated("-128 ", 16)
                                + "v128.const i32x4 0xFFFFFFFF -2147483648 0 1)");
    CHECK(f64.v128Literals[0] == bytesOf({ 0, 0, 0, 0, 0, 0, 0xF0, 0x3F, 1, 0, 0, 0, 0, 0, 0, 0 }));
    CHECK(f64.v128Literals[1]
          == bytesOf({ 0x80,
                       0x80,
                       0x80,
                       0x80,
                       0x80,
                       0x80,
                       0x80,
                       0x80,
                       0x80,
                       0x80,
                       0x80,
                       0x80,
                       0x80,
                       0x80,
                       0x80,
                       0x80 }));
    CHECK(f64.v128Literals[2] == bytesOf({ 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0x80, 0, 0, 0, 0, 1, 0, 0, 0 }));

    auto const fifteen = readError("(func v128.const i8x16 " + repeated("0 ", 15) + ")");
    CHECK(fifteen.code == TextErrorCode::WrongLaneCount);
    CHECK(fifteen.position.column == 7);
    // The reference's lexer refuses a malformed NaN before the lanes are counted (simd_const.wast:383,
    // 468), while a known keyword after the lanes ends them.
    auto const nanLane = readError("(func (v128.const f32x4 nan:1 nan:1 nan:1 nan:1) drop)");
    CHECK(specMessage(nanLane) == "unknown operator nan:1");
    CHECK(nanLane.position.column == 25);
    CHECK(specMessage(readError("(func (v128.const f64x2 0 nan:1))")) == "unknown operator nan:1");
    CHECK(readError("(func v128.const i32x4 0 0 0 drop)").code == TextErrorCode::WrongLaneCount);
    // simd_const.wast:142 expects "constant out of range", without the lane type, for a lane that
    // overflows; only a shuffle's lane, a nat8, names its type (simd_lane.wast:522).
    auto const overflow = readError("(func v128.const i8x16 " + repeated("256 ", 16) + ")");
    CHECK(specMessage(overflow) == "constant out of range");
    CHECK(overflow.position.column == 7);
    CHECK(specMessage(readError("(func v128.const f32x4 1e39 0 0 0)")) == "constant out of range");
    CHECK(specMessage(readError("(func v128.const i32x4 1.5 0 0 0)")) == "constant out of range");
    CHECK(specMessage(readError("(func v128.const i32x4 0x 0 0 0)")) == "unknown operator 0x");

    auto const shortShuffle = readError("(func i8x16.shuffle " + repeated("0 ", 15) + ")");
    CHECK(shortShuffle.code == TextErrorCode::InvalidLaneLength);
    auto const wideShuffle = readError("(func i8x16.shuffle " + repeated("0 ", 15) + "256)");
    CHECK(specMessage(wideShuffle) == "i8 constant out of range");
    CHECK(wideShuffle.position.column == 7);
    CHECK(specMessage(readError("(func i8x16.shuffle nan " + repeated("0 ", 15) + ")"))
          == "i8 constant out of range");
    auto const longShuffle =
        readError("(func i8x16.shuffle " + repeated("0 ", 15) + "1" + repeated("0", 4'096) + ")");
    CHECK(longShuffle.code == TextErrorCode::NumberTooLong);
    CHECK(longShuffle.position.column == 51);
}

TEST_CASE("ModuleReader.instructionsAddImplicitTypes")
{
    using isoline::binary::BlockType;
    using isoline::binary::F32;
    using isoline::binary::I32;
    using isoline::binary::I64;
    // Two results are no short form: the block takes an implicit type.
    auto const twoResults = readModule("(func block (result i32 i64) end)");
    CHECK(twoResults.types == std::vector { signature({}), signature({}, { I32, I64 }) });
    CHECK(fieldsOf(twoResults.code[0].body[0])
          == Fields { .opcode = Opcode::Block, .a = 1, .blockKind = BlockType::Kind::TypeIndex });

    // Implicit types follow the order of first textual use, function bodies included: the block's type
    // comes before the next function's.
    auto const ordered = readModule("(func block (param i32) (result i32) end) (func (param f32))");
    CHECK(ordered.types == std::vector { signature({}), signature({ I32 }, { I32 }), signature({ F32 }) });
    CHECK(ordered.code[0].body[0].a == 1);
    CHECK(ordered.functions == std::vector<uint32_t> { 0, 2 });
}

TEST_CASE("ModuleReader.instructionImmediates")
{
    using isoline::binary::BlockType;
    using isoline::binary::I32;
    using isoline::binary::I64;
    struct Row
    {
        std::string_view body;   ///< The function body.
        std::size_t instruction; ///< The index of the instruction checked.
        Fields expected;         ///< What it reads as.
    };
    using enum Opcode;
    auto const rows = std::vector<Row> {
        // Plain indices, named and numeric.
        { .body = "local.get $l", .instruction = 0, .expected = { .opcode = LocalGet, .a = 1 } },
        { .body = "local.tee 0", .instruction = 0, .expected = { .opcode = LocalTee, .a = 0 } },
        { .body = "call $f", .instruction = 0, .expected = { .opcode = Call, .a = 0 } },
        { .body = "return_call 1", .instruction = 0, .expected = { .opcode = ReturnCall, .a = 1 } },
        { .body = "call_ref $ii", .instruction = 0, .expected = { .opcode = CallRef, .a = 1 } },
        { .body = "throw $e", .instruction = 0, .expected = { .opcode = Throw, .a = 0 } },
        { .body = "global.set $g", .instruction = 0, .expected = { .opcode = GlobalSet, .a = 0 } },
        { .body = "ref.func $f", .instruction = 0, .expected = { .opcode = RefFunc, .a = 0 } },
        { .body = "elem.drop $s", .instruction = 0, .expected = { .opcode = ElemDrop, .a = 0 } },
        { .body = "data.drop $d", .instruction = 0, .expected = { .opcode = DataDrop, .a = 0 } },
        // Optional indices.
        { .body = "table.get", .instruction = 0, .expected = { .opcode = TableGet, .a = 0 } },
        { .body = "table.set $u", .instruction = 0, .expected = { .opcode = TableSet, .a = 1 } },
        { .body = "table.size 1", .instruction = 0, .expected = { .opcode = TableSize, .a = 1 } },
        { .body = "memory.size $n", .instruction = 0, .expected = { .opcode = MemorySize, .a = 1 } },
        { .body = "memory.grow", .instruction = 0, .expected = { .opcode = MemoryGrow, .a = 0 } },
        // Pairs.
        { .body = "table.copy", .instruction = 0, .expected = { .opcode = TableCopy } },
        { .body = "table.copy $u $t", .instruction = 0, .expected = { .opcode = TableCopy, .a = 1, .b = 0 } },
        { .body = "memory.copy $m $n",
          .instruction = 0,
          .expected = { .opcode = MemoryCopy, .a = 0, .b = 1 } },
        { .body = "table.init $s", .instruction = 0, .expected = { .opcode = TableInit, .a = 0, .b = 0 } },
        { .body = "table.init $u $s", .instruction = 0, .expected = { .opcode = TableInit, .a = 0, .b = 1 } },
        { .body = "memory.init $d", .instruction = 0, .expected = { .opcode = MemoryInit, .a = 0, .b = 0 } },
        { .body = "memory.init $n 0",
          .instruction = 0,
          .expected = { .opcode = MemoryInit, .a = 0, .b = 1 } },
        // call_indirect: an optional table, then a type use.
        { .body = "call_indirect (type $v)", .instruction = 0, .expected = { .opcode = CallIndirect } },
        { .body = "call_indirect $u (type $ii)",
          .instruction = 0,
          .expected = { .opcode = CallIndirect, .a = 1, .b = 1 } },
        { .body = "call_indirect (param i32) (result i32)",
          .instruction = 0,
          .expected = { .opcode = CallIndirect, .a = 1 } },
        { .body = "return_call_indirect 1 (type 0) (param) (result)",
          .instruction = 0,
          .expected = { .opcode = ReturnCallIndirect, .a = 0, .b = 1 } },
        { .body = "call_indirect", .instruction = 0, .expected = { .opcode = CallIndirect, .a = 0 } },
        // Block types.
        { .body = "block end", .instruction = 0, .expected = { .opcode = Block } },
        { .body = "block $b (result i64) end",
          .instruction = 0,
          .expected = { .opcode = Block, .blockKind = BlockType::Kind::Value, .type = I64 } },
        { .body = "loop (type $v) end",
          .instruction = 0,
          .expected = { .opcode = Loop, .blockKind = BlockType::Kind::TypeIndex } },
        { .body = "loop (param i32) (result i32) end",
          .instruction = 0,
          .expected = { .opcode = Loop, .a = 1, .blockKind = BlockType::Kind::TypeIndex } },
        { .body = "i32.const 0 if (type $ii) (param i32) (result i32) else end",
          .instruction = 1,
          .expected = { .opcode = If, .a = 1, .blockKind = BlockType::Kind::TypeIndex } },
        { .body = "block (result i32) (result) end",
          .instruction = 0,
          .expected = { .opcode = Block, .blockKind = BlockType::Kind::Value, .type = I32 } },
        { .body = "try_table (result i32) end",
          .instruction = 0,
          .expected = { .opcode = TryTable, .blockKind = BlockType::Kind::Value, .type = I32 } },
        // Memory arguments.
        { .body = "i64.load $n offset=8 align=4",
          .instruction = 0,
          .expected = { .opcode = I64Load, .a = 2, .b = 1, .c = 8 } },
        { .body = "i32.store16 1", .instruction = 0, .expected = { .opcode = I32Store16, .a = 1, .b = 1 } },
        { .body = "f32.load offset=0x10",
          .instruction = 0,
          .expected = { .opcode = F32Load, .a = 2, .c = 16 } },
        { .body = "v128.load8_lane 1", .instruction = 0, .expected = { .opcode = V128Load8Lane, .lane = 1 } },
        { .body = "v128.load32_lane 1 2",
          .instruction = 0,
          .expected = { .opcode = V128Load32Lane, .a = 2, .b = 1, .lane = 2 } },
        { .body = "v128.store16_lane $n offset=2 align=1 7",
          .instruction = 0,
          .expected = { .opcode = V128Store16Lane, .a = 0, .b = 1, .c = 2, .lane = 7 } },
        // A natural is the memory before a memory argument, and the lane only alone.
        { .body = "v128.load8_lane 1 offset=4 2",
          .instruction = 0,
          .expected = { .opcode = V128Load8Lane, .b = 1, .c = 4, .lane = 2 } },
        { .body = "v128.load16_lane 1 align=1 3",
          .instruction = 0,
          .expected = { .opcode = V128Load16Lane, .b = 1, .lane = 3 } },
        { .body = "v128.load64_lane offset=8 1",
          .instruction = 0,
          .expected = { .opcode = V128Load64Lane, .a = 3, .c = 8, .lane = 1 } },
        { .body = "v128.load64_lane align=2 0",
          .instruction = 0,
          .expected = { .opcode = V128Load64Lane, .a = 1 } },
        // Lanes and constants.
        { .body = "i8x16.extract_lane_s 15",
          .instruction = 0,
          .expected = { .opcode = I8x16ExtractLaneS, .lane = 15 } },
        { .body = "f64x2.replace_lane 1",
          .instruction = 0,
          .expected = { .opcode = F64x2ReplaceLane, .lane = 1 } },
        { .body = "i32.const -1", .instruction = 0, .expected = { .opcode = I32Const, .c = 0xFFFF'FFFF } },
        { .body = "i32.const 0xFFFF_FFFF",
          .instruction = 0,
          .expected = { .opcode = I32Const, .c = 0xFFFF'FFFF } },
        { .body = "i64.const -2",
          .instruction = 0,
          .expected = { .opcode = I64Const, .c = 0xFFFF'FFFF'FFFF'FFFE } },
        { .body = "f32.const 1", .instruction = 0, .expected = { .opcode = F32Const, .c = 0x3F80'0000 } },
        { .body = "f64.const -0",
          .instruction = 0,
          .expected = { .opcode = F64Const, .c = 0x8000'0000'0000'0000 } },
        { .body = "f32.const nan:0x1",
          .instruction = 0,
          .expected = { .opcode = F32Const, .c = 0x7F80'0001 } },
        // Reference types and select.
        { .body = "ref.null func",
          .instruction = 0,
          .expected = { .opcode = RefNull, .type = isoline::binary::FuncRef } },
        { .body = "ref.null $v",
          .instruction = 0,
          .expected = { .opcode = RefNull,
                        .type = isoline::binary::concreteRef(0, isoline::binary::Nullable::Yes) } },
        { .body = "ref.null 1",
          .instruction = 0,
          .expected = { .opcode = RefNull,
                        .type = isoline::binary::concreteRef(1, isoline::binary::Nullable::Yes) } },
        { .body = "select", .instruction = 0, .expected = { .opcode = Select } },
        { .body = "select (result i32)",
          .instruction = 0,
          .expected = { .opcode = SelectTyped, .a = 0, .b = 1 } },
        { .body = "select (result) (result i64 i32)",
          .instruction = 0,
          .expected = { .opcode = SelectTyped, .a = 0, .b = 2 } },
        { .body = "select (result i32) select (result i64)",
          .instruction = 1,
          .expected = { .opcode = SelectTyped, .a = 1, .b = 1 } },
    };
    for (auto const& row: rows)
    {
        INFO(row.body);
        auto const module = readBody(row.body);
        auto const& body = module.code.back().body;
        REQUIRE(row.instruction < body.size());
        auto const actual = fieldsOf(body[row.instruction]);
        INFO(describe(actual));
        CHECK(actual == row.expected);
    }
}

TEST_CASE("ModuleReader.sideTables")
{
    using isoline::binary::CatchClause;
    using isoline::binary::CatchKind;
    using isoline::binary::I32;
    using isoline::binary::I64;

    auto const tables = readBody("block $a block br_table 0 $a 1 br_table $a end end"
                                 " select (result i32) (result i64)");
    auto const& function = tables.code.back();
    CHECK(function.brTableLabels == std::vector<uint32_t> { 0, 1, 1, 1 });
    CHECK(fieldsOf(function.body[2]) == Fields { .opcode = Opcode::BrTable, .a = 0, .b = 3 });
    CHECK(fieldsOf(function.body[3]) == Fields { .opcode = Opcode::BrTable, .a = 3, .b = 1 });
    CHECK(function.selectTypes == std::vector { I32, I64 });

    // A catch clause's label counts from outside its try_table.
    auto const handlers = readBody("block $o try_table $t (result i32) (catch $e $o) (catch_ref 0 1)"
                                   " (catch_all 0) (catch_all_ref $o) br $t end drop end"
                                   " try_table (catch_all 0) end");
    auto const& handlerFunction = handlers.code.back();
    CHECK(handlerFunction.catchClauses
          == std::vector<CatchClause> { { .kind = CatchKind::Catch, .tag = 0, .label = 0 },
                                        { .kind = CatchKind::CatchRef, .tag = 0, .label = 1 },
                                        { .kind = CatchKind::CatchAll, .tag = 0, .label = 0 },
                                        { .kind = CatchKind::CatchAllRef, .tag = 0, .label = 0 },
                                        { .kind = CatchKind::CatchAll, .tag = 0, .label = 0 } });
    auto const& tryTable = handlerFunction.body[1];
    CHECK(tryTable.opcode == Opcode::TryTable);
    CHECK(tryTable.c == 0);
    CHECK(tryTable.b == 4);
    CHECK(tryTable.blockKind == isoline::binary::BlockType::Kind::Value);
    CHECK(handlerFunction.body[2].a == 0); // br $t
    CHECK(handlerFunction.body[6].c == 4);
    CHECK(handlerFunction.body[6].b == 1);
    CHECK(specMessage(readBodyError("try_table $t (catch_all $t) end")) == "unknown label");
}

TEST_CASE("ModuleReader.constantExpressions")
{
    // Constant expressions read as function bodies do, side tables apart; a v128 literal goes to the
    // module's table.
    auto const module = readModule(R"wat((global i32 i32.const 7) (global v128 v128.const i64x2 1 2)
        (global funcref br_table 0 1 select (result i32)) (memory 1) (data (offset global.get 0) "x"))wat");
    REQUIRE(module.globals.size() == 3);
    CHECK(fieldsOf(module.globals[0].init[0]) == Fields { .opcode = Opcode::I32Const, .c = 7 });
    CHECK(module.globals[1].init[0].opcode == Opcode::V128Const);
    CHECK(module.v128Literals.size() == 1);
    CHECK(module.globals[2].init.size() == 3);
    CHECK(module.data[0].offset[0].opcode == Opcode::GlobalGet);
    // A constant expression has no locals and no labels around it.
    CHECK(specMessage(readError("(global i32 local.get $x)")) == "unknown local $x");
    CHECK(readError("(global i32 br $x)").code == TextErrorCode::UnknownLabel);
}

TEST_CASE("ModuleReader.malformedNestedExpressions")
{
    // An error inside an expression a field holds is the field's error: a table's initializer, an
    // element segment's offset and items, a data segment's offset.
    for (auto const* const source: {
             "(table 1 funcref (i32.const x))",
             "(table 1 funcref) (elem (offset (i32.const x)))",
             "(table 1 funcref) (elem (table 0) (offset (i32.const 0)) funcref x)",
             "(table 1 funcref) (elem (table 0) (offset (i32.const 0)) funcref (item x))",
             "(table 1 funcref) (elem (table 0) (offset (i32.const 0)) funcref (x))",
             "(memory 1) (data (offset (i32.const x)))",
         })
    {
        auto const error = readError(source);
        CHECK(specMessage(error) == "unknown operator x");
    }
}

TEST_CASE("ModuleReader.malformedInstructions")
{
    struct Row
    {
        std::string_view body;
        TextErrorCode code;
    };
    using enum TextErrorCode;
    auto const longNumber = "i32.const 1" + repeated("0", 4'096);
    auto const longLane = "v128.const i64x2 0 1" + repeated("0", 4'096);
    auto const rows = std::vector<Row> {
        // Operators and structure.
        { .body = "get_local 0", .code = UnknownOperator },
        { .body = "result", .code = UnexpectedToken },
        { .body = "$x", .code = UnexpectedToken },
        { .body = R"wat("x")wat", .code = UnexpectedToken },
        { .body = "end", .code = UnexpectedToken },
        { .body = "else", .code = UnexpectedToken },
        { .body = "block else end", .code = UnexpectedToken },
        { .body = "i32.const 0 if else else end", .code = UnexpectedToken },
        { .body = "block", .code = UnexpectedToken },
        { .body = "block $a end $b", .code = MismatchingLabel },
        { .body = "block end x", .code = UnknownOperator },
        // Folded instructions.
        { .body = "()", .code = UnexpectedToken },
        { .body = "((nop))", .code = UnexpectedToken },
        { .body = "(foo)", .code = UnknownOperator },
        { .body = "(else)", .code = UnexpectedToken },
        { .body = "(end)", .code = UnexpectedToken },
        { .body = "(nop nop)", .code = UnexpectedToken },
        { .body = "(drop (nop) nop)", .code = UnexpectedToken },
        { .body = "(drop 0 (nop))", .code = UnexpectedToken },
        { .body = R"wat((if "s" (then)))wat", .code = UnexpectedToken },
        { .body = "(table.copy $nope 0)", .code = UnknownIdentifier },
        { .body = "(table.init $nope 0)", .code = UnknownIdentifier },
        { .body = R"wat((if (i32.const 0) "s" (then)))wat", .code = UnexpectedToken },
        { .body = "block br_table 0 $nope end", .code = UnknownLabel },
        { .body = "(i8x16.shuffle 0 ,)", .code = UnknownOperator },
        { .body = "(drop (type 0))", .code = UnexpectedToken },
        { .body = "(i32.const (nop))", .code = UnexpectedToken },
        { .body = "(i32.const)", .code = UnexpectedToken },
        { .body = "(block end)", .code = UnexpectedToken },
        { .body = "(block block)", .code = UnexpectedToken },
        { .body = "(block (else))", .code = UnexpectedToken },
        { .body = "(if)", .code = UnexpectedToken },
        { .body = "(if (local.get 0))", .code = UnexpectedToken },
        { .body = "(if nop (then))", .code = UnexpectedToken },
        { .body = "(if 0 (then))", .code = UnexpectedToken },
        { .body = "(if (else))", .code = UnexpectedToken },
        { .body = "(if (then) (then))", .code = UnexpectedToken },
        { .body = "(if (then) (else) (else))", .code = UnexpectedToken },
        { .body = "(if (then) nop)", .code = UnexpectedToken },
        { .body = "(if (then) (else) nop)", .code = UnexpectedToken },
        { .body = "(if (then else))", .code = UnexpectedToken },
        { .body = "(if (then end))", .code = UnexpectedToken },
        { .body = "(if (then if))", .code = UnexpectedToken },
        { .body = "(if (then) (else end))", .code = UnexpectedToken },
        { .body = "(if $a (then br $b))", .code = UnknownLabel },
        { .body = "(if (result i32) (param i32) (then))", .code = UnexpectedToken },
        { .body = "(if (type 9) (result i32) (then))", .code = UnknownIdentifier },
        { .body = "(call_indirect (result i32) (param i32) (nop))", .code = UnexpectedToken },
        { .body = "(try_table (catch $x 0))", .code = UnknownIdentifier },
        // Indices.
        { .body = "br", .code = UnexpectedToken },
        { .body = "br x", .code = UnknownOperator },
        { .body = "br -1", .code = UnexpectedToken },
        { .body = "br 4294967296", .code = ConstantOutOfRange },
        { .body = "br $x", .code = UnknownLabel },
        { .body = "local.get $x", .code = UnknownIdentifier },
        { .body = "call $x", .code = UnknownIdentifier },
        { .body = R"wat(call $"\ef")wat", .code = MalformedUtf8 },
        { .body = R"wat(br $"\ef")wat", .code = MalformedUtf8 },
        { .body = R"wat(block $"\ef" end)wat", .code = MalformedUtf8 },
        { .body = R"wat(block $a end $"\ef")wat", .code = MalformedUtf8 },
        { .body = "br_table", .code = UnexpectedToken },
        { .body = "br_table $x", .code = UnknownLabel },
        { .body = "table.copy 0", .code = UnexpectedToken },
        { .body = "table.copy $x $t", .code = UnknownIdentifier },
        { .body = "table.init", .code = UnexpectedToken },
        { .body = "memory.init $n", .code = UnknownIdentifier },
        { .body = "table.get $x", .code = UnknownIdentifier },
        // Type uses.
        { .body = "call_indirect (type 9) (param i32)", .code = UnknownIdentifier },
        { .body = "call_indirect (type $v) (param i32)", .code = InlineFunctionType },
        { .body = "call_indirect (param $x i32)", .code = UnexpectedToken },
        { .body = "call_indirect (result i32) (param i32)", .code = UnexpectedToken },
        { .body = "call_indirect $x", .code = UnknownIdentifier },
        { .body = "block (param $x i32) end", .code = UnexpectedToken },
        { .body = "block (type 9) (result i32) end", .code = UnknownIdentifier },
        { .body = "block (type) end", .code = UnexpectedToken },
        { .body = "block (param foo) end", .code = UnknownOperator },
        { .body = "block (result foo) end", .code = UnknownOperator },
        { .body = "select (result foo)", .code = UnknownOperator },
        // Memory arguments.
        { .body = "i32.load align=3", .code = AlignmentNotPowerOfTwo },
        { .body = "i32.load align=18446744073709551616", .code = ConstantOutOfRange },
        { .body = "i32.load offset=18446744073709551616", .code = ConstantOutOfRange },
        { .body = "i32.load align=4 offset=0", .code = UnexpectedToken },
        { .body = "i32.load $x", .code = UnknownIdentifier },
        { .body = "v128.load8_lane", .code = UnexpectedToken },
        { .body = "v128.load8_lane offset=0", .code = UnexpectedToken },
        { .body = "v128.load8_lane align=1", .code = UnexpectedToken },
        { .body = "v128.load8_lane 0 256", .code = ConstantOutOfRange },
        { .body = "v128.load8_lane $x 0", .code = UnknownIdentifier },
        { .body = "v128.load8_lane align=3 0", .code = AlignmentNotPowerOfTwo },
        // Lanes and constants.
        { .body = "i8x16.extract_lane_s", .code = UnexpectedToken },
        { .body = "i8x16.extract_lane_s 256", .code = ConstantOutOfRange },
        { .body = "i8x16.extract_lane_s -1", .code = UnexpectedToken },
        { .body = "i32.const", .code = UnexpectedToken },
        { .body = "i32.const $x", .code = UnexpectedToken },
        { .body = "i32.const 1x", .code = UnknownOperator },
        { .body = "i32.const 4294967296", .code = ConstantOutOfRange },
        { .body = "i32.const 1.5", .code = ConstantOutOfRange },
        { .body = "i64.const nan", .code = ConstantOutOfRange },
        { .body = "f32.const 1e39", .code = ConstantOutOfRange },
        { .body = "f64.const nan:0x0", .code = ConstantOutOfRange },
        { .body = "f32.const nan:1", .code = UnknownOperator },
        { .body = longNumber, .code = NumberTooLong },
        { .body = "v128.const", .code = UnexpectedToken },
        { .body = "v128.const 0 0 0 0", .code = UnexpectedToken },
        { .body = "v128.const i8x15 0", .code = UnknownOperator },
        { .body = "v128.const i32x4 0 0 0", .code = WrongLaneCount },
        { .body = "v128.const i32x4 0 0 0 0 0", .code = WrongLaneCount },
        { .body = longLane, .code = NumberTooLong },
        { .body = "i8x16.shuffle", .code = InvalidLaneLength },
        // Reference types.
        { .body = "ref.null", .code = UnexpectedToken },
        { .body = "ref.null funcref", .code = UnexpectedToken },
        { .body = "ref.null any", .code = UnknownOperator },
        { .body = "ref.null $x", .code = UnknownIdentifier },
        // Catch clauses.
        { .body = "try_table (catch) end", .code = UnexpectedToken },
        { .body = "try_table (catch $e) end", .code = UnexpectedToken },
        { .body = "try_table (catch $x 0) end", .code = UnknownIdentifier },
        { .body = "try_table (catch_all) end", .code = UnexpectedToken },
        { .body = "try_table (catch_all 0 0) end", .code = UnexpectedToken },
        { .body = "try_table (catch_all $x) end", .code = UnknownLabel },
    };
    for (auto const& row: rows)
    {
        INFO(row.body);
        auto const error = readBodyError(row.body);
        INFO(specMessage(error));
        CHECK(error.code == row.code);
    }
}

TEST_CASE("ModuleReader.foldedEqualsFlat")
{
    struct Row
    {
        std::string_view folded; ///< A body of folded instructions.
        std::string_view flat;   ///< The same body, flat.
    };
    auto const rows = std::to_array<Row>({
        { .folded = "(i32.add (local.get 0) (i32.const 1))", .flat = "local.get 0 i32.const 1 i32.add" },
        { .folded = "(i32.add (i32.mul (local.get 0) (i32.const 2)) (i32.const 3))",
          .flat = "local.get 0 i32.const 2 i32.mul i32.const 3 i32.add" },
        { .folded = "(i32.add (@a) (local.get 0) (@b x (y)) (i32.const 1) (@c))",
          .flat = "local.get 0 i32.const 1 i32.add" },
        { .folded = "(drop (block (result i32) (i32.const 1)))",
          .flat = "block (result i32) i32.const 1 end drop" },
        { .folded = "(block $b nop (nop) block end (br $b))",
          .flat = "block $b nop nop block end br $b end" },
        { .folded = "(loop $l (br_if $l (local.get 0)))", .flat = "loop $l local.get 0 br_if $l end" },
        { .folded = "(if (result i32) (local.get 0) (then (i32.const 1)) (else (i32.const 2)))",
          .flat = "local.get 0 if (result i32) i32.const 1 else i32.const 2 end" },
        { .folded = "(if (local.get 0) (then))", .flat = "local.get 0 if end" },
        { .folded = "(if (local.get 0) (then) (else))", .flat = "local.get 0 if else end" },
        { .folded = "(if $i (local.get 0) (then (br $i)) (else (br $i)))",
          .flat = "local.get 0 if $i br $i else br $i end" },
        { .folded = "(if (local.get 0) (local.get 0) (then if end))",
          .flat = "local.get 0 local.get 0 if if end end" },
        { .folded = "(try_table (catch $e 0) (catch_all 0) (nop))",
          .flat = "try_table (catch $e 0) (catch_all 0) nop end" },
        { .folded = "(select (result i32) (local.get 0) (local.get 0) (local.get 0))",
          .flat = "local.get 0 local.get 0 local.get 0 select (result i32)" },
        { .folded = "(select (local.get 0) (local.get 0) (local.get 0))",
          .flat = "local.get 0 local.get 0 local.get 0 select" },
        { .folded = "(call_indirect $u (type $ii) (local.get 0) (local.get 0))",
          .flat = "local.get 0 local.get 0 call_indirect $u (type $ii)" },
        { .folded = "(br_table 0 0 (local.get 0))", .flat = "local.get 0 br_table 0 0" },
        { .folded = "(v128.store offset=16 (local.get 0) (v128.const i32x4 1 2 3 4))",
          .flat = "local.get 0 v128.const i32x4 1 2 3 4 v128.store offset=16" },
        { .folded = "(i8x16.shuffle 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 (v128.const i64x2 1 2) (v128.const "
                    "i64x2 3 4))",
          .flat = "v128.const i64x2 1 2 v128.const i64x2 3 4 i8x16.shuffle 0 1 2 3 4 5 6 7 8 9 10 11 12 13 "
                  "14 15" },
        // An operator's immediates are read when it is emitted, after its operands: implicit types and v128
        // literals follow the body's order, as wasm-tools assigns them.
        { .folded = "(call_indirect (param i64) (i64.const 0) (call_indirect (param f32) (f32.const 0) "
                    "(i32.const 0)))",
          .flat = "i64.const 0 f32.const 0 i32.const 0 call_indirect (param f32) call_indirect (param i64)" },
        { .folded = "(if (param i64) (call_indirect (param f32) (i32.const 0)) (then))",
          .flat = "i32.const 0 call_indirect (param f32) if (param i64) end" },
    });
    for (auto const& row: rows)
    {
        INFO(row.folded);
        CHECK(compileBody(row.folded) == compileBody(row.flat));
    }

    // Constant expressions fold alike, wherever they are.
    CHECK(compileSource("(global i32 (i32.add (i32.const 1) (i32.const 2)))")
          == compileSource("(global i32 i32.const 1 i32.const 2 i32.add)"));
    CHECK(
        compileSource("(table 1 funcref) (func) (elem (i32.add (i32.const 0) (i32.const 0)) func 0)")
        == compileSource("(table 1 funcref) (func) (elem (offset i32.const 0 i32.const 0 i32.add) func 0)"));
}

TEST_CASE("ModuleReader.foldedUnfoldsIteratively")
{
    // 9'000 folded levels, near the default nesting cap: unfolding takes no stack frame per level.
    constexpr auto Depth = std::size_t { 9'000 };
    auto limits = TextLimits {};
    limits.maxNesting = 10'000;
    auto const module = readModule(
        "(func " + repeated("(i32.eqz ", Depth) + "(i32.const 0)" + std::string(Depth + 1, ')'), limits);
    auto const& body = module.code.at(0).body;
    REQUIRE(body.size() == Depth + 2);
    CHECK(body.front().opcode == Opcode::I32Const);
    CHECK(std::ranges::all_of(body | std::views::drop(1) | std::views::take(Depth),
                              [](auto const& instruction) { return instruction.opcode == Opcode::I32Eqz; }));
    CHECK(body.back().opcode == Opcode::End);

    // Folded ifs nest alike, each through its then part.
    auto const ifs = readModule(
        "(func " + repeated("(if (i32.const 0) (then ", Depth / 2) + std::string(Depth, ')') + ")", limits);
    CHECK(ifs.code.at(0).body.size() == (3 * (Depth / 2)) + 1);

    // 500'000 levels under a nesting cap raised to 1'000'000: far past any native stack a recursive
    // unfolder would take, and cheap for the iterative parser and reader.
    constexpr auto DeepDepth = std::size_t { 500'000 };
    auto deepLimits = TextLimits {};
    deepLimits.maxNesting = 1'000'000;
    auto const deep = readModule("(func " + repeated("(i32.eqz ", DeepDepth) + "(i32.const 0)"
                                     + std::string(DeepDepth + 1, ')'),
                                 deepLimits);
    CHECK(deep.code.at(0).body.size() == DeepDepth + 2);
}

TEST_CASE("ModuleReader.foldedBlocksTakeNoFlatEnd")
{
    // A folded block ends at its `)`: a flat `end` or `else` in its body is refused at that keyword, not
    // taken as closing it, which would leave its `)` reported as a missing end instead.
    struct Row
    {
        std::string_view source; ///< A module whose body has a stray flat end or else.
        uint32_t column;         ///< That keyword's column.
    };
    auto const rows = std::to_array<Row>({
        { .source = "(func (block end))", .column = 14 },
        { .source = "(func (if (i32.const 0) (then end)))", .column = 31 },
        { .source = "(func (if (i32.const 0) (then else)))", .column = 31 },
        { .source = "(func (if (i32.const 0) (then) (else end)))", .column = 38 },
        { .source = "(func (loop block (block end)))", .column = 26 },
    });
    for (auto const& row: rows)
    {
        INFO(row.source);
        auto const error = readError(row.source);
        CHECK(error.code == TextErrorCode::UnexpectedToken);
        CHECK(error.position.column == row.column);
    }
}

TEST_CASE("ModuleReader.foldedLabels")
{
    // A folded block's label binds in its body; a folded if's in its parts, not in its condition.
    auto const nested = readModule("(func (block $a (block $b (br $a))))");
    CHECK(nested.code[0].body[2].a == 1);
    CHECK(readError("(func (if $c (br $c) (then)))").code == TextErrorCode::UnknownLabel);
    auto const parts = readModule("(func (block $c (if $c (br $c) (then (br $c)) (else (br $c)))))");
    auto const& body = parts.code[0].body;
    CHECK(fieldsOf(body[1]) == Fields { .opcode = Opcode::Br, .a = 0 });
    CHECK(fieldsOf(body[3]) == Fields { .opcode = Opcode::Br, .a = 0 });
    CHECK(fieldsOf(body[5]) == Fields { .opcode = Opcode::Br, .a = 0 });

    // A label's index in the name section counts blocks in the body's order: a block in a folded if's
    // condition comes before the if.
    auto const names = namesOf(readModule("(func (if $c (block $b) (then)))"));
    auto const& labels = names.indirect[static_cast<std::size_t>(NameKind::Label)];
    REQUIRE(labels.size() == 1);
    CHECK(labels[0].names
          == std::vector<NameAssoc> { { .index = 0, .name = "b" }, { .index = 1, .name = "c" } });
}

TEST_CASE("ModuleReader.sourceOffsets")
{
    // Each instruction's offset: a flat one's keyword, a folded one's `(`, and a synthesized end's `)`.
    auto const body =
        std::string { "(func nop (drop (i32.const 1)) (block) (if (i32.const 0) (then) (else)))" };
    auto const result = readInstructions(body);
    auto const at = [&](std::string_view text, std::size_t from = 0) {
        return static_cast<uint32_t>(body.find(text, from));
    };
    REQUIRE(result.offsets.size() == result.code.body.size());
    CHECK(result.offsets
          == std::vector<uint32_t> { at("nop"),
                                     at("(i32.const 1)"),
                                     at("(drop"),
                                     at("(block"),
                                     at(")", at("(block")),
                                     at("(i32.const 0)"),
                                     at("(if"),
                                     at("(else"),
                                     at(")", at("(else")) + 1,
                                     static_cast<uint32_t>(body.size() - 1) });
    CHECK(result.code.body[2].opcode == Opcode::Drop);
    CHECK(result.code.body[8].opcode == Opcode::End);
}

namespace
{

/// The text of each value type an opcode row's fixture uses.
struct ValueTypeText
{
    ValueType type;        ///< The type.
    std::string_view text; ///< How the text format writes it.
};

/// Every value type of OpcodeRowModules' fixtures, as the text format writes it.
constexpr auto FixtureValueTypes = std::to_array<ValueTypeText>({
    { .type = isoline::binary::I32, .text = "i32" },
    { .type = isoline::binary::I64, .text = "i64" },
    { .type = isoline::binary::F32, .text = "f32" },
    { .type = isoline::binary::F64, .text = "f64" },
    { .type = isoline::binary::V128, .text = "v128" },
    { .type = isoline::binary::FuncRef, .text = "funcref" },
    { .type = isoline::binary::ExternRef, .text = "externref" },
    { .type = isoline::binary::ExnRef, .text = "exnref" },
    { .type = isoline::binary::NonNullFuncRef, .text = "(ref func)" },
    { .type = isoline::binary::testing::RefNullType0, .text = "(ref null 0)" },
});

/// The text of @p types, each after a space; a type no FixtureValueTypes row names fails the test.
std::string textOf(std::vector<ValueType> const& types)
{
    auto text = std::string {};
    for (auto const& type: types)
    {
        auto const row = std::ranges::find(FixtureValueTypes, type, &ValueTypeText::type);
        REQUIRE(row != FixtureValueTypes.end());
        text += ' ';
        text += row->text;
    }
    return text;
}

/// How the text format writes the zero immediate OpcodeRowModules gives a row of @p kind, after its
/// keyword: what zeroImmediate() and fixtureOfAccess() encode, a memory argument's natural alignment and
/// offset 0 being the text's defaults. Every other kind's rows have a SpecialBodies row.
std::string_view immediateText(isoline::binary::ImmediateKind kind)
{
    using enum isoline::binary::ImmediateKind;
    switch (kind)
    {
        case None:
        case MemArg: return "";
        case I32Const:
        case I64Const:
        case F32Const:
        case F64Const:
        case LaneIndex:
        case MemArgLane: return " 0";
        case V128Const: return " i32x4 0 0 0 0";
        case Shuffle: return " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0";
        case BlockType:
        case TryTable:
        case LabelIndex:
        case LabelTable:
        case FunctionIndex:
        case CallIndirect:
        case TypeIndex:
        case TagIndex:
        case SelectTypes:
        case LocalIndex:
        case GlobalIndex:
        case TableIndex:
        case TableCopy:
        case TableInit:
        case ElemIndex:
        case MemoryIndex:
        case MemoryCopy:
        case MemoryInit:
        case DataIndex:
        case HeapType: break;
    }
    FAIL("no zero immediate text for kind " << static_cast<int>(kind));
    return "";
}

/// The text body of a Special row's fixture that is no load or store, as fixtures() encodes it.
struct SpecialBody
{
    Opcode opcode;         ///< The row.
    std::string_view body; ///< Its function's body, folded where it takes operands.
};

/// Every fixtures() row's body in text: indices 0 name the context moduleOf() gives every module.
constexpr auto SpecialBodies = std::to_array<SpecialBody>({
    { .opcode = Opcode::Unreachable, .body = "unreachable" },
    { .opcode = Opcode::Block, .body = "(block)" },
    { .opcode = Opcode::Loop, .body = "(loop)" },
    { .opcode = Opcode::If, .body = "(if (local.get 0) (then))" },
    { .opcode = Opcode::Else, .body = "(if (i32.const 1) (then) (else))" },
    { .opcode = Opcode::Throw, .body = "throw 0" },
    { .opcode = Opcode::ThrowRef, .body = "(throw_ref (local.get 0))" },
    { .opcode = Opcode::End, .body = "block end" },
    { .opcode = Opcode::Br, .body = "br 0" },
    { .opcode = Opcode::BrIf, .body = "(br_if 0 (local.get 0))" },
    { .opcode = Opcode::BrTable, .body = "(br_table 0 (local.get 0))" },
    { .opcode = Opcode::Return, .body = "return" },
    { .opcode = Opcode::Call, .body = "call 0" },
    { .opcode = Opcode::CallIndirect, .body = "(call_indirect (type 0) (local.get 0))" },
    { .opcode = Opcode::ReturnCall, .body = "return_call 0" },
    { .opcode = Opcode::ReturnCallIndirect, .body = "(return_call_indirect (type 0) (local.get 0))" },
    { .opcode = Opcode::CallRef, .body = "(call_ref 0 (local.get 0))" },
    { .opcode = Opcode::ReturnCallRef, .body = "(return_call_ref 0 (local.get 0))" },
    { .opcode = Opcode::TryTable, .body = "(try_table (catch 0 0))" },
    { .opcode = Opcode::Drop, .body = "(drop (local.get 0))" },
    { .opcode = Opcode::Select, .body = "(select (local.get 0) (local.get 1) (local.get 2))" },
    { .opcode = Opcode::SelectTyped,
      .body = "(select (result i32) (local.get 0) (local.get 1) (local.get 2))" },
    { .opcode = Opcode::LocalGet, .body = "local.get 0" },
    { .opcode = Opcode::LocalSet, .body = "(local.set 0 (local.get 0))" },
    { .opcode = Opcode::LocalTee, .body = "(local.tee 0 (local.get 0))" },
    { .opcode = Opcode::GlobalGet, .body = "global.get 0" },
    { .opcode = Opcode::GlobalSet, .body = "(global.set 0 (local.get 0))" },
    { .opcode = Opcode::TableGet, .body = "(table.get 0 (local.get 0))" },
    { .opcode = Opcode::TableSet, .body = "(table.set 0 (local.get 0) (local.get 1))" },
    { .opcode = Opcode::TableInit, .body = "(table.init 0 0 (local.get 0) (local.get 1) (local.get 2))" },
    { .opcode = Opcode::ElemDrop, .body = "elem.drop 0" },
    { .opcode = Opcode::TableCopy, .body = "(table.copy 0 0 (local.get 0) (local.get 1) (local.get 2))" },
    { .opcode = Opcode::TableGrow, .body = "(table.grow 0 (local.get 0) (local.get 1))" },
    { .opcode = Opcode::TableSize, .body = "table.size 0" },
    { .opcode = Opcode::TableFill, .body = "(table.fill 0 (local.get 0) (local.get 1) (local.get 2))" },
    { .opcode = Opcode::MemorySize, .body = "memory.size" },
    { .opcode = Opcode::MemoryGrow, .body = "(memory.grow (local.get 0))" },
    { .opcode = Opcode::MemoryInit, .body = "(memory.init 0 (local.get 0) (local.get 1) (local.get 2))" },
    { .opcode = Opcode::DataDrop, .body = "data.drop 0" },
    { .opcode = Opcode::MemoryCopy, .body = "(memory.copy (local.get 0) (local.get 1) (local.get 2))" },
    { .opcode = Opcode::MemoryFill, .body = "(memory.fill (local.get 0) (local.get 1) (local.get 2))" },
    { .opcode = Opcode::RefNull, .body = "ref.null func" },
    { .opcode = Opcode::RefIsNull, .body = "(ref.is_null (local.get 0))" },
    { .opcode = Opcode::RefFunc, .body = "ref.func 0" },
    { .opcode = Opcode::RefAsNonNull, .body = "(ref.as_non_null (local.get 0))" },
    { .opcode = Opcode::BrOnNull, .body = "(block (br_on_null 0 (local.get 0)) (drop))" },
    { .opcode = Opcode::BrOnNonNull, .body = "(br_on_non_null 0 (local.get 0)) unreachable" },
});

/// The text body of @p row's fixture: its SpecialBodies row, or else the instruction folded over its
/// parameters, with its zero immediate.
std::string bodyTextOf(isoline::binary::OpcodeInfo const& row,
                       isoline::binary::testing::OpcodeRowFixture const& fixture)
{
    auto const special = std::ranges::find(SpecialBodies, row.opcode, &SpecialBody::opcode);
    if (special != SpecialBodies.end())
        return std::string { special->body };
    auto text = std::format("({}{}", isoline::text::textName(row.opcode), immediateText(row.immediate));
    for (auto const index: std::views::iota(std::size_t { 0 }, fixture.params.size()))
        text += std::format(" (local.get {})", index);
    return text + ")";
}

/// The text twin of moduleOf(@p fixture): its context, index 0 of each kind, and the function under
/// test, function 1 of type 1, whose body is @p body.
std::string moduleTextOf(isoline::binary::testing::OpcodeRowFixture const& fixture, std::string_view body)
{
    return std::format("(type (func)) (type (func (param{}) (result{}))) (func (type 0)) (func (type 1) {})"
                       " (table 1 funcref) (memory 1) (global (mut i32) (i32.const 0)) (tag (type 0))"
                       " (elem func 0) (data \"\")",
                       textOf(fixture.params),
                       textOf(fixture.results),
                       body);
}

} // namespace

TEST_CASE("ModuleReader.everyOpcodeRowRoundTrips")
{
    // Every row of the opcode table, written in text, compiles to its OpcodeRowModules module: the same
    // canonical bytes, which drop the binary's data count where no body needs one.
    using isoline::binary::OpcodeTable;
    auto const modules = isoline::binary::testing::opcodeRowModules();
    REQUIRE(modules.size() == OpcodeTable.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, OpcodeTable.size()))
    {
        auto const& row = OpcodeTable[index];
        auto const fixture = isoline::binary::testing::fixtureOf(row);
        auto const source = moduleTextOf(fixture, bodyTextOf(row, fixture));
        INFO(source);
        auto const text = compileText(source, TextLimits {});
        if (!text)
            INFO(specMessage(text.error()));
        REQUIRE(text.has_value());
        auto const expected = isoline::binary::canonicalize(modules[index], FeatureSet::all());
        auto const actual = isoline::binary::canonicalize(*text, FeatureSet::all());
        REQUIRE(expected.has_value());
        REQUIRE(actual.has_value());
        CHECK(*actual == *expected);
    }
}
