// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/ConstantExpression.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/ModuleCode.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidatedModule.hpp>

using isoline::FeatureSet;
using isoline::binary::AbstractHeapType;
using isoline::binary::ConstExpr;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::Instruction;
using isoline::binary::Mutability;
using isoline::binary::Opcode;
using isoline::binary::ValueKind;
using isoline::binary::ValueType;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::ModuleBuilder;
using isoline::runtime::Abrupt;
using isoline::runtime::evaluateConstantExpression;
using isoline::runtime::ExecutionEngine;
using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::FuncAddr;
using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::StoreServices;
using isoline::runtime::V128;
using isoline::runtime::Value;
using isoline::validate::ValidatedModule;

namespace
{

/// Runs no code: the modules of these tests have no start function.
class NoCodeExecutor final: public isoline::runtime::ICodeExecutor
{
  public:
    [[nodiscard]] ExecutionEngine engine() const noexcept override { return ExecutionEngine::Reference; }

    [[nodiscard]] std::expected<void, Abrupt> invoke(Store& /*store*/,
                                                     FuncAddr /*function*/,
                                                     std::span<Value const> /*args*/,
                                                     std::span<Value> /*results*/) override
    {
        FAIL("no function is invoked");
        return {};
    }
};

/// An instruction of a constant expression.
/// @param opcode The instruction.
/// @param immediate Its wide immediate (Instruction::c).
/// @param index Its index immediate (Instruction::a).
Instruction instruction(Opcode opcode, uint64_t immediate = 0, uint32_t index = 0)
{
    return Instruction { .opcode = opcode, .a = index, .c = immediate };
}

/// A store holding one instance of @p builder's module, to evaluate constant expressions in.
class Fixture
{
  public:
    /// Decodes, validates and instantiates @p builder's module, under WebAssembly 3.0.
    explicit Fixture(ModuleBuilder const& builder):
        _store { StoreLimits {}, StoreServices { .allocator = _allocator } }
    {
        auto validated =
            ValidatedModule::load(builder.build(), FeatureSet::wasm3(), isoline::binary::DecodeLimits {});
        REQUIRE(validated.has_value());
        auto executor = NoCodeExecutor {};
        auto const address = isoline::runtime::instantiate(_store, executor, std::move(*validated), {});
        REQUIRE(address.has_value());
        _address = *address;
    }

    /// Evaluates @p expression in the instance.
    [[nodiscard]] Value evaluate(ConstExpr const& expression)
    {
        return evaluateConstantExpression(_store, _address, expression);
    }

    /// The store address of the instance's function @p index.
    [[nodiscard]] FuncAddr function(uint32_t index) const
    {
        return _store.moduleInstance(_address).functions.at(index);
    }

  private:
    HeapMemoryAllocator _allocator;
    Store _store;
    uint32_t _address = 0;
};

/// A module of one function and an immutable i32 global of @p value.
ModuleBuilder moduleWithGlobal(int32_t value)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .function(0, {}, {})
        .global(GlobalType { .type = isoline::binary::I32, .mutability = Mutability::Const },
                i32Const(value));
    return builder;
}

} // namespace

TEST_CASE("ConstantExpression.evaluatesExtendedConst")
{
    auto fixture = Fixture { moduleWithGlobal(37) };
    CHECK(fixture.evaluate(ConstExpr { instruction(Opcode::GlobalGet),
                                       instruction(Opcode::I32Const, 5),
                                       instruction(Opcode::I32Add),
                                       instruction(Opcode::End) })
          == Value { I32 { .bits = 42 } });
}

TEST_CASE("ConstantExpression.chainsArithmetic")
{
    // (9 + 3) * 2 - 1: the operands are popped right first, so sub computes 24 - 1.
    auto fixture = Fixture { moduleWithGlobal(9) };
    CHECK(fixture.evaluate(ConstExpr { instruction(Opcode::GlobalGet),
                                       instruction(Opcode::I32Const, 3),
                                       instruction(Opcode::I32Add),
                                       instruction(Opcode::I32Const, 2),
                                       instruction(Opcode::I32Mul),
                                       instruction(Opcode::I32Const, 1),
                                       instruction(Opcode::I32Sub),
                                       instruction(Opcode::End) })
          == Value { I32 { .bits = 23 } });
}

TEST_CASE("ConstantExpression.arithmeticWraps")
{
    // Each of extended-const's six operations, on operands whose result wraps: none ever traps.
    struct Case
    {
        Opcode constant {};  ///< i32.const or i64.const.
        Opcode operation {}; ///< The operation.
        uint64_t lhs = 0;    ///< The first operand's bits.
        uint64_t rhs = 0;    ///< The second operand's bits.
        Value expected;      ///< The wrapped result.
    };
    auto const cases = std::to_array<Case>({
        { .constant = Opcode::I32Const,
          .operation = Opcode::I32Add,
          .lhs = 0xFFFF'FFFF,
          .rhs = 2,
          .expected = I32 { .bits = 1 } },
        { .constant = Opcode::I32Const,
          .operation = Opcode::I32Sub,
          .lhs = 1,
          .rhs = 2,
          .expected = I32 { .bits = 0xFFFF'FFFF } },
        { .constant = Opcode::I32Const,
          .operation = Opcode::I32Mul,
          .lhs = 0x8000'0001,
          .rhs = 2,
          .expected = I32 { .bits = 2 } },
        { .constant = Opcode::I64Const,
          .operation = Opcode::I64Add,
          .lhs = ~uint64_t { 0 },
          .rhs = 3,
          .expected = I64 { .bits = 2 } },
        { .constant = Opcode::I64Const,
          .operation = Opcode::I64Sub,
          .lhs = 0,
          .rhs = 1,
          .expected = I64 { .bits = ~uint64_t { 0 } } },
        { .constant = Opcode::I64Const,
          .operation = Opcode::I64Mul,
          .lhs = ~uint64_t { 0 },
          .rhs = 2,
          .expected = I64 { .bits = ~uint64_t { 1 } } },
    });
    auto fixture = Fixture { moduleWithGlobal(0) };
    for (auto const& row: cases)
    {
        CAPTURE(row.operation);
        CHECK(fixture.evaluate(ConstExpr { instruction(row.constant, row.lhs),
                                           instruction(row.constant, row.rhs),
                                           instruction(row.operation),
                                           instruction(Opcode::End) })
              == row.expected);
    }
}

TEST_CASE("ConstantExpression.constants")
{
    // Every constant instruction pushes its immediate's bits unchanged, a float NaN's payload included.
    auto fixture = Fixture { moduleWithGlobal(0) };
    CHECK(fixture.evaluate(
              ConstExpr { instruction(Opcode::I64Const, 0x1234'5678'9ABC'DEF0), instruction(Opcode::End) })
          == Value { I64 { .bits = 0x1234'5678'9ABC'DEF0 } });
    CHECK(fixture.evaluate(ConstExpr { instruction(Opcode::F32Const, 0x7FC0'0001), instruction(Opcode::End) })
          == Value { F32 { .bits = 0x7FC0'0001 } });
    CHECK(fixture.evaluate(
              ConstExpr { instruction(Opcode::F64Const, 0x7FF8'0000'0000'0001), instruction(Opcode::End) })
          == Value { F64 { .bits = 0x7FF8'0000'0000'0001 } });
}

TEST_CASE("ConstantExpression.vectorConstant")
{
    // v128.const reads its 16 bytes from the module's literals, lowest lane byte first.
    auto builder = ModuleBuilder {};
    auto body = std::vector<uint8_t> { 0xFD, 0x0C };
    for (auto const byte: std::views::iota(uint8_t { 0 }, uint8_t { 16 }))
        body.push_back(byte);
    body.push_back(0x1A);
    builder.type(FuncType {}).function(0, {}, body);
    auto fixture = Fixture { builder };
    CHECK(fixture.evaluate(ConstExpr { instruction(Opcode::V128Const), instruction(Opcode::End) })
          == Value { V128 { .lanes = { 0x0706'0504'0302'0100, 0x0F0E'0D0C'0B0A'0908 } } });
}

TEST_CASE("ConstantExpression.references")
{
    // ref.func names the instance's function by its store address; ref.null of an abstract heap is
    // the null of that heap.
    auto fixture = Fixture { moduleWithGlobal(0) };
    auto const function =
        fixture.evaluate(ConstExpr { instruction(Opcode::RefFunc), instruction(Opcode::End) });
    REQUIRE(std::holds_alternative<Ref>(function));
    CHECK(std::get<Ref>(function).kind == RefKind::Func);
    CHECK(std::get<Ref>(function).address == fixture.function(0));

    auto null = instruction(Opcode::RefNull);
    null.type = ValueType { .kind = ValueKind::Ref, .heap = { .kind = AbstractHeapType::Extern } };
    auto const value = fixture.evaluate(ConstExpr { null, instruction(Opcode::End) });
    REQUIRE(std::holds_alternative<Ref>(value));
    CHECK(std::get<Ref>(value).kind == RefKind::Null);
    CHECK(std::get<Ref>(value).heap.kind == AbstractHeapType::Extern);
}

TEST_CASE("ConstantExpression.nullOfAConcreteTypeNamesItsTypeId")
{
    // Types 0 and 1 are equal, so they share TypeId 0, and type 2 gets TypeId 1: ref.null 2 names the
    // TypeId, not the module's index.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .type(FuncType {})
        .type(FuncType { .params = { isoline::binary::I32 }, .results = {} })
        .function(0, {}, {});
    auto fixture = Fixture { builder };
    auto null = instruction(Opcode::RefNull);
    null.type =
        ValueType { .kind = ValueKind::Ref, .heap = { .kind = AbstractHeapType::Concrete, .typeIndex = 2 } };
    auto const value = fixture.evaluate(ConstExpr { null, instruction(Opcode::End) });
    REQUIRE(std::holds_alternative<Ref>(value));
    CHECK(std::get<Ref>(value).kind == RefKind::Null);
    CHECK(std::get<Ref>(value).heap.kind == AbstractHeapType::Concrete);
    CHECK(std::get<Ref>(value).heap.typeIndex == 1);
}
