// SPDX-License-Identifier: Apache-2.0
#pragma once

// One function per instruction the compiler compiles: a sample body for each OpcodeTable row of a feature
// compiledFeatures() holds, in a module that gives it what it names (a global, a table, a memory, an element
// and a data segment, a callee). The translation and optimization suites share them, so that a row added
// without a sample fails both.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/compiler/FeatureSupport.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/translation/MemoryTranslation.hpp>
#include <isoline/compiler/translation/NumericRules.hpp>
#include <isoline/compiler/translation/testing/Translation.hpp>

namespace isoline::compiler::translation::testing
{

using binary::AddressType;
using binary::FuncRef;
using binary::Limits;
using binary::MemoryType;
using binary::Opcode;
using binary::TableType;
using binary::testing::code;
using binary::testing::encodeU32;
using binary::testing::encodeU64;

/// The bytes of @p opcode: its byte, or its prefix and its sub-opcode.
inline Bytes opcodeBytes(Opcode opcode)
{
    auto const& info = isoline::binary::OpcodeTable[std::to_underlying(opcode)];
    if (info.prefix == isoline::binary::NoPrefix)
        return { static_cast<uint8_t>(info.code) };
    return code({ { info.prefix }, encodeU32(info.code) });
}

/// A load or store @p opcode of @p memory at @p offset, its alignment 0.
inline Bytes access(Opcode opcode, uint32_t memory, uint64_t offset)
{
    // A memory other than 0 sets bit 6 of the alignment and follows it with its index.
    auto const flags = memory == 0 ? Bytes { 0x00 } : code({ { 0x40 }, encodeU32(memory) });
    return code({ opcodeBytes(opcode), flags, encodeU64(offset) });
}

/// The bytes of @p opcode with its index immediates @p indices.
inline Bytes withIndices(Opcode opcode, std::initializer_list<uint32_t> indices)
{
    auto bytes = opcodeBytes(opcode);
    for (auto const index: indices)
        bytes = code({ bytes, encodeU32(index) });
    return bytes;
}

/// ref.null func.
inline Bytes refNullFunc()
{
    return { 0xD0, 0x70 };
}

/// The memory type of @p pages pages of @p address addresses and a page of 2^@p pageSizeLog2 bytes.
inline MemoryType memoryOf(AddressType address, uint8_t pageSizeLog2 = isoline::binary::DefaultPageSizeLog2)
{
    return MemoryType { .limits = Limits { .min = 1, .max = std::nullopt, .addressType = address },
                        .pageSizeLog2 = pageSizeLog2 };
}

/// A table of funcref of @p address addresses.
inline TableType tableOf(AddressType address)
{
    return TableType { .element = FuncRef,
                       .limits = Limits { .min = 2, .max = std::nullopt, .addressType = address } };
}

/// One instruction's sample: its row and a module whose function 0 executes it.
struct OpcodeSample
{
    binary::OpcodeInfo const* info; ///< The instruction's row.
    ModuleBuilder module;           ///< The module.
};

/// Where a sample's instruction runs.
enum class Placement : uint8_t
{
    Body,   ///< In the function's body.
    InLoop, ///< In a loop, after a br_if back to it: on a path where a poll changed the countdown.
};

/// A sample of every instruction of every feature compiledFeatures() holds, in OpcodeTable order. Fails the
/// test when a compiled numeric row has no operation (numericOperationOf()).
/// @param placement Where the instruction runs.
inline std::vector<OpcodeSample> everyCompiledOpcode(Placement placement = Placement::Body)
{
    using binary::I32;
    using binary::testing::drop;
    using binary::testing::i32Const;
    using binary::testing::i64Const;
    using binary::testing::localGet;
    auto samples = std::map<Opcode, Bytes> {
        { Opcode::Unreachable, { 0x00 } },
        { Opcode::Nop, { 0x01 } },
        { Opcode::Block, { 0x02, 0x40, 0x0B } },
        { Opcode::Loop, { 0x03, 0x40, 0x0B } },
        { Opcode::If, code({ i32Const(0), { 0x04, 0x40, 0x05, 0x0B } }) },
        { Opcode::Else, code({ i32Const(0), { 0x04, 0x40, 0x05, 0x0B } }) },
        { Opcode::End, {} },
        { Opcode::Br, { 0x02, 0x40, 0x0C, 0x00, 0x0B } },
        { Opcode::BrIf, code({ { 0x02, 0x40 }, i32Const(0), { 0x0D, 0x00, 0x0B } }) },
        { Opcode::BrTable, code({ { 0x02, 0x40 }, i32Const(0), { 0x0E, 0x01, 0x00, 0x00, 0x0B } }) },
        { Opcode::Return, { 0x0F } },
        { Opcode::Call, code({ i32Const(0), binary::testing::call(1), drop() }) },
        { Opcode::CallIndirect, code({ i32Const(0), i32Const(0), { 0x11, 0x01, 0x00 }, drop() }) },
        { Opcode::Drop, code({ i32Const(0), drop() }) },
        { Opcode::Select, code({ i32Const(1), i32Const(2), i32Const(0), { 0x1B }, drop() }) },
        { Opcode::SelectTyped,
          code({ i32Const(1), i32Const(2), i32Const(0), { 0x1C, 0x01, 0x7F }, drop() }) },
        { Opcode::LocalGet, code({ localGet(0), drop() }) },
        { Opcode::LocalSet, code({ i32Const(0), binary::testing::localSet(0) }) },
        { Opcode::LocalTee, code({ i32Const(0), { 0x22, 0x00 }, drop() }) },
        { Opcode::GlobalGet, code({ { 0x23, 0x00 }, drop() }) },
        { Opcode::GlobalSet, code({ i32Const(0), { 0x24, 0x00 } }) },
        { Opcode::TableGet, code({ i32Const(0), withIndices(Opcode::TableGet, { 0 }), drop() }) },
        { Opcode::TableSet, code({ i32Const(0), refNullFunc(), withIndices(Opcode::TableSet, { 0 }) }) },
        { Opcode::MemorySize, code({ withIndices(Opcode::MemorySize, { 0 }), drop() }) },
        { Opcode::MemoryGrow, code({ i32Const(1), withIndices(Opcode::MemoryGrow, { 0 }), drop() }) },
        { Opcode::I32Const, code({ i32Const(0), drop() }) },
        { Opcode::I64Const, code({ i64Const(0), drop() }) },
        { Opcode::F32Const, code({ { 0x43, 0, 0, 0, 0 }, drop() }) },
        { Opcode::F64Const, code({ { 0x44, 0, 0, 0, 0, 0, 0, 0, 0 }, drop() }) },
        { Opcode::RefNull, code({ refNullFunc(), drop() }) },
        { Opcode::RefIsNull, code({ refNullFunc(), { 0xD1 }, drop() }) },
        { Opcode::RefFunc, code({ { 0xD2, 0x01 }, drop() }) },
        { Opcode::MemoryInit,
          code({ i32Const(0), i32Const(0), i32Const(0), withIndices(Opcode::MemoryInit, { 0, 0 }) }) },
        { Opcode::DataDrop, withIndices(Opcode::DataDrop, { 0 }) },
        { Opcode::MemoryCopy,
          code({ i32Const(0), i32Const(0), i32Const(0), withIndices(Opcode::MemoryCopy, { 0, 0 }) }) },
        { Opcode::MemoryFill,
          code({ i32Const(0), i32Const(0), i32Const(0), withIndices(Opcode::MemoryFill, { 0 }) }) },
        { Opcode::TableInit,
          code({ i32Const(0), i32Const(0), i32Const(0), withIndices(Opcode::TableInit, { 0, 0 }) }) },
        { Opcode::ElemDrop, withIndices(Opcode::ElemDrop, { 0 }) },
        { Opcode::TableCopy,
          code({ i32Const(0), i32Const(0), i32Const(0), withIndices(Opcode::TableCopy, { 0, 0 }) }) },
        { Opcode::TableGrow,
          code({ refNullFunc(), i32Const(1), withIndices(Opcode::TableGrow, { 0 }), drop() }) },
        { Opcode::TableSize, code({ withIndices(Opcode::TableSize, { 0 }), drop() }) },
        { Opcode::TableFill,
          code({ i32Const(0), refNullFunc(), i32Const(1), withIndices(Opcode::TableFill, { 0 }) }) },
    };
    // Every scalar load and store, from its row: an address, the value a store stores, the access.
    for (auto const& row: MemoryAccessTable)
    {
        auto const value = [&row] {
            switch (row.type)
            {
                case intermediate::Type::I32: return i32Const(0);
                case intermediate::Type::I64: return i64Const(0);
                case intermediate::Type::F32: return Bytes { 0x43, 0, 0, 0, 0 };
                default: return Bytes { 0x44, 0, 0, 0, 0, 0, 0, 0, 0 };
            }
        }();
        samples[row.opcode] = row.kind == AccessKind::Load
                                  ? code({ i32Const(0), access(row.opcode, 0, 0), drop() })
                                  : code({ i32Const(0), value, access(row.opcode, 0, 0) });
    }
    // Every numeric instruction, from its operation's operand types.
    auto const constantOf = [](intermediate::OperandType type) {
        using intermediate::OperandType;
        switch (type)
        {
            case OperandType::I32: return i32Const(0);
            case OperandType::I64: return i64Const(0);
            case OperandType::F32: return Bytes { 0x43, 0, 0, 0, 0 };
            default: return Bytes { 0x44, 0, 0, 0, 0, 0, 0, 0, 0 };
        }
    };
    auto result = std::vector<OpcodeSample> {};
    for (auto const& info: binary::OpcodeTable)
    {
        if (!compiledFeatures().includes(info.required))
            continue;
        INFO(info.text);
        auto body = Bytes {};
        if (auto const found = samples.find(info.opcode); found != samples.end())
            body = found->second;
        else
        {
            auto const operation = numericOperationOf(info.opcode);
            REQUIRE(operation.has_value());
            auto const& row = intermediate::operationRow(*operation);
            for (auto const operand: intermediate::operandSignatureRow(row.operands).fixed())
                body = code({ body, constantOf(operand) });
            body = code({ body, opcodeBytes(info.opcode), drop() });
        }
        if (placement == Placement::InLoop)
            body =
                code({ { 0x03, 0x40 }, localGet(0), { 0x0D, 0x00 }, body, { 0x0B } }); // loop br_if 0 … end
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = {}, .results = {} });
        builder.type(FuncType { .params = { I32 }, .results = { I32 } });
        builder.global(binary::GlobalType { .type = I32, .mutability = binary::Mutability::Var },
                       i32Const(0));
        builder.function(0, { I32 }, body);
        builder.function(1, {}, localGet(0));
        builder.table(tableOf(AddressType::I32));
        builder.memory(memoryOf(AddressType::I32));
        builder.dataCount(1);
        builder.element({ 0x01, 0x00, 0x01, 0x01 });
        builder.data({ 0x01, 0x01, 0xAA });
        result.push_back(OpcodeSample { .info = &info, .module = std::move(builder) });
    }
    return result;
}

} // namespace isoline::compiler::translation::testing
