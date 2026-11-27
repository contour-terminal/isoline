// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/intermediate/Printer.hpp>

#include <core/Utils.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace isoline::compiler::intermediate
{

namespace
{
    /// Appends @p values to @p text as `vN`, separated by ", ", @p separator first when there are any.
    void appendValues(std::string& text, std::span<ValueId const> values, std::string_view separator)
    {
        for (auto const [position, value]: core::views::enumerate(values))
            std::format_to(std::back_inserter(text),
                           "{}v{}",
                           position == 0 ? separator : ", ",
                           std::to_underlying(value));
    }

    /// Appends @p values to @p text as `vN:type`, separated by ", ".
    void appendParameters(std::string& text, Function const& function, std::span<ValueId const> values)
    {
        for (auto const [position, value]: core::views::enumerate(values))
            std::format_to(std::back_inserter(text),
                           "{}v{}:{}",
                           position == 0 ? "" : ", ",
                           std::to_underlying(value),
                           typeRow(function.typeOf(value)).name);
    }

    /// Appends one instruction's line to @p text.
    void appendInstruction(std::string& text, Function const& function, Instruction const& instruction)
    {
        text += "  ";
        if (instruction.operation == Operation::Removed)
        {
            text += "removed\n";
            return;
        }
        auto const& row = operationRow(instruction.operation);
        auto const& schema = immediateSchemaRow(row.immediates);
        auto const results = function.operands(instruction.results);
        if (!results.empty())
        {
            appendValues(text, results, "");
            text += " = ";
        }
        text += row.text;

        // The operands no target takes: all of them, unless the instruction has targets.
        auto const targets = function.targets(instruction);
        auto const operands = function.operands(instruction.operands);
        auto const own = targets.empty() ? operands : operands.first(operandSignatureRow(row.operands).count);
        appendValues(text, own, " ");
        for (auto const [position, target]: core::views::enumerate(targets))
        {
            std::format_to(std::back_inserter(text),
                           "{}b{}(",
                           position == 0 && own.empty() ? " " : ", ",
                           std::to_underlying(target.block));
            appendValues(text, function.operands(target.arguments), "");
            text += ')';
        }

        // The immediates targets do not hold.
        auto separator = std::string_view { " [" };
        for (auto const [position, value]:
             core::views::enumerate(function.immediates(instruction.immediates)))
        {
            auto const field = immediateFieldAt(schema, position);
            if (field == ImmediateField::Target || field == ImmediateField::ArgumentCount)
                continue;
            auto const& fieldRow = immediateFieldRow(field);
            if (field == ImmediateField::Type)
                std::format_to(std::back_inserter(text),
                               "{}{}={}",
                               separator,
                               fieldRow.name,
                               typeRow(static_cast<Type>(value)).name);
            else
                std::format_to(std::back_inserter(text), "{}{}={}", separator, fieldRow.name, value);
            separator = ", ";
        }
        if (separator == ", ")
            text += ']';
        text += '\n';
    }
} // namespace

std::string print(Function const& function)
{
    auto text = std::string {};
    for (auto const index: std::views::iota(uint32_t { 0 }, function.blockCount()))
    {
        auto const& block = function.block(BlockId { index });
        std::format_to(std::back_inserter(text), "b{}(", index);
        appendParameters(text, function, function.operands(block.parameters));
        text += block.hotness == Hotness::Cold ? ") cold:\n" : "):\n";
        for (auto const instruction: block.instructions)
            appendInstruction(text, function, function.instruction(instruction));
    }
    return text;
}

} // namespace isoline::compiler::intermediate
