// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/translation/MemoryTranslation.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace isoline::compiler::translation
{

std::optional<MemoryAccessRow> scalarAccessOf(binary::Opcode opcode) noexcept
{
    // The table is the run of opcodes from i32.load to i64.store32 (its static_assert).
    auto const first = std::to_underlying(MemoryAccessTable.front().opcode);
    auto const value = std::to_underlying(opcode);
    if (value < first || value > std::to_underlying(MemoryAccessTable.back().opcode))
        return std::nullopt;
    return MemoryAccessTable[value - first];
}

std::optional<StubCallRow> stubCallOf(binary::Opcode opcode) noexcept
{
    auto const row =
        std::ranges::lower_bound(StubCallTable, std::to_underlying(opcode), {}, [](StubCallRow const& entry) {
            return std::to_underlying(entry.opcode);
        });
    if (row == StubCallTable.end() || row->opcode != opcode)
        return std::nullopt;
    return *row;
}

} // namespace isoline::compiler::translation
