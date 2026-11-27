// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

// One generated module per row of the opcode table, each using the row's instruction in a function
// against a context every module provides. A fixed-signature row is typed by SignatureTable, a load or
// store by MemoryAccessTable, and every other Special row by a fixture here, so a new instruction
// lands with its module. The modules are canonical: encodeModule writes each back byte for byte.

namespace isoline::binary::testing
{

/// A Special row's module: the function under test's type and its body, which uses the row's
/// instruction against the context every module provides (contextOf).
struct OpcodeRowFixture
{
    Opcode opcode;                  ///< The row the fixture exercises.
    std::vector<ValueType> params;  ///< The function's parameter types.
    std::vector<ValueType> results; ///< The function's result types.
    std::vector<uint8_t> body;      ///< Its body without the final end.
};

/// (ref null 0): a nullable reference to the context's type 0, [] -> [].
inline constexpr auto RefNullType0 = concreteRef(0, Nullable::Yes);

/// Every Special row's fixture that is no load or store. Indices name the context: type, function,
/// table, memory, global, tag, element and data segment 0 each; locals 0, 1 and 2 are the parameters.
inline std::vector<OpcodeRowFixture> const& fixtures()
{
    static auto const table = std::vector<OpcodeRowFixture> {
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
[[nodiscard]] inline std::vector<uint8_t> opcodeBytes(OpcodeInfo const& row)
{
    if (row.prefix == NoPrefix)
        return { static_cast<uint8_t>(row.code) };
    auto bytes = std::vector<uint8_t> { row.prefix };
    std::ranges::copy(encodeU32(row.code), std::back_inserter(bytes));
    return bytes;
}

/// The zero immediate of a fixed-signature row's @p kind: a constant of zero, the zero vector, the
/// shuffle that picks lane 0 throughout, or lane index 0, each valid for every row of its kind.
[[nodiscard]] inline std::vector<uint8_t> zeroImmediate(ImmediateKind kind)
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
[[nodiscard]] inline ValueType valueTypeOf(ValueKind kind)
{
    return ValueType { .kind = kind };
}

/// The fixture of a row SignatureTable types: its parameters as locals 0.., then the instruction
/// with a zero immediate, whose results are the function's.
[[nodiscard]] inline OpcodeRowFixture fixtureOfSignature(OpcodeInfo const& row)
{
    auto const& signature = signatureInfo(row.signature);
    auto fixture = OpcodeRowFixture { .opcode = row.opcode, .params = {}, .results = {}, .body = {} };
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
[[nodiscard]] inline OpcodeRowFixture fixtureOfAccess(OpcodeInfo const& row, MemoryAccess const& access)
{
    auto const isLoad = access.access == MemoryAccessKind::Load;
    auto const takesValue = !isLoad || access.shape == VectorShape::Lane;
    auto fixture =
        OpcodeRowFixture { .opcode = row.opcode, .params = { I32 }, .results = {}, .body = { 0x20, 0x00 } };
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
[[nodiscard]] inline ModuleBuilder moduleOf(OpcodeRowFixture const& fixture)
{
    constexpr auto PassiveFuncRefsOfFunction0 = std::array<uint8_t, 4> { 0x01, 0x00, 0x01, 0x00 };
    constexpr auto PassiveEmptyData = std::array<uint8_t, 2> { 0x01, 0x00 };
    auto const onePage = Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 };
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

/// The fixture that exercises @p row; a Special row without one fails the running test case.
[[nodiscard]] inline OpcodeRowFixture fixtureOf(OpcodeInfo const& row)
{
    if (row.signature != OpcodeSignature::Special)
        return fixtureOfSignature(row);
    if (auto const* access = memoryAccessOf(row.opcode))
        return fixtureOfAccess(row, *access);
    auto const& table = fixtures();
    auto const fixture = std::ranges::find(table, row.opcode, &OpcodeRowFixture::opcode);
    REQUIRE(fixture != table.end());
    return *fixture;
}

/// One module per row of OpcodeTable, in its order: moduleOf(fixtureOf(row)), built.
/// @return The modules' bytes.
[[nodiscard]] inline std::vector<std::vector<std::byte>> opcodeRowModules()
{
    auto modules = std::vector<std::vector<std::byte>> {};
    modules.reserve(OpcodeTable.size());
    for (auto const& row: OpcodeTable)
        modules.push_back(moduleOf(fixtureOf(row)).build());
    return modules;
}

} // namespace isoline::binary::testing
