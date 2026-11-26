// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interp/ReferenceInterpreter.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/Validator.hpp>

// One generated module per row of the opcode table: each row's instruction must decode to that row,
// validate with the operand and result types its row names, and run. A fixed-signature row is typed
// by SignatureTable, a load or store by MemoryAccessTable, and every other Special row by a fixture
// here; a Special row without one fails the test, so a new instruction lands with its module. Running
// it proves the interpreter has a case or a table row for it: one without ends the process.

using isoline::FeatureSet;
using isoline::binary::decodeModule;
using isoline::binary::ExnRef;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::I32;
using isoline::binary::ImmediateKind;
using isoline::binary::Instruction;
using isoline::binary::Limits;
using isoline::binary::MemoryAccess;
using isoline::binary::MemoryAccessKind;
using isoline::binary::memoryAccessOf;
using isoline::binary::Module;
using isoline::binary::Mutability;
using isoline::binary::NonNullFuncRef;
using isoline::binary::Opcode;
using isoline::binary::OpcodeInfo;
using isoline::binary::OpcodeSignature;
using isoline::binary::OpcodeTable;
using isoline::binary::signatureInfo;
using isoline::binary::TableType;
using isoline::binary::ValueKind;
using isoline::binary::ValueType;
using isoline::binary::VectorShape;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::ModuleBuilder;
using isoline::validate::validateModule;

namespace
{

/// A Special row's module: the function under test's type and its body, which uses the row's
/// instruction against the context every module provides (contextOf).
struct Fixture
{
    Opcode opcode;                  ///< The row the fixture exercises.
    std::vector<ValueType> params;  ///< The function's parameter types.
    std::vector<ValueType> results; ///< The function's result types.
    std::vector<uint8_t> body;      ///< Its body without the final end.
};

/// (ref null 0): a nullable reference to the context's type 0, [] -> [].
constexpr auto RefNullType0 = isoline::binary::concreteRef(0, isoline::binary::Nullable::Yes);

/// Every Special row's fixture that is no load or store. Indices name the context: type, function,
/// table, memory, global, tag, element and data segment 0 each; locals 0, 1 and 2 are the parameters.
std::vector<Fixture> const& fixtures()
{
    static auto const table = std::vector<Fixture> {
        // Control instructions
        { .opcode = Opcode::Unreachable, .params = {}, .results = {}, .body = { 0x00 } },
        { .opcode = Opcode::Block, .params = {}, .results = {}, .body = { 0x02, 0x40, 0x0B } },
        { .opcode = Opcode::Loop, .params = {}, .results = {}, .body = { 0x03, 0x40, 0x0B } },
        { .opcode = Opcode::If, .params = { I32 }, .results = {}, .body = { 0x20, 0x00, 0x04, 0x40, 0x0B } },
        // A true condition, so that running the then-branch reaches the else.
        { .opcode = Opcode::Else,
          .params = {},
          .results = {},
          .body = { 0x41, 0x01, 0x04, 0x40, 0x05, 0x0B } },
        { .opcode = Opcode::Throw, .params = {}, .results = {}, .body = { 0x08, 0x00 } },
        { .opcode = Opcode::ThrowRef, .params = { ExnRef }, .results = {}, .body = { 0x20, 0x00, 0x0A } },
        { .opcode = Opcode::End, .params = {}, .results = {}, .body = { 0x02, 0x40, 0x0B } },
        { .opcode = Opcode::Br, .params = {}, .results = {}, .body = { 0x0C, 0x00 } },
        { .opcode = Opcode::BrIf, .params = { I32 }, .results = {}, .body = { 0x20, 0x00, 0x0D, 0x00 } },
        { .opcode = Opcode::BrTable,
          .params = { I32 },
          .results = {},
          .body = { 0x20, 0x00, 0x0E, 0x00, 0x00 } },
        { .opcode = Opcode::Return, .params = {}, .results = {}, .body = { 0x0F } },
        { .opcode = Opcode::Call, .params = {}, .results = {}, .body = { 0x10, 0x00 } },
        { .opcode = Opcode::CallIndirect,
          .params = { I32 },
          .results = {},
          .body = { 0x20, 0x00, 0x11, 0x00, 0x00 } },
        { .opcode = Opcode::ReturnCall, .params = {}, .results = {}, .body = { 0x12, 0x00 } },
        { .opcode = Opcode::ReturnCallIndirect,
          .params = { I32 },
          .results = {},
          .body = { 0x20, 0x00, 0x13, 0x00, 0x00 } },
        { .opcode = Opcode::CallRef,
          .params = { RefNullType0 },
          .results = {},
          .body = { 0x20, 0x00, 0x14, 0x00 } },
        { .opcode = Opcode::ReturnCallRef,
          .params = { RefNullType0 },
          .results = {},
          .body = { 0x20, 0x00, 0x15, 0x00 } },
        // try_table with one clause, catch of tag 0 to label 0 (the function's).
        { .opcode = Opcode::TryTable,
          .params = {},
          .results = {},
          .body = { 0x1F, 0x40, 0x01, 0x00, 0x00, 0x00, 0x0B } },

        // Parametric instructions
        { .opcode = Opcode::Drop, .params = { I32 }, .results = {}, .body = { 0x20, 0x00, 0x1A } },
        { .opcode = Opcode::Select,
          .params = { I32, I32, I32 },
          .results = { I32 },
          .body = { 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0x1B } },
        { .opcode = Opcode::SelectTyped,
          .params = { I32, I32, I32 },
          .results = { I32 },
          .body = { 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0x1C, 0x01, 0x7F } },

        // Variable instructions
        { .opcode = Opcode::LocalGet, .params = { I32 }, .results = { I32 }, .body = { 0x20, 0x00 } },
        { .opcode = Opcode::LocalSet, .params = { I32 }, .results = {}, .body = { 0x20, 0x00, 0x21, 0x00 } },
        { .opcode = Opcode::LocalTee,
          .params = { I32 },
          .results = { I32 },
          .body = { 0x20, 0x00, 0x22, 0x00 } },
        { .opcode = Opcode::GlobalGet, .params = {}, .results = { I32 }, .body = { 0x23, 0x00 } },
        { .opcode = Opcode::GlobalSet, .params = { I32 }, .results = {}, .body = { 0x20, 0x00, 0x24, 0x00 } },

        // Table instructions
        { .opcode = Opcode::TableGet,
          .params = { I32 },
          .results = { FuncRef },
          .body = { 0x20, 0x00, 0x25, 0x00 } },
        { .opcode = Opcode::TableSet,
          .params = { I32, FuncRef },
          .results = {},
          .body = { 0x20, 0x00, 0x20, 0x01, 0x26, 0x00 } },
        { .opcode = Opcode::TableInit,
          .params = { I32, I32, I32 },
          .results = {},
          .body = { 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0xFC, 0x0C, 0x00, 0x00 } },
        { .opcode = Opcode::ElemDrop, .params = {}, .results = {}, .body = { 0xFC, 0x0D, 0x00 } },
        { .opcode = Opcode::TableCopy,
          .params = { I32, I32, I32 },
          .results = {},
          .body = { 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0xFC, 0x0E, 0x00, 0x00 } },
        { .opcode = Opcode::TableGrow,
          .params = { FuncRef, I32 },
          .results = { I32 },
          .body = { 0x20, 0x00, 0x20, 0x01, 0xFC, 0x0F, 0x00 } },
        { .opcode = Opcode::TableSize, .params = {}, .results = { I32 }, .body = { 0xFC, 0x10, 0x00 } },
        { .opcode = Opcode::TableFill,
          .params = { I32, FuncRef, I32 },
          .results = {},
          .body = { 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0xFC, 0x11, 0x00 } },

        // Memory instructions other than loads and stores
        { .opcode = Opcode::MemorySize, .params = {}, .results = { I32 }, .body = { 0x3F, 0x00 } },
        { .opcode = Opcode::MemoryGrow,
          .params = { I32 },
          .results = { I32 },
          .body = { 0x20, 0x00, 0x40, 0x00 } },
        { .opcode = Opcode::MemoryInit,
          .params = { I32, I32, I32 },
          .results = {},
          .body = { 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0xFC, 0x08, 0x00, 0x00 } },
        { .opcode = Opcode::DataDrop, .params = {}, .results = {}, .body = { 0xFC, 0x09, 0x00 } },
        { .opcode = Opcode::MemoryCopy,
          .params = { I32, I32, I32 },
          .results = {},
          .body = { 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0xFC, 0x0A, 0x00, 0x00 } },
        { .opcode = Opcode::MemoryFill,
          .params = { I32, I32, I32 },
          .results = {},
          .body = { 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0xFC, 0x0B, 0x00 } },

        // Reference instructions
        { .opcode = Opcode::RefNull, .params = {}, .results = { FuncRef }, .body = { 0xD0, 0x70 } },
        { .opcode = Opcode::RefIsNull,
          .params = { FuncRef },
          .results = { I32 },
          .body = { 0x20, 0x00, 0xD1 } },
        { .opcode = Opcode::RefFunc, .params = {}, .results = { FuncRef }, .body = { 0xD2, 0x00 } },
        { .opcode = Opcode::RefAsNonNull,
          .params = { FuncRef },
          .results = { NonNullFuncRef },
          .body = { 0x20, 0x00, 0xD4 } },
        // Inside a block of no results, which the null branches to; the non-null reference is dropped.
        { .opcode = Opcode::BrOnNull,
          .params = { FuncRef },
          .results = {},
          .body = { 0x02, 0x40, 0x20, 0x00, 0xD5, 0x00, 0x1A, 0x0B } },
        // The non-null reference branches out as the function's result; the null falls through.
        { .opcode = Opcode::BrOnNonNull,
          .params = { FuncRef },
          .results = { NonNullFuncRef },
          .body = { 0x20, 0x00, 0xD6, 0x00, 0x00 } },
    };
    return table;
}

/// The bytes of @p row's opcode: its prefix, if any, then its code.
std::vector<uint8_t> opcodeBytes(OpcodeInfo const& row)
{
    if (row.prefix == isoline::binary::NoPrefix)
        return { static_cast<uint8_t>(row.code) };
    auto bytes = std::vector<uint8_t> { row.prefix };
    std::ranges::copy(encodeU32(row.code), std::back_inserter(bytes));
    return bytes;
}

/// The zero immediate of a fixed-signature row's @p kind: a constant of zero, the zero vector, the
/// shuffle that picks lane 0 throughout, or lane index 0, each valid for every row of its kind.
std::vector<uint8_t> zeroImmediate(ImmediateKind kind)
{
    switch (kind)
    {
        case ImmediateKind::I32Const:
        case ImmediateKind::I64Const:
        case ImmediateKind::LaneIndex: return { 0x00 };
        case ImmediateKind::F32Const: return std::vector<uint8_t>(4, 0x00);
        case ImmediateKind::F64Const: return std::vector<uint8_t>(8, 0x00);
        case ImmediateKind::V128Const:
        case ImmediateKind::Shuffle: return std::vector<uint8_t>(16, 0x00);
        default: return {};
    }
}

/// The value type of a number or vector @p kind.
ValueType valueTypeOf(ValueKind kind)
{
    return ValueType { .kind = kind };
}

/// The fixture of a row SignatureTable types: its parameters as locals 0.., then the instruction
/// with a zero immediate, whose results are the function's.
Fixture fixtureOfSignature(OpcodeInfo const& row)
{
    auto const& signature = signatureInfo(row.signature);
    auto fixture = Fixture { .opcode = row.opcode, .params = {}, .results = {}, .body = {} };
    for (auto const kind: signature.paramKinds())
    {
        fixture.body.push_back(0x20);
        std::ranges::copy(encodeU32(static_cast<uint32_t>(fixture.params.size())),
                          std::back_inserter(fixture.body));
        fixture.params.push_back(valueTypeOf(kind));
    }
    for (auto const kind: signature.resultKinds())
        fixture.results.push_back(valueTypeOf(kind));
    std::ranges::copy(opcodeBytes(row), std::back_inserter(fixture.body));
    std::ranges::copy(zeroImmediate(row.immediate), std::back_inserter(fixture.body));
    return fixture;
}

/// The fixture of a load or store: an i32 address, the value a store or a lane access takes, then the
/// instruction at its natural alignment, offset 0 and (a lane access) lane 0.
Fixture fixtureOfAccess(OpcodeInfo const& row, MemoryAccess const& access)
{
    auto const isLoad = access.access == MemoryAccessKind::Load;
    auto const takesValue = !isLoad || access.shape == VectorShape::Lane;
    auto fixture = Fixture { .opcode = row.opcode, .params = { I32 }, .results = {}, .body = { 0x20, 0x00 } };
    if (takesValue)
    {
        fixture.params.push_back(valueTypeOf(access.value));
        fixture.body.insert(fixture.body.end(), { 0x20, 0x01 });
    }
    if (isLoad)
        fixture.results.push_back(valueTypeOf(access.value));
    std::ranges::copy(opcodeBytes(row), std::back_inserter(fixture.body));
    std::ranges::copy(encodeU32(access.naturalAlignment()), std::back_inserter(fixture.body));
    fixture.body.push_back(0x00); // offset
    if (row.immediate == ImmediateKind::MemArgLane)
        fixture.body.push_back(0x00); // lane
    return fixture;
}

/// The module of @p fixture: the context every fixture may name, index 0 of each kind, and the
/// function under test, function 1 of type 1.
ModuleBuilder moduleOf(Fixture const& fixture)
{
    constexpr auto PassiveFuncRefsOfFunction0 = std::array<uint8_t, 4> { 0x01, 0x00, 0x01, 0x00 };
    constexpr auto PassiveEmptyData = std::array<uint8_t, 2> { 0x01, 0x00 };
    auto const onePage =
        Limits { .min = 1, .max = std::nullopt, .addressType = isoline::binary::AddressType::I32 };
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .type(FuncType { .params = fixture.params, .results = fixture.results })
        .function(0, {}, {})
        .function(1, {}, fixture.body)
        .table(TableType { .element = FuncRef, .limits = onePage })
        .memory(onePage)
        .global(GlobalType { .type = I32, .mutability = Mutability::Var }, { 0x41, 0x00 })
        .tag(0)
        .element({ PassiveFuncRefsOfFunction0.begin(), PassiveFuncRefsOfFunction0.end() })
        .dataCount(1)
        .data({ PassiveEmptyData.begin(), PassiveEmptyData.end() });
    return builder;
}

/// The fixture that exercises @p row.
Fixture fixtureOf(OpcodeInfo const& row)
{
    if (row.signature != OpcodeSignature::Special)
        return fixtureOfSignature(row);
    if (auto const* access = memoryAccessOf(row.opcode))
        return fixtureOfAccess(row, *access);
    auto const& table = fixtures();
    auto const fixture = std::ranges::find(table, row.opcode, &Fixture::opcode);
    REQUIRE(fixture != table.end());
    return *fixture;
}

} // namespace

TEST_CASE("Opcode.everyRowDecodes")
{
    auto const features = FeatureSet::all();
    auto specialRows = std::size_t { 0 };
    for (auto const& row: OpcodeTable)
    {
        INFO(row.text);
        if (row.signature == OpcodeSignature::Special && memoryAccessOf(row.opcode) == nullptr)
            ++specialRows;
        auto const fixture = fixtureOf(row);
        auto const bytes = moduleOf(fixture).build();
        auto module = decodeModule(bytes, features);
        REQUIRE(module.has_value());
        REQUIRE(module->code.size() == 2);
        // A fixed-signature row's instruction directly follows the local.get of each operand, so it must
        // decode as that very row; a fixture's is somewhere in its body.
        auto const& body = module->code[1].body;
        if (row.signature != OpcodeSignature::Special)
        {
            CHECK(body.at(fixture.params.size()).opcode == row.opcode);
        }
        else
        {
            // The fixture's own instruction, not the final end every body has.
            auto const own = std::span { body }.first(body.size() - 1);
            CHECK(std::ranges::find(own, row.opcode, &Instruction::opcode) != own.end());
        }
        auto const valid = validateModule(*module, features);
        REQUIRE(valid.has_value());

        // Any end of its own will do (results, a trap or an exception): only a missing case aborts.
        auto const allocator = isoline::runtime::UnlimitedAllocator {};
        auto store = isoline::runtime::Store { allocator };
        auto interpreter = isoline::interp::ReferenceInterpreter {};
        auto const instance = isoline::runtime::instantiate(
            store, interpreter, std::make_shared<Module const>(std::move(*module)), {});
        REQUIRE(instance.has_value());
        auto const function = store.moduleInstance(*instance).functions.at(1);
        auto args = std::vector<isoline::runtime::Value> {};
        std::ranges::transform(fixture.params, std::back_inserter(args), isoline::runtime::defaultValue);
        auto results = std::vector<isoline::runtime::Value>(fixture.results.size());
        std::ignore = interpreter.invoke(store, function, args, results);
    }
    // Every fixture serves a row: none is left over from an instruction since removed.
    CHECK(fixtures().size() == specialRows);
}

TEST_CASE("Opcode.everyFixedSignatureRowIsTypedByItsRow")
{
    // The module of a fixed-signature row stops validating when its function claims one more result
    // than the row pushes: the validator types the instruction by its row, not by its surroundings.
    auto const features = FeatureSet::all();
    for (auto const& row: OpcodeTable)
    {
        if (row.signature == OpcodeSignature::Special)
            continue;
        INFO(row.text);
        auto fixture = fixtureOfSignature(row);
        fixture.results.push_back(I32);
        auto const bytes = moduleOf(fixture).build();
        auto const module = decodeModule(bytes, features);
        REQUIRE(module.has_value());
        CHECK_FALSE(validateModule(*module, features).has_value());
    }
}
