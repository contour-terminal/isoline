// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_message.hpp>

#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>

// Field-by-field equality of decoded modules for tests, over normalize()d modules: what "the same
// module" means for a round trip through the encoder. It compares the decoder-derived fields too
// (Instruction::endIndex and elseIndex, Function::branchSites), which a module the text reader builds
// leaves default, so such a module is compared after decoding its encoding.

namespace isoline::binary::testing
{

namespace detail
{
    /// Reports @p field as the first difference through Catch2's UNSCOPED_INFO.
    /// @return false, so that a comparison can return it.
    [[nodiscard]] inline bool differs(std::string const& field)
    {
        UNSCOPED_INFO("the modules differ first in " << field);
        return false;
    }

    /// Whether two sequences have equal elements, reporting the first differing one as field[index].
    template <typename T, typename Same>
    [[nodiscard]] bool sameSequence(std::string_view field,
                                    std::span<T const> a,
                                    std::span<T const> b,
                                    Same same)
    {
        if (a.size() != b.size())
            return differs(std::format("{}.size() ({} and {})", field, a.size(), b.size()));
        for (auto const index: std::views::iota(std::size_t { 0 }, a.size()))
            if (!same(std::format("{}[{}]", field, index), a[index], b[index]))
                return false;
        return true;
    }

    /// Whether two sequences of values with operator== are equal, reporting the first difference.
    template <typename T>
    [[nodiscard]] bool sameValues(std::string_view field, std::span<T const> a, std::span<T const> b)
    {
        return sameSequence(
            field, a, b, [](std::string const& at, T const& x, T const& y) { return x == y || differs(at); });
    }

    /// Whether two values with operator== are equal, reporting @p field otherwise.
    template <typename T>
    [[nodiscard]] bool sameValue(std::string const& field, T const& a, T const& b)
    {
        return a == b || differs(field);
    }

    [[nodiscard]] inline auto tied(Limits const& limits) noexcept
    {
        return std::tie(limits.min, limits.max, limits.addressType);
    }

    [[nodiscard]] inline bool sameLimits(Limits const& a, Limits const& b) noexcept
    {
        return tied(a) == tied(b);
    }

    [[nodiscard]] inline bool sameTableType(TableType const& a, TableType const& b) noexcept
    {
        return a.element == b.element && sameLimits(a.limits, b.limits);
    }

    [[nodiscard]] inline bool sameMemoryType(MemoryType const& a, MemoryType const& b) noexcept
    {
        return sameLimits(a.limits, b.limits) && a.pageSizeLog2 == b.pageSizeLog2;
    }

    [[nodiscard]] inline bool sameGlobalType(GlobalType const& a, GlobalType const& b) noexcept
    {
        return a.type == b.type && a.mutability == b.mutability;
    }

    [[nodiscard]] inline bool sameInstruction(Instruction const& a, Instruction const& b) noexcept
    {
        return std::tie(a.opcode, a.blockKind, a.lane, a.a, a.b, a.endIndex, a.c, a.elseIndex, a.type)
               == std::tie(b.opcode, b.blockKind, b.lane, b.a, b.b, b.endIndex, b.c, b.elseIndex, b.type);
    }

    [[nodiscard]] inline bool sameExpression(std::string_view field, ConstExpr const& a, ConstExpr const& b)
    {
        return sameSequence(field,
                            std::span { a },
                            std::span { b },
                            [](std::string const& at, Instruction const& x, Instruction const& y) {
                                return sameInstruction(x, y) || differs(at);
                            });
    }

    [[nodiscard]] inline bool sameFunction(std::string const& field, Function const& a, Function const& b)
    {
        auto const sameRun = [](std::string const& at, LocalRun const& x, LocalRun const& y) {
            return (x.count == y.count && x.type == y.type) || differs(at);
        };
        // Branch site offsets count bytes, which differ between encodings; which instructions are
        // branch sites does not.
        auto const sameSite = [](std::string const& at, BranchSite const& x, BranchSite const& y) {
            return x.instruction == y.instruction || differs(at);
        };
        return sameValue(field + ".typeIndex", a.typeIndex, b.typeIndex)
               && sameSequence(field + ".locals", std::span { a.locals }, std::span { b.locals }, sameRun)
               && sameValue(field + ".localCount", a.localCount, b.localCount)
               && sameExpression(field + ".body", a.body, b.body)
               && sameValues(
                   field + ".brTableLabels", std::span { a.brTableLabels }, std::span { b.brTableLabels })
               && sameValues(field + ".selectTypes", std::span { a.selectTypes }, std::span { b.selectTypes })
               && sameValues(
                   field + ".catchClauses", std::span { a.catchClauses }, std::span { b.catchClauses })
               && sameSequence(field + ".branchSites",
                               std::span { a.branchSites },
                               std::span { b.branchSites },
                               sameSite);
    }

    [[nodiscard]] inline bool sameElement(std::string const& field,
                                          ElementSegment const& a,
                                          ElementSegment const& b)
    {
        if (a.mode != b.mode || a.table != b.table || a.type != b.type || a.items.index() != b.items.index())
            return differs(field);
        if (!sameExpression(field + ".offset", a.offset, b.offset))
            return false;
        if (auto const* indices = std::get_if<std::vector<uint32_t>>(&a.items))
            return sameValues(field + ".items",
                              std::span { *indices },
                              std::span { std::get<std::vector<uint32_t>>(b.items) });
        return sameSequence(field + ".items",
                            std::span { std::get<std::vector<ConstExpr>>(a.items) },
                            std::span { std::get<std::vector<ConstExpr>>(b.items) },
                            [](std::string const& at, ConstExpr const& x, ConstExpr const& y) {
                                return sameExpression(at, x, y);
                            });
    }

    /// Names every field of every struct the comparators below compare, so that adding a field to one
    /// breaks the build here until its comparator compares it too, and the count is updated. Never
    /// called: it only has to compile.
    inline void comparedFieldsAreComplete(Module const& module,
                                          Function const& function,
                                          Instruction const& instruction,
                                          Import const& import,
                                          Table const& table,
                                          Global const& global,
                                          Export const& entry,
                                          ElementSegment const& element,
                                          DataSegment const& data,
                                          CustomSection const& custom,
                                          BranchHint const& hint)
    {
        [[maybe_unused]] auto const& [types,
                                      imports,
                                      importModules,
                                      functions,
                                      tables,
                                      memories,
                                      tags,
                                      globals,
                                      exports,
                                      start,
                                      elements,
                                      dataCount,
                                      code,
                                      dataSegments,
                                      customs,
                                      v128Literals,
                                      branchHints,
                                      customErrors] = module;
        [[maybe_unused]] auto const& [typeIndex,
                                      locals,
                                      localCount,
                                      body,
                                      bodyOffset,
                                      brTableLabels,
                                      selectTypes,
                                      catchClauses,
                                      branchSites] = function;
        [[maybe_unused]] auto const& [opcode, blockKind, lane, a, b, endIndex, c, elseIndex, type] =
            instruction;
        [[maybe_unused]] auto const& [moduleIndex,
                                      name,
                                      kind,
                                      importType,
                                      importTable,
                                      importMemory,
                                      importGlobal] = import;
        [[maybe_unused]] auto const& [tableType, tableInit] = table;
        [[maybe_unused]] auto const& [globalType, globalInit] = global;
        [[maybe_unused]] auto const& [exportName, exportKind, exportIndex] = entry;
        [[maybe_unused]] auto const& [elementMode, elementTable, elementOffset, elementType, elementItems] =
            element;
        [[maybe_unused]] auto const& [dataMode, dataMemory, dataOffset, dataBytes] = data;
        [[maybe_unused]] auto const& [customName, customBytes, customAfter] = custom;
        [[maybe_unused]] auto const& [hintFunction, hintInstruction, hintLikelihood] = hint;
        [[maybe_unused]] auto const& [limitsMin, limitsMax, limitsAddressType] = import.table.limits;
        [[maybe_unused]] auto const& [tableElement, tableLimits] = import.table;
        [[maybe_unused]] auto const& [memoryLimits, memoryPageSize] = import.memory;
        [[maybe_unused]] auto const& [globalValueType, globalMutability] = import.global;
        [[maybe_unused]] auto const& [runCount, runType] = function.locals.front();
        [[maybe_unused]] auto const& [siteInstruction, siteOffset] = function.branchSites.front();
    }

    /// Compares two modules that are normalized already.
    [[nodiscard]] inline bool sameNormalized(Module const& a, Module const& b)
    {
        auto const sameImport = [&](std::string const& at, Import const& x, Import const& y) {
            return (importModuleOf(a, x) == importModuleOf(b, y) && x.name == y.name && x.kind == y.kind
                    && x.typeIndex == y.typeIndex && sameTableType(x.table, y.table)
                    && sameMemoryType(x.memory, y.memory) && sameGlobalType(x.global, y.global))
                   || differs(at);
        };
        auto const sameTable = [](std::string const& at, Table const& x, Table const& y) {
            return (sameTableType(x.type, y.type) || differs(at))
                   && sameExpression(at + ".init", x.init, y.init);
        };
        auto const sameMemory = [](std::string const& at, MemoryType const& x, MemoryType const& y) {
            return sameMemoryType(x, y) || differs(at);
        };
        auto const sameTag = [](std::string const& at, Tag const& x, Tag const& y) {
            return x.typeIndex == y.typeIndex || differs(at);
        };
        auto const sameGlobal = [](std::string const& at, Global const& x, Global const& y) {
            return (sameGlobalType(x.type, y.type) || differs(at))
                   && sameExpression(at + ".init", x.init, y.init);
        };
        auto const sameExport = [](std::string const& at, Export const& x, Export const& y) {
            return (x.name == y.name && x.kind == y.kind && x.index == y.index) || differs(at);
        };
        auto const sameData = [](std::string const& at, DataSegment const& x, DataSegment const& y) {
            return ((x.mode == y.mode && x.memory == y.memory && x.bytes == y.bytes) || differs(at))
                   && sameExpression(at + ".offset", x.offset, y.offset);
        };
        auto const sameCustom = [](std::string const& at, CustomSection const& x, CustomSection const& y) {
            return (x.name == y.name && x.bytes == y.bytes && x.after == y.after) || differs(at);
        };
        auto const sameHint = [](std::string const& at, BranchHint const& x, BranchHint const& y) {
            return (x.function == y.function && x.instruction == y.instruction
                    && x.likelihood == y.likelihood)
                   || differs(at);
        };
        // An error's offset is a byte position, which differs between encodings; what was refused does not.
        auto const sameError =
            [](std::string const& at, CustomSectionError const& x, CustomSectionError const& y) {
                return x.code == y.code || differs(at);
            };
        return sameValues("types", std::span { a.types }, std::span { b.types })
               && sameSequence("imports", std::span { a.imports }, std::span { b.imports }, sameImport)
               && sameValues("functions", std::span { a.functions }, std::span { b.functions })
               && sameSequence("tables", std::span { a.tables }, std::span { b.tables }, sameTable)
               && sameSequence("memories", std::span { a.memories }, std::span { b.memories }, sameMemory)
               && sameSequence("tags", std::span { a.tags }, std::span { b.tags }, sameTag)
               && sameSequence("globals", std::span { a.globals }, std::span { b.globals }, sameGlobal)
               && sameSequence("exports", std::span { a.exports }, std::span { b.exports }, sameExport)
               && sameValue("start", a.start, b.start)
               && sameSequence("elements", std::span { a.elements }, std::span { b.elements }, sameElement)
               && sameValue("dataCount", a.dataCount, b.dataCount)
               && sameSequence("code", std::span { a.code }, std::span { b.code }, sameFunction)
               && sameSequence("data", std::span { a.data }, std::span { b.data }, sameData)
               && sameSequence("customs", std::span { a.customs }, std::span { b.customs }, sameCustom)
               && sameValues("v128Literals", std::span { a.v128Literals }, std::span { b.v128Literals })
               && sameSequence(
                   "branchHints", std::span { a.branchHints }, std::span { b.branchHints }, sameHint)
               && sameSequence(
                   "customErrors", std::span { a.customErrors }, std::span { b.customErrors }, sameError);
    }
} // namespace detail

/// Whether @p a and @p b are the same module once normalize()d: field by field, comparing imports by
/// their resolved module name rather than Import::moduleIndex, and leaving out what depends on byte
/// positions (Function::bodyOffset, BranchSite::offset and CustomSectionError::offset). The first
/// difference is reported through Catch2's UNSCOPED_INFO, by field.
/// @param a One module.
/// @param b The other.
/// @return Whether they are the same.
[[nodiscard]] inline bool sameModule(Module const& a, Module const& b)
{
    return detail::sameNormalized(normalize(a), normalize(b));
}

} // namespace isoline::binary::testing
