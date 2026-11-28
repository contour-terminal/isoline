// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/ConstantExpression.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/meta/Check.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::runtime
{

namespace
{
    using binary::Opcode;

    /// The width of an extended-const operation's integers.
    enum class IntegerWidth : uint8_t
    {
        Bits32, ///< i32: computed in 64 bits and truncated, which wraps as i32 arithmetic does.
        Bits64, ///< i64.
    };

    /// One of extended-const's operations: its opcode, its operands' width, and what it computes on
    /// their bits, wrapping (unsigned arithmetic, so no operand traps or overflows).
    struct ArithmeticRow
    {
        Opcode opcode;                         ///< The instruction.
        IntegerWidth width;                    ///< Its operands' and result's width.
        uint64_t (*apply)(uint64_t, uint64_t); ///< Its result's bits from its operands' bits.
    };

    /// Extended-const's six operations: i32 and i64 add, sub and mul.
    constexpr auto ArithmeticTable = std::to_array<ArithmeticRow>({
        { .opcode = Opcode::I32Add,
          .width = IntegerWidth::Bits32,
          .apply = [](uint64_t lhs, uint64_t rhs) { return lhs + rhs; } },
        { .opcode = Opcode::I32Sub,
          .width = IntegerWidth::Bits32,
          .apply = [](uint64_t lhs, uint64_t rhs) { return lhs - rhs; } },
        { .opcode = Opcode::I32Mul,
          .width = IntegerWidth::Bits32,
          .apply = [](uint64_t lhs, uint64_t rhs) { return lhs * rhs; } },
        { .opcode = Opcode::I64Add,
          .width = IntegerWidth::Bits64,
          .apply = [](uint64_t lhs, uint64_t rhs) { return lhs + rhs; } },
        { .opcode = Opcode::I64Sub,
          .width = IntegerWidth::Bits64,
          .apply = [](uint64_t lhs, uint64_t rhs) { return lhs - rhs; } },
        { .opcode = Opcode::I64Mul,
          .width = IntegerWidth::Bits64,
          .apply = [](uint64_t lhs, uint64_t rhs) { return lhs * rhs; } },
    });

    /// The bits of an integer operand of @p width. Precondition (checked): @p value is of that width.
    [[nodiscard]] uint64_t bitsOf(Value const& value, IntegerWidth width)
    {
        switch (width)
        {
            case IntegerWidth::Bits32:
                Require(std::holds_alternative<I32>(value));
                return std::get<I32>(value).bits;
            case IntegerWidth::Bits64:
                Require(std::holds_alternative<I64>(value));
                return std::get<I64>(value).bits;
        }
        // uncovered-begin(integer-width-exhausted)
        core::unreachable(); // every IntegerWidth is a case above
    }
    // uncovered-end(integer-width-exhausted)

    /// The value of @p width whose bits are @p bits, truncated to the width.
    [[nodiscard]] Value valueOf(uint64_t bits, IntegerWidth width)
    {
        switch (width)
        {
            case IntegerWidth::Bits32: return I32 { .bits = static_cast<uint32_t>(bits) };
            case IntegerWidth::Bits64: return I64 { .bits = bits };
        }
        // uncovered-begin(integer-width-exhausted)
        core::unreachable(); // every IntegerWidth is a case above
    }
    // uncovered-end(integer-width-exhausted)

    /// Applies the extended-const operation @p opcode to the two operands on top of @p stack, replacing
    /// them by its result. Precondition (checked): @p opcode is a row of ArithmeticTable, and the top
    /// two operands are of its width.
    void applyArithmetic(Opcode opcode, std::vector<Value>& stack)
    {
        auto const row = std::ranges::find(ArithmeticTable, opcode, &ArithmeticRow::opcode);
        Require(row != ArithmeticTable.end() && stack.size() >= 2);
        auto const rhs = bitsOf(stack.back(), row->width);
        stack.pop_back();
        auto const lhs = bitsOf(stack.back(), row->width);
        stack.back() = valueOf(row->apply(lhs, rhs), row->width);
    }
} // namespace

std::optional<Value> constantOf(binary::Module const& module, binary::Instruction const& instruction)
{
    switch (instruction.opcode)
    {
        case Opcode::I32Const: return I32 { static_cast<uint32_t>(instruction.c) };
        case Opcode::I64Const: return I64 { instruction.c };
        case Opcode::F32Const: return F32 { static_cast<uint32_t>(instruction.c) };
        case Opcode::F64Const: return F64 { instruction.c };
        case Opcode::V128Const: return v128FromBytes(binary::v128LiteralOf(module, instruction));
        default: return std::nullopt;
    }
}

Value nullOf(ModuleInstance const& instance, binary::Instruction const& instruction)
{
    auto const type =
        binary::mapTypeIndex(instruction.type, [&](uint32_t index) { return instance.types.at(index); });
    return Ref { .kind = RefKind::Null, .heap = type.heap, .address = 0 };
}

Value functionReference(ModuleInstance const& instance, uint32_t index)
{
    return Ref { .kind = RefKind::Func,
                 .heap = { .kind = binary::AbstractHeapType::Func },
                 .address = instance.functions.at(index) };
}

Value evaluateConstantExpression(Store const& store,
                                 uint32_t moduleInstance,
                                 binary::ConstExpr const& expression)
{
    // The decoder ends every constant expression with end, and only there.
    Require(!expression.empty() && expression.back().opcode == Opcode::End);
    auto const& instance = store.moduleInstance(moduleInstance);
    auto stack = std::vector<Value> {};
    for (auto const& instruction: std::span { expression }.first(expression.size() - 1))
    {
        if (auto const constant = constantOf(*instance.module, instruction))
        {
            stack.push_back(*constant);
            continue;
        }
        switch (instruction.opcode)
        {
            case Opcode::GlobalGet:
                stack.push_back(store.global(instance.globals.at(instruction.a)).value());
                break;
            case Opcode::RefNull: stack.push_back(nullOf(instance, instruction)); break;
            case Opcode::RefFunc: stack.push_back(functionReference(instance, instruction.a)); break;
            default: applyArithmetic(instruction.opcode, stack); break;
        }
    }
    Require(stack.size() == 1);
    return stack.back();
}

} // namespace isoline::runtime
