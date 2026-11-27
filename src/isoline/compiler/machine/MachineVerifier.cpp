// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/machine/MachineVerifier.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/compiler/intermediate/Function.hpp>

namespace isoline::compiler::machine
{

namespace
{
    /// The refusal of a function that breaks a rule: a compiler bug.
    [[nodiscard]] std::unexpected<CompileError> broken() noexcept
    {
        return std::unexpected { CompileError { .code = CompileErrorCode::InternalVerificationFailed,
                                                .pass = "machine-verifier" } };
    }

    /// Whether @p operand keeps @p row.
    [[nodiscard]] bool keeps(MachineOperand const& operand, OperandConstraint const& row) noexcept
    {
        if (auto const* reg = std::get_if<RegisterOperand>(&operand))
        {
            if (reg->role != row.role || reg->name.registerClass != row.registerClass)
                return false;
            return !row.fixed || reg->name.kind != RegisterKind::Physical || reg->name.number == *row.fixed;
        }
        if (std::holds_alternative<MemoryOperand>(operand))
            return row.memory == AllowsMemory::Yes;
        return true; // immediates, labels, stubs and constants: the row at their position is not read
    }

    /// Whether every instruction's operands keep their constraint rows.
    [[nodiscard]] bool operandsKeepTheirRows(MachineFunction const& function, ITarget const& target)
    {
        for (auto const index: std::views::iota(uint32_t { 0 }, function.instructionCount()))
        {
            auto const operands = function.operands(index);
            auto const rows = target.constraints(function.instruction(index).opcode);
            Require(!rows.empty() || operands.empty()); // a target's table gives every opcode its rows
            for (auto const position: std::views::iota(std::size_t { 0 }, operands.size()))
                if (!keeps(operands[position], rows[std::min(position, rows.size() - 1)]))
                    return false;
        }
        return true;
    }

    /// Whether every edge passes one argument of the right class per parameter of its target.
    [[nodiscard]] bool edgesMatchTheirTargets(MachineFunction const& function)
    {
        for (auto const block: std::views::iota(uint32_t { 0 }, function.blockCount()))
            for (auto const& edge: function.edges(BlockId { block }))
            {
                auto const arguments = function.arguments(edge);
                auto const parameters = function.parameters(edge.target);
                if (arguments.size() != parameters.size())
                    return false;
                for (auto const position: std::views::iota(std::size_t { 0 }, arguments.size()))
                    if (function.registerClass(arguments[position])
                        != function.registerClass(parameters[position]))
                        return false;
            }
        return true;
    }

    /// Whether no virtual register is defined in two blocks.
    [[nodiscard]] bool definedInOneBlock(MachineFunction const& function)
    {
        auto definingBlock = std::pmr::vector<uint32_t>(
            function.virtualRegisterCount(), intermediate::Unnumbered, &function.memory());
        auto isUnique = true;
        auto const define = [&](uint32_t reg, uint32_t block) {
            if (definingBlock[reg] != intermediate::Unnumbered && definingBlock[reg] != block)
                isUnique = false;
            definingBlock[reg] = block;
        };
        for (auto const block: std::views::iota(uint32_t { 0 }, function.blockCount()))
        {
            for (auto const parameter: function.parameters(BlockId { block }))
                define(std::to_underlying(parameter), block);
            for (auto const instruction: function.instructions(BlockId { block }))
                for (auto const& operand: function.operands(instruction))
                {
                    auto const* reg = std::get_if<RegisterOperand>(&operand);
                    if (reg != nullptr && reg->role != OperandRole::Use
                        && reg->name.kind == RegisterKind::Virtual)
                        define(reg->name.number, block);
                }
        }
        return isUnique;
    }
} // namespace

std::expected<void, CompileError> verifyMachineFunction(MachineFunction const& function,
                                                        ITarget const& target)
{
    if (!operandsKeepTheirRows(function, target) || !edgesMatchTheirTargets(function)
        || !definedInOneBlock(function))
        return broken();
    return {};
}

} // namespace isoline::compiler::machine
