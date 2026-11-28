// SPDX-License-Identifier: Apache-2.0
#pragma once

// An evaluator of straight-line selected code: the subset of encoding rows the guard lowerings emit, over
// virtual registers of 64 bits and the flags CF and ZF, with every trap branch recorded and then passed by,
// as code running ahead of a mispredicted branch passes it. So a lowering is executed on concrete inputs and
// both its trap and the pointer its select leaves are observed, independently of the selector that emitted
// it. A row the evaluator does not know, or a flag read that an instruction since its compare left undefined
// (a shift's, a multiply's), fails the evaluation rather than guess. Test support, linked into no shipped
// binary.

#include <core/Assert.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <variant>

#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/x86_64/Encoder.hpp>
#include <isoline/compiler/x86_64/Instructions.hpp>
#include <isoline/runtime/Trap.hpp>

namespace isoline::compiler::x86_64::testing
{

/// What an evaluation saw.
struct Evaluation
{
    std::optional<runtime::TrapCode> trap {};  ///< The first trap branch taken; none when none was.
    std::optional<uint64_t> subject {};        ///< That trap's subject, for TrapIfSubject.
    std::map<uint32_t, uint64_t> registers {}; ///< Every virtual register's final value.
    /// How many instructions the evaluator did not know, or found reading a flag left undefined: 0 for a
    /// complete evaluation.
    uint32_t misunderstood = 0;

    /// The value of @p reg (Require: defined).
    [[nodiscard]] uint64_t valueOf(machine::VirtualRegister reg) const
    {
        auto const found = registers.find(std::to_underlying(reg));
        Require(found != registers.end());
        return found->second;
    }
};

/// Reads 8 or 4 bytes at an address, for a load the code makes.
using MemoryReader = std::function<uint64_t(uint64_t address, uint32_t bytes)>;

/// Evaluates @p block of @p function from @p inputs (virtual register number → value), its loads read by
/// @p read.
inline Evaluation evaluate(machine::MachineFunction const& function,
                           machine::BlockId block,
                           std::map<uint32_t, uint64_t> inputs,
                           MemoryReader const& read = {})
{
    auto evaluation = Evaluation { .registers = std::move(inputs) };
    auto carry = std::optional<bool> {};
    auto zero = std::optional<bool> {};
    auto sign = std::optional<bool> {};
    auto const valueOf = [&](machine::MachineOperand const& operand) -> uint64_t {
        if (auto const* immediate = std::get_if<machine::Immediate>(&operand))
            return static_cast<uint64_t>(immediate->value);
        auto const& name = std::get<machine::RegisterOperand>(operand).name;
        Require(name.kind == machine::RegisterKind::Virtual);
        return evaluation.registers.at(name.number);
    };
    auto const nameOf = [](machine::MachineOperand const& operand) {
        return std::get<machine::RegisterOperand>(operand).name.number;
    };
    auto const addressOf = [&](machine::MachineOperand const& operand) {
        auto const& memory = std::get<machine::MemoryOperand>(operand);
        auto address = static_cast<uint64_t>(memory.displacement);
        if (memory.base.kind == machine::RegisterKind::Virtual)
            address += evaluation.registers.at(memory.base.number);
        if (memory.index.kind == machine::RegisterKind::Virtual)
            address += evaluation.registers.at(memory.index.number) * memory.scale;
        return address;
    };
    auto const holds = [&](ConditionCode condition) {
        auto const known = [&](std::optional<bool> const& flag) {
            if (!flag)
                ++evaluation.misunderstood;
            return flag.value_or(false);
        };
        switch (condition)
        {
            case ConditionCode::Below: return known(carry);
            case ConditionCode::AboveOrEqual: return !known(carry);
            case ConditionCode::Equal: return known(zero);
            case ConditionCode::NotEqual: return !known(zero);
            case ConditionCode::Above: return !known(carry) && !known(zero);
            case ConditionCode::BelowOrEqual: return known(carry) || known(zero);
            case ConditionCode::Sign: return known(sign);
            default: ++evaluation.misunderstood; return false;
        }
    };
    auto const setLogical = [&](uint64_t result) {
        carry = false;
        zero = result == 0;
        sign = (result >> 63U) != 0;
    };
    auto const poisonFlags = [&] {
        carry.reset();
        zero.reset();
        sign.reset();
    };
    for (auto const index: function.instructions(block))
    {
        auto const operands = function.operands(index);
        auto const opcode = function.instruction(index).opcode;
        if (auto const pseudo = pseudoOf(opcode))
        {
            if (*pseudo != Pseudo::TrapIf && *pseudo != Pseudo::TrapIfSubject)
            {
                ++evaluation.misunderstood;
                continue;
            }
            auto const condition = static_cast<ConditionCode>(valueOf(operands[0]));
            if (holds(condition) && !evaluation.trap)
            {
                evaluation.trap = static_cast<runtime::TrapCode>(valueOf(operands[1]));
                if (*pseudo == Pseudo::TrapIfSubject)
                    evaluation.subject = valueOf(operands[2]);
            }
            continue;
        }
        auto const mnemonic = *mnemonicOf(opcode);
        auto& registers = evaluation.registers;
        switch (mnemonic)
        {
            case Mnemonic::Mov32RR:
                registers[nameOf(operands[0])] = valueOf(operands[1]) & 0xFFFF'FFFFU;
                break;
            case Mnemonic::Mov64RR: registers[nameOf(operands[0])] = valueOf(operands[1]); break;
            case Mnemonic::Mov32RI:
                registers[nameOf(operands[0])] = valueOf(operands[1]) & 0xFFFF'FFFFU;
                break;
            case Mnemonic::Mov64RI:
            case Mnemonic::Mov64RI32: registers[nameOf(operands[0])] = valueOf(operands[1]); break;
            case Mnemonic::Lea64RM: registers[nameOf(operands[0])] = addressOf(operands[1]); break;
            case Mnemonic::Mov64RM:
            case Mnemonic::Mov32RM: {
                Require(static_cast<bool>(read));
                registers[nameOf(operands[0])] =
                    read(addressOf(operands[1]), mnemonic == Mnemonic::Mov64RM ? 8 : 4);
                break;
            }
            case Mnemonic::Add64RR:
            case Mnemonic::Add64RI: {
                auto const a = valueOf(operands[0]);
                auto const b = valueOf(operands[1]);
                auto const result = a + b;
                registers[nameOf(operands[0])] = result;
                carry = result < a;
                zero = result == 0;
                sign = (result >> 63U) != 0;
                break;
            }
            case Mnemonic::Cmp64RR: {
                auto const a = valueOf(operands[0]);
                auto const b = valueOf(operands[1]);
                carry = a < b;
                zero = a == b;
                sign = ((a - b) >> 63U) != 0;
                break;
            }
            case Mnemonic::Test64RR: setLogical(valueOf(operands[0]) & valueOf(operands[1])); break;
            case Mnemonic::And64RI: {
                auto const result = valueOf(operands[0]) & valueOf(operands[1]);
                registers[nameOf(operands[0])] = result;
                setLogical(result);
                break;
            }
            case Mnemonic::Shl64RI8:
                registers[nameOf(operands[0])] = valueOf(operands[0]) << (valueOf(operands[1]) & 63U);
                poisonFlags();
                break;
            case Mnemonic::Shr64RI8:
                registers[nameOf(operands[0])] = valueOf(operands[0]) >> (valueOf(operands[1]) & 63U);
                poisonFlags();
                break;
            case Mnemonic::Imul64RRI:
                registers[nameOf(operands[0])] = valueOf(operands[1]) * valueOf(operands[2]);
                poisonFlags();
                break;
            case Mnemonic::Cmovcc64RR:
                if (holds(static_cast<ConditionCode>(valueOf(operands[0]))))
                    registers[nameOf(operands[1])] = valueOf(operands[2]);
                break;
            default: ++evaluation.misunderstood; break;
        }
    }
    return evaluation;
}

} // namespace isoline::compiler::x86_64::testing
