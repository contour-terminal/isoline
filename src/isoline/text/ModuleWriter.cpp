// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/ModuleWriter.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/text/Annotations.hpp>
#include <isoline/text/Instructions.hpp>
#include <isoline/text/Keywords.hpp>
#include <isoline/text/Lexer.hpp>

namespace isoline::text
{

namespace
{
    using binary::BlockType;
    using binary::ConstExpr;
    using binary::CustomSection;
    using binary::ExternKind;
    using binary::Function;
    using binary::FuncType;
    using binary::ImmediateKind;
    using binary::Instruction;
    using binary::Limits;
    using binary::Module;
    using binary::NameAssoc;
    using binary::NameKind;
    using binary::NameSection;
    using binary::Opcode;
    using binary::SectionAnchor;
    using binary::SegmentMode;
    using binary::ValueType;

    /// The smallest page size exponent the text format cannot write: a page size is a u32 there.
    inline constexpr auto MaxWritablePageSizeLog2 = uint8_t { 32 };

    /// How the name section is written.
    enum class NameRendering : uint8_t
    {
        Raw,         ///< As an `@custom` annotation of its bytes; no identifier is printed.
        Identifiers, ///< As identifiers and `@name` annotations, which re-synthesize it.
    };

    /// How the branch hint section is written.
    enum class HintRendering : uint8_t
    {
        Raw,         ///< As an `@custom` annotation of its bytes.
        Annotations, ///< As a branch hint annotation before each hinted instruction.
    };

    /// The layout of one floating-point format: its mantissa and exponent widths.
    struct FloatFormat
    {
        uint32_t mantissaBits; ///< The bits of its fraction field.
        uint32_t exponentBits; ///< The bits of its biased exponent field.
    };

    inline constexpr auto F32Format = FloatFormat { .mantissaBits = 23, .exponentBits = 8 };
    inline constexpr auto F64Format = FloatFormat { .mantissaBits = 52, .exponentBits = 11 };

    /// The `v128.const` shape every vector is written in: four 32-bit lanes, in hex.
    inline constexpr auto WrittenVectorShape =
        *std::ranges::find(LaneShapes, std::string_view { "i32x4" }, &LaneShape::keyword);

    /// The text a float's bits are written as, exactly: a hex float (`0x1.8p+1`, a subnormal
    /// `0x0.…p-126`), `inf`, or `nan:0x…` with the payload, each signed by a leading `-`.
    /// @param bits The float's bits.
    /// @param format Its layout.
    /// @return The literal.
    std::string floatText(uint64_t bits, FloatFormat format)
    {
        auto const mantissaMask = (uint64_t { 1 } << format.mantissaBits) - 1;
        auto const exponentMask = (uint64_t { 1 } << format.exponentBits) - 1;
        auto const mantissa = bits & mantissaMask;
        auto const exponent = (bits >> format.mantissaBits) & exponentMask;
        auto const isNegative = ((bits >> (format.mantissaBits + format.exponentBits)) & 1U) != 0;
        auto text = std::string { isNegative ? "-" : "" };
        if (exponent == exponentMask)
            return mantissa == 0 ? text + "inf" : text + std::format("nan:{:#x}", mantissa);
        if (exponent == 0 && mantissa == 0)
            return text + "0x0p+0";
        // The fraction's hex digits, its bits shifted to fill the last one, trailing zeros dropped.
        auto const digits = (format.mantissaBits + 3) / 4;
        auto fraction = std::format("{:0{}x}", mantissa << ((digits * 4) - format.mantissaBits), digits);
        while (!fraction.empty() && fraction.back() == '0')
            fraction.pop_back();
        auto const bias = static_cast<int64_t>(exponentMask >> 1U);
        // A subnormal is 0.fraction times the least normal exponent; a normal one 1.fraction.
        auto const isSubnormal = exponent == 0;
        auto const power = isSubnormal ? 1 - bias : static_cast<int64_t>(exponent) - bias;
        text += isSubnormal ? "0x0" : "0x1";
        if (!fraction.empty())
            text += '.' + fraction;
        return text + std::format("p{:+}", power);
    }

    /// Whether a name is written `$name`: it is not empty, and every byte of it is an idchar.
    bool isPlainIdentifier(std::string_view name) noexcept
    {
        return !name.empty() && std::ranges::all_of(name, isIdentifierCharacter);
    }

    /// Appends @p bytes as a string literal: printable ASCII as is, `"` and `\` escaped as `\"` and `\\`,
    /// every other byte as a `\hh` escape, which reads back as that very byte.
    void appendString(std::string& out, std::span<std::byte const> bytes)
    {
        out += '"';
        for (auto const byte: bytes)
        {
            auto const value = std::to_integer<uint8_t>(byte);
            if (value == '"' || value == '\\')
                out += { '\\', static_cast<char>(value) };
            else if (value >= 0x20 && value < 0x7F)
                out += static_cast<char>(value);
            else
                out += std::format("\\{:02x}", value);
        }
        out += '"';
    }

    /// The bytes of a string.
    std::span<std::byte const> bytesOf(std::string_view text) noexcept
    {
        return std::as_bytes(std::span { text.data(), text.size() });
    }

    /// The decoration a name is printed with after its entity's keyword: ` $name`, ` $"…"`, or, for a
    /// name an identifier cannot hold (the empty name, or one a scope has bound already), ` (@name "…")`.
    std::string decorationOf(std::string_view name, std::set<std::string_view>* scope)
    {
        auto const isUnique = scope == nullptr || scope->insert(name).second;
        auto text = std::string {};
        if (name.empty() || !isUnique)
        {
            text += " (@name ";
            appendString(text, bytesOf(name));
            return text + ')';
        }
        if (isPlainIdentifier(name))
            return std::format(" ${}", name);
        text += " $";
        appendString(text, bytesOf(name));
        return text;
    }

    /// Whether the reader binds the names of a subsection in one scope, so that a second entity of a
    /// name must be named by an annotation: every module-level space, and each function's locals.
    /// Labels, and the parameters of types and tags, are named without binding.
    bool bindsInOneScope(NameKind kind) noexcept
    {
        return kind != NameKind::Label && kind != NameKind::Parameter && kind != NameKind::TagParameter;
    }

    /// The names a module is written with, and what each entity's decoration is.
    struct Names
    {
        NameRendering rendering = NameRendering::Raw; ///< Whether the section is written as names.
        NameSection section;                          ///< The decoded section; empty when Raw.
        std::string module;                           ///< The module's decoration.
        /// Per Direct kind, the decoration of each of section.direct's entries.
        std::array<std::vector<std::string>, binary::NameKindCount> direct;
        /// Per Indirect kind, per entry of section.indirect, the decoration of each of its names.
        std::array<std::vector<std::vector<std::string>>, binary::NameKindCount> indirect;
    };

    /// The entry of a name map for @p index, by binary search: decoded maps ascend by index.
    template <typename Entry>
    std::optional<std::size_t> entryIndexOf(std::vector<Entry> const& entries, uint32_t index)
    {
        auto const found = std::ranges::lower_bound(entries, index, {}, &Entry::index);
        if (found == entries.end() || found->index != index)
            return std::nullopt;
        return static_cast<std::size_t>(found - entries.begin());
    }

    /// The index-space sizes and per-entity types a module's names are checked against.
    struct Spaces
    {
        std::vector<uint32_t> functionTypes; ///< The type index of every function, imported ones first.
        std::vector<uint32_t> tagTypes;      ///< The type index of every tag, imported ones first.
        std::size_t importedFunctions = 0;   ///< How many of functionTypes are imported.
        std::size_t importedTags = 0;        ///< How many of tagTypes are imported.
        std::array<std::size_t, binary::NameKindCount> counts {}; ///< Per Direct kind, its space's size.
    };

    Spaces spacesOf(Module const& module)
    {
        auto spaces = Spaces {};
        auto& counts = spaces.counts;
        for (auto const& import: module.imports)
        {
            switch (import.kind)
            {
                case ExternKind::Func: spaces.functionTypes.push_back(import.typeIndex); break;
                case ExternKind::Tag: spaces.tagTypes.push_back(import.typeIndex); break;
                case ExternKind::Table: ++counts[std::to_underlying(NameKind::Table)]; break;
                case ExternKind::Memory: ++counts[std::to_underlying(NameKind::Memory)]; break;
                case ExternKind::Global: ++counts[std::to_underlying(NameKind::Global)]; break;
            }
        }
        spaces.importedFunctions = spaces.functionTypes.size();
        spaces.importedTags = spaces.tagTypes.size();
        spaces.functionTypes.insert(
            spaces.functionTypes.end(), module.functions.begin(), module.functions.end());
        std::ranges::transform(module.tags, std::back_inserter(spaces.tagTypes), &binary::Tag::typeIndex);
        counts[std::to_underlying(NameKind::Type)] = module.types.size();
        counts[std::to_underlying(NameKind::Function)] = spaces.functionTypes.size();
        counts[std::to_underlying(NameKind::Table)] += module.tables.size();
        counts[std::to_underlying(NameKind::Memory)] += module.memories.size();
        counts[std::to_underlying(NameKind::Global)] += module.globals.size();
        counts[std::to_underlying(NameKind::Element)] = module.elements.size();
        counts[std::to_underlying(NameKind::Data)] = module.data.size();
        counts[std::to_underlying(NameKind::Tag)] = spaces.tagTypes.size();
        return spaces;
    }

    /// How many labels a body binds: one per block, loop, if and try_table.
    std::size_t labelCountOf(Function const& function)
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(function.body, [](Instruction const& instruction) {
                auto const kind = binary::opcodeInfo(instruction.opcode).immediate;
                return kind == ImmediateKind::BlockType || kind == ImmediateKind::TryTable;
            }));
    }

    /// The parameter count of the type at @p typeIndex, or nullopt when the module has no such type.
    std::optional<std::size_t> paramCountOf(Module const& module, uint32_t typeIndex)
    {
        if (typeIndex >= module.types.size())
            return std::nullopt;
        return module.types[typeIndex].params.size();
    }

    /// How many names an Indirect entry's outer entity may hold, or nullopt when the reader cannot name
    /// it at all: a local of a function of a known type, a label of a defined function, a parameter of a
    /// type, or of a defined tag of a known type.
    std::optional<std::size_t> innerLimitOf(Module const& module,
                                            Spaces const& spaces,
                                            NameKind kind,
                                            uint32_t outer)
    {
        switch (kind)
        {
            case NameKind::Local: {
                if (outer >= spaces.functionTypes.size())
                    return std::nullopt;
                auto const params = paramCountOf(module, spaces.functionTypes[outer]);
                if (!params || outer < spaces.importedFunctions)
                    return params;
                return *params + module.code.at(outer - spaces.importedFunctions).localCount;
            }
            case NameKind::Label:
                if (outer < spaces.importedFunctions || outer >= spaces.functionTypes.size())
                    return std::nullopt;
                return labelCountOf(module.code.at(outer - spaces.importedFunctions));
            case NameKind::Parameter: return paramCountOf(module, outer);
            case NameKind::TagParameter:
                if (outer < spaces.importedTags || outer >= spaces.tagTypes.size())
                    return std::nullopt;
                return paramCountOf(module, spaces.tagTypes[outer]);
            // uncovered-begin(non-indirect-kind)
            case NameKind::Module:
            case NameKind::Function:
            case NameKind::Type:
            case NameKind::Table:
            case NameKind::Memory:
            case NameKind::Global:
            case NameKind::Element:
            case NameKind::Data:
            case NameKind::Tag:
                break;
                // uncovered-end(non-indirect-kind)
        }
        // Only an Indirect row has inner names, and namesAreInRange() asks of no other.
        core::unreachable(); // uncovered(non-indirect-kind)
    } // uncovered(non-indirect-kind)

    /// Whether every entry of @p names names an entity the reader can name in @p module, and each
    /// Indirect entry holds a name: what reading identifiers back re-synthesizes exactly.
    bool namesAreInRange(Module const& module, NameSection const& names)
    {
        auto const spaces = spacesOf(module);
        for (auto const& row: binary::NameSubsections)
        {
            auto const kind = std::to_underlying(row.kind);
            if (row.shape == binary::NameShape::Direct)
            {
                auto const& entries = names.direct[kind];
                if (!entries.empty() && entries.back().index >= spaces.counts[kind])
                    return false;
                continue;
            }
            if (row.shape != binary::NameShape::Indirect)
                continue;
            for (auto const& entry: names.indirect[kind])
            {
                auto const limit = innerLimitOf(module, spaces, row.kind, entry.index);
                if (!limit || entry.names.empty() || entry.names.back().index >= *limit)
                    return false;
            }
        }
        return true;
    }

    /// Whether @p custom is the branch hint section and the hints are written as annotations, so that
    /// it is not written raw.
    bool isSynthesizedHintSection(CustomSection const& custom, HintRendering hints) noexcept
    {
        return hints == HintRendering::Annotations && custom.name == binary::BranchHintSectionName;
    }

    /// The anchor a custom section anchored at @p anchor gets in the module reading the text back
    /// builds: the data count section is written only when a function body needs it (the text rule),
    /// so a section anchored to a redundant one sits after the section before it.
    SectionAnchor textAnchorOf(Module const& module, SectionAnchor anchor) noexcept
    {
        auto const keepsDataCount = module.dataCount.has_value() && binary::codeNamesDataSegment(module);
        if (anchor == SectionAnchor::DataCount && !keepsDataCount)
            anchor = SectionAnchor::Element;
        return binary::writtenAnchor(module, anchor);
    }

    /// Whether a function's local runs are as reading text back makes them: none empty, no two adjacent
    /// ones of one type. Only then does its body encode at the offsets its decoded hints name.
    bool hasTextLocalRuns(Function const& function) noexcept
    {
        auto const isEmpty = [](binary::LocalRun const& run) {
            return run.count == 0;
        };
        auto const isSameType = [](binary::LocalRun const& left, binary::LocalRun const& right) {
            return left.type == right.type;
        };
        return std::ranges::none_of(function.locals, isEmpty)
               && std::ranges::adjacent_find(function.locals, isSameType) == function.locals.end();
    }

    /// How a module's branch hint section is written: as annotations exactly when the text re-synthesizes
    /// it byte for byte at its place. The section is the module's only one, read whole, with hints; it
    /// sits where binary::SynthesizedSections puts the rebuilt one, last in the slot before the code
    /// section; its hinted functions' locals are as the text gives them; and its payload is what the
    /// encoder builds from Module::branchHints (binary::encodeBranchHints, which encodes the hinted
    /// bodies alone, never the module).
    HintRendering hintRenderingOf(Module const& module)
    {
        auto const isHints = [](CustomSection const& custom) {
            return custom.name == binary::BranchHintSectionName;
        };
        auto const isRefused =
            std::ranges::any_of(module.customErrors, [](binary::CustomSectionError const& error) {
                return binary::customSectionOf(error.code) == binary::BranchHintSectionName;
            });
        if (std::ranges::count_if(module.customs, isHints) != 1 || isRefused || module.branchHints.empty())
            return HintRendering::Raw;
        auto const section = std::ranges::find_if(module.customs, isHints);
        auto const slot = textAnchorOf(module, SectionAnchor::DataCount);
        auto const next = std::next(section);
        auto const isLastInSlot = next == module.customs.end() || textAnchorOf(module, next->after) != slot;
        if (textAnchorOf(module, section->after) != slot || !isLastInSlot)
            return HintRendering::Raw;
        auto const importedFunctions = module.importedCount(ExternKind::Func);
        // Each hinted function once: the decoder keeps hints ordered by function.
        auto checked = std::optional<uint32_t> {};
        for (auto const& hint: module.branchHints)
        {
            if (checked == hint.function)
                continue;
            checked = hint.function;
            if (!hasTextLocalRuns(module.code.at(hint.function - importedFunctions)))
                return HintRendering::Raw;
        }
        if (binary::encodeBranchHints(module) != section->bytes)
            return HintRendering::Raw;
        return HintRendering::Annotations;
    }

    /// How a module's name section is written, and its names: as identifiers exactly when reading them
    /// back re-synthesizes the identical section at the identical place. The section must be the module's
    /// only one, after every custom section written raw and in the data section's slot, where the reader
    /// appends it (binary::SynthesizedSections); decode; name something; and name only what the reader can
    /// name.
    Names namesOf(Module const& module, HintRendering hints)
    {
        auto names = Names {};
        auto const isName = [](CustomSection const& custom) {
            return custom.name == binary::NameSectionName;
        };
        auto const found = std::ranges::find_if(module.customs, isName);
        if (found == module.customs.end() || std::ranges::count_if(module.customs, isName) != 1)
            return names;
        auto const isLast =
            std::ranges::all_of(std::next(found), module.customs.end(), [&](CustomSection const& custom) {
                return isSynthesizedHintSection(custom, hints);
            });
        if (!isLast
            || binary::writtenAnchor(module, found->after)
                   != binary::writtenAnchor(module, SectionAnchor::Data))
            return names;
        auto section = binary::decodeNameSection(found->bytes);
        if (!section || *section == NameSection {} || !namesAreInRange(module, *section))
            return names;

        names.rendering = NameRendering::Identifiers;
        names.section = std::move(*section);
        if (names.section.module)
            names.module = decorationOf(*names.section.module, nullptr);
        for (auto const& row: binary::NameSubsections)
        {
            auto const kind = std::to_underlying(row.kind);
            if (row.shape == binary::NameShape::Direct)
            {
                auto scope = std::set<std::string_view> {};
                for (auto const& entry: names.section.direct[kind])
                    names.direct[kind].push_back(decorationOf(entry.name, &scope));
            }
            else if (row.shape == binary::NameShape::Indirect)
            {
                for (auto const& entry: names.section.indirect[kind])
                {
                    auto scope = std::set<std::string_view> {};
                    auto& decorations = names.indirect[kind].emplace_back();
                    for (auto const& name: entry.names)
                        decorations.push_back(
                            decorationOf(name.name, bindsInOneScope(row.kind) ? &scope : nullptr));
                }
            }
        }
        return names;
    }

    /// The decorations of one Indirect entry's names: its map and their decorations, or none.
    struct InnerNames
    {
        std::span<NameAssoc const> names;         ///< The names, ascending by index.
        std::span<std::string const> decorations; ///< Their decorations.

        /// The decoration of the inner entity @p index, or the empty string without a name.
        [[nodiscard]] std::string_view of(uint32_t index) const
        {
            auto const found = std::ranges::lower_bound(names, index, {}, &NameAssoc::index);
            if (found == names.end() || found->index != index)
                return {};
            return decorations[static_cast<std::size_t>(found - names.begin())];
        }

        /// Whether any inner entity below @p count is named.
        [[nodiscard]] bool namesAnyBelow(std::size_t count) const noexcept
        {
            return !names.empty() && names.front().index < count;
        }
    };

    /// Writes one module. Every write goes through put(), which refuses what would pass the output
    /// limit; once one is refused, nothing more is written, and the loops stop at their next check.
    class Printer
    {
      public:
        Printer(Module const& module, WriterOptions const& options, uint32_t baseLevel):
            _module { module },
            _options { options },
            _baseLevel { baseLevel },
            _hints { hintRenderingOf(module) },
            _names { namesOf(module, _hints) },
            _importedFunctions { static_cast<uint32_t>(module.importedCount(ExternKind::Func)) }
        {
        }

        [[nodiscard]] std::expected<std::string, WriteError> print()
        {
            writeModule();
            if (_error)
                return std::unexpected { *_error };
            return std::move(_out);
        }

      private:
        // -----------------------------------------------------------------------------------------
        // Output

        void put(std::string_view text)
        {
            if (isStopped())
                return;
            if (text.size() > _options.maxTextBytes - _out.size())
            {
                _error = WriteError::TextTooLarge;
                return;
            }
            _out += text;
        }

        void put(char character) { put(std::string_view { &character, 1 }); }

        /// Whether writing has failed, so that nothing more is written.
        [[nodiscard]] bool isStopped() const noexcept { return _error.has_value(); }

        /// Starts a line at @p level: its indentation, capped at WriterOptions::maxIndentLevel.
        void startLine(uint32_t level)
        {
            auto const capped = std::min(level, _options.maxIndentLevel);
            auto const width = std::size_t { capped } * _options.indentWidth;
            if (isStopped())
                return;
            if (width > _options.maxTextBytes - _out.size())
            {
                _error = WriteError::TextTooLarge;
                return;
            }
            _out.append(width, ' ');
        }

        void putString(std::span<std::byte const> bytes)
        {
            // In pieces, so that a long string is refused before it is held whole.
            constexpr auto Piece = std::size_t { 4096 };
            put('"');
            for (auto const piece: std::views::iota(std::size_t { 0 }, (bytes.size() + Piece - 1) / Piece))
            {
                if (isStopped())
                    break;
                auto const offset = piece * Piece;
                auto text = std::string {};
                appendString(text, bytes.subspan(offset, std::min(Piece, bytes.size() - offset)));
                put(std::string_view { text }.substr(1, text.size() - 2));
            }
            put('"');
        }

        void putName(std::string_view name) { putString(bytesOf(name)); }

        void putNumber(uint64_t value) { put(std::format(" {}", value)); }

        // -----------------------------------------------------------------------------------------
        // Names

        [[nodiscard]] std::string_view directName(NameKind kind, uint32_t index) const
        {
            if (_names.rendering == NameRendering::Raw)
                return {};
            auto const& entries = _names.section.direct[std::to_underlying(kind)];
            auto const found = entryIndexOf(entries, index);
            return found ? std::string_view { _names.direct[std::to_underlying(kind)][*found] }
                         : std::string_view {};
        }

        [[nodiscard]] InnerNames innerNames(NameKind kind, uint32_t outer) const
        {
            if (_names.rendering == NameRendering::Raw)
                return {};
            auto const& entries = _names.section.indirect[std::to_underlying(kind)];
            auto const found = entryIndexOf(entries, outer);
            if (!found)
                return {};
            return InnerNames { .names = entries[*found].names,
                                .decorations = _names.indirect[std::to_underlying(kind)][*found] };
        }

        // -----------------------------------------------------------------------------------------
        // Types

        void putHeapType(binary::HeapType const& heap)
        {
            if (heap.kind == binary::AbstractHeapType::Concrete)
            {
                put(std::format("{}", heap.typeIndex));
                return;
            }
            auto const found = std::ranges::find(HeapTypeKeywords, heap.kind, &HeapTypeKeyword::heap);
            Require(found != HeapTypeKeywords.end()); // The decoder reads no other abstract heap type.
            put(found->keyword);
        }

        void putValueType(ValueType const& type)
        {
            if (auto const found = std::ranges::find(ValueTypeKeywords, type, &ValueTypeKeyword::type);
                found != ValueTypeKeywords.end())
            {
                put(found->keyword);
                return;
            }
            Require(type.kind == binary::ValueKind::Ref); // Every number and vector type has a keyword.
            put(type.nullable == binary::Nullable::Yes ? "(ref null " : "(ref ");
            putHeapType(type.heap);
            put(')');
        }

        /// ` (keyword t…)`, or nothing for no types.
        void putTypeList(std::string_view keyword, std::span<ValueType const> types)
        {
            if (types.empty())
                return;
            put(std::format(" ({}", keyword));
            for (auto const& type: types)
            {
                put(' ');
                putValueType(type);
            }
            put(')');
        }

        /// A signature written inline: its parameters, each named one in a list of its own, so that it can
        /// carry its name, and the unnamed ones between them in one; then its results.
        void putSignature(FuncType const& type, InnerNames const& names)
        {
            auto isOpen = false;
            for (auto const index: std::views::iota(std::size_t { 0 }, type.params.size()))
            {
                if (isStopped())
                    return;
                auto const name = names.of(static_cast<uint32_t>(index));
                if (!name.empty() || !isOpen)
                {
                    if (isOpen)
                        put(')');
                    put(" (param");
                    put(name);
                    isOpen = name.empty();
                }
                put(' ');
                putValueType(type.params[index]);
                if (!name.empty())
                    put(')');
            }
            if (isOpen)
                put(')');
            putTypeList("result", type.results);
        }

        /// ` (type N)`, and the signature inline after it when a parameter is named, since only an inline
        /// parameter can carry a name; the reader then checks it against type N, which it is.
        void putTypeUse(uint32_t typeIndex, InnerNames const& names)
        {
            put(std::format(" (type {})", typeIndex));
            if (typeIndex < _module.types.size()
                && names.namesAnyBelow(_module.types[typeIndex].params.size()))
                putSignature(_module.types[typeIndex], names);
        }

        void putLimits(Limits const& limits)
        {
            if (limits.addressType == binary::AddressType::I64)
                put(" i64");
            putNumber(limits.min);
            if (limits.max)
                putNumber(*limits.max);
        }

        void putTableType(binary::TableType const& type)
        {
            putLimits(type.limits);
            put(' ');
            putValueType(type.element);
        }

        void putMemoryType(binary::MemoryType const& type)
        {
            putLimits(type.limits);
            if (type.pageSizeLog2 == binary::DefaultPageSizeLog2)
                return;
            // The text format's page size is a u32 (`pagesize` reads an i32 natural), so it has no
            // spelling for one of 2^32 bytes or more, which decodes (but never validates).
            if (type.pageSizeLog2 >= MaxWritablePageSizeLog2)
            {
                _error = WriteError::PageSizeNotWritable;
                return;
            }
            put(std::format(" (pagesize {})", binary::pageSize(type)));
        }

        void putGlobalType(binary::GlobalType const& type)
        {
            put(' ');
            if (type.mutability == binary::Mutability::Var)
                put("(mut ");
            putValueType(type.type);
            if (type.mutability == binary::Mutability::Var)
                put(')');
        }

        // -----------------------------------------------------------------------------------------
        // Instructions

        /// The immediates of an index-only kind, in the text format's order (IndexImmediates).
        void putIndices(IndexImmediate const& row, Instruction const& instruction)
        {
            auto const field = [&](IndexOperand const& operand) {
                return operand.field == InstructionField::A ? instruction.a : instruction.b;
            };
            auto const& [first, second] = row.operands;
            putNumber(field(first));
            if (row.arity == IndexArity::NoneOrTwo || row.arity == IndexArity::OneOrTwo)
                putNumber(field(second));
        }

        void putBlockType(Instruction const& instruction)
        {
            switch (instruction.blockKind)
            {
                case BlockType::Kind::Empty: return;
                case BlockType::Kind::Value:
                    put(" (result ");
                    putValueType(instruction.type);
                    put(')');
                    return;
                case BlockType::Kind::TypeIndex: put(std::format(" (type {})", instruction.a)); return;
            }
        }

        void putCatchClauses(Function const* function, Instruction const& instruction)
        {
            // A constant expression keeps no catch clauses (binary::ConstExpr).
            if (function == nullptr)
                return;
            for (auto const& clause: binary::catchClausesOf(*function, instruction))
            {
                auto const row = std::ranges::find(CatchKeywords, clause.kind, &CatchKeyword::kind);
                put(std::format(" ({}", row->keyword));
                if (binary::catchesByTag(clause.kind))
                    putNumber(clause.tag);
                putNumber(clause.label);
                put(')');
            }
        }

        void putMemoryArgument(Instruction const& instruction)
        {
            auto const* access = binary::memoryAccessOf(instruction.opcode);
            Require(access
                    != nullptr); // Every MemArg and MemArgLane instruction is a row of MemoryAccessTable.
            if (instruction.b != 0)
                putNumber(instruction.b);
            if (instruction.c != 0)
                put(std::format(" {}{}", OffsetPrefix, instruction.c));
            // The decoder reads an alignment exponent below 64 only, so its power is a u64.
            if (instruction.a != access->naturalAlignment())
                put(std::format(" {}{}", AlignmentPrefix, uint64_t { 1 } << instruction.a));
        }

        void putVector(std::span<std::byte const, 16> bytes)
        {
            put(std::format(" {}", WrittenVectorShape.keyword));
            for (auto const lane: std::views::iota(std::size_t { 0 }, WrittenVectorShape.laneCount()))
            {
                auto value = uint32_t { 0 };
                for (auto const byte:
                     std::views::iota(std::size_t { 0 }, std::size_t { WrittenVectorShape.laneBytes }))
                    value |= uint32_t {
                        std::to_integer<uint8_t>(bytes[(lane * WrittenVectorShape.laneBytes) + byte])
                    } << (8U * byte);
                put(std::format(" {:#x}", value));
            }
        }

        /// Writes an instruction and its immediates; @p function is the body it belongs to, or null for
        /// a constant expression, which has no side tables.
        void putInstruction(Instruction const& instruction, Function const* function, std::string_view label)
        {
            put(textName(instruction.opcode));
            auto const kind = binary::opcodeInfo(instruction.opcode).immediate;
            switch (kind)
            {
                case ImmediateKind::None: return;
                case ImmediateKind::LabelIndex:
                case ImmediateKind::FunctionIndex:
                case ImmediateKind::TypeIndex:
                case ImmediateKind::TagIndex:
                case ImmediateKind::LocalIndex:
                case ImmediateKind::GlobalIndex:
                case ImmediateKind::TableIndex:
                case ImmediateKind::TableCopy:
                case ImmediateKind::TableInit:
                case ImmediateKind::ElemIndex:
                case ImmediateKind::MemoryIndex:
                case ImmediateKind::MemoryCopy:
                case ImmediateKind::MemoryInit:
                case ImmediateKind::DataIndex:
                    // Every index-only kind has an IndexImmediates row.
                    putIndices(*indexImmediateOf(kind), instruction);
                    return;
                case ImmediateKind::BlockType:
                    put(label);
                    putBlockType(instruction);
                    return;
                case ImmediateKind::TryTable:
                    put(label);
                    putBlockType(instruction);
                    putCatchClauses(function, instruction);
                    return;
                case ImmediateKind::LabelTable:
                    // A constant expression keeps no labels: one is enough to read as a br_table.
                    if (function == nullptr)
                        putNumber(0);
                    else
                        for (auto const target: binary::labelsOf(*function, instruction))
                            putNumber(target);
                    return;
                case ImmediateKind::CallIndirect:
                    putNumber(instruction.b);
                    put(std::format(" (type {})", instruction.a));
                    return;
                case ImmediateKind::SelectTypes:
                    put(" (result");
                    if (function != nullptr)
                        for (auto const& type: binary::selectTypesOf(*function, instruction))
                        {
                            put(' ');
                            putValueType(type);
                        }
                    put(')');
                    return;
                case ImmediateKind::MemArg: putMemoryArgument(instruction); return;
                case ImmediateKind::MemArgLane:
                    putMemoryArgument(instruction);
                    putNumber(instruction.lane);
                    return;
                case ImmediateKind::LaneIndex: putNumber(instruction.lane); return;
                case ImmediateKind::I32Const:
                    put(std::format(" {}", static_cast<int32_t>(static_cast<uint32_t>(instruction.c))));
                    return;
                case ImmediateKind::I64Const:
                    put(std::format(" {}", static_cast<int64_t>(instruction.c)));
                    return;
                case ImmediateKind::F32Const: put(' ' + floatText(instruction.c, F32Format)); return;
                case ImmediateKind::F64Const: put(' ' + floatText(instruction.c, F64Format)); return;
                case ImmediateKind::HeapType:
                    put(' ');
                    putHeapType(instruction.type.heap);
                    return;
                case ImmediateKind::V128Const: putVector(binary::v128LiteralOf(_module, instruction)); return;
                case ImmediateKind::Shuffle:
                    for (auto const lane: binary::v128LiteralOf(_module, instruction))
                        putNumber(std::to_integer<uint8_t>(lane));
                    return;
            }
        }

        /// A constant expression on the current line: its instructions, flat, without the final end.
        void putExpression(ConstExpr const& expression)
        {
            auto const instructions =
                std::span { expression }.first(expression.empty() ? 0 : expression.size() - 1);
            for (auto const& instruction: instructions)
            {
                put(' ');
                putInstruction(instruction, nullptr, {});
            }
        }

        /// A function's body, one instruction per line from @p level, block bodies one level deeper; its
        /// final end is the function's `)`.
        void writeBody(uint32_t functionIndex, Function const& function, uint32_t level)
        {
            auto const labels = innerNames(NameKind::Label, functionIndex);
            auto hint = std::ranges::lower_bound(
                _module.branchHints, functionIndex, {}, &binary::BranchHint::function);
            auto labelIndex = uint32_t { 0 };
            auto depth = level;
            auto const body =
                std::span { function.body }.first(function.body.empty() ? 0 : function.body.size() - 1);
            for (auto const index: std::views::iota(std::size_t { 0 }, body.size()))
            {
                if (isStopped())
                    return;
                auto const& instruction = body[index];
                if (instruction.opcode == Opcode::End || instruction.opcode == Opcode::Else)
                    --depth;
                startLine(depth);
                if (_hints == HintRendering::Annotations && hint != _module.branchHints.end()
                    && hint->function == functionIndex && hint->instruction == index)
                {
                    put(std::format(R"((@metadata.code.branch_hint "\{:02x}") )",
                                    std::to_underlying(hint->likelihood)));
                    ++hint;
                }
                auto const kind = binary::opcodeInfo(instruction.opcode).immediate;
                auto const opensBlock = kind == ImmediateKind::BlockType || kind == ImmediateKind::TryTable;
                putInstruction(
                    instruction, &function, opensBlock ? labels.of(labelIndex) : std::string_view {});
                put('\n');
                if (opensBlock)
                    ++labelIndex;
                if (opensBlock || instruction.opcode == Opcode::Else)
                    ++depth;
            }
        }

        // -----------------------------------------------------------------------------------------
        // Fields

        void writeTypes(uint32_t level)
        {
            for (auto const index:
                 std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(_module.types.size())))
            {
                if (isStopped())
                    return;
                startLine(level);
                put("(type");
                put(directName(NameKind::Type, index));
                put(" (func");
                putSignature(_module.types[index], innerNames(NameKind::Parameter, index));
                put("))\n");
            }
        }

        void writeImports(uint32_t level)
        {
            auto counters = std::array<uint32_t, ExternRows.size()> {};
            for (auto const& import: _module.imports)
            {
                if (isStopped())
                    return;
                auto const row = std::ranges::find(ExternRows, import.kind, &ExternRow::kind);
                auto const index = counters[static_cast<std::size_t>(row - ExternRows.begin())]++;
                startLine(level);
                put("(import ");
                putName(binary::importModuleOf(_module, import));
                put(' ');
                putName(import.name);
                put(std::format(" ({}", row->keyword));
                switch (import.kind)
                {
                    case ExternKind::Func:
                        put(directName(NameKind::Function, index));
                        putTypeUse(import.typeIndex, innerNames(NameKind::Local, index));
                        break;
                    case ExternKind::Table:
                        put(directName(NameKind::Table, index));
                        putTableType(import.table);
                        break;
                    case ExternKind::Memory:
                        put(directName(NameKind::Memory, index));
                        putMemoryType(import.memory);
                        break;
                    case ExternKind::Global:
                        put(directName(NameKind::Global, index));
                        putGlobalType(import.global);
                        break;
                    case ExternKind::Tag:
                        put(directName(NameKind::Tag, index));
                        put(std::format(" (type {})", import.typeIndex));
                        break;
                }
                put("))\n");
            }
        }

        /// A function's declared locals, run by run: each named local in a list of its own, the unnamed
        /// ones of a run between them in one.
        void writeLocals(Function const& function,
                         InnerNames const& names,
                         std::size_t paramCount,
                         uint32_t level)
        {
            auto local = paramCount;
            for (auto const& run: function.locals)
            {
                auto isOpen = false;
                for ([[maybe_unused]] auto const ordinal: std::views::iota(uint32_t { 0 }, run.count))
                {
                    if (isStopped())
                        return;
                    auto const name = names.of(static_cast<uint32_t>(local++));
                    if (!name.empty() || !isOpen)
                    {
                        if (isOpen)
                            put(")\n");
                        startLine(level);
                        put("(local");
                        put(name);
                        isOpen = name.empty();
                    }
                    put(' ');
                    putValueType(run.type);
                    if (!name.empty())
                        put(")\n");
                }
                if (isOpen)
                    put(")\n");
            }
        }

        void writeFunctions(uint32_t level)
        {
            for (auto const index: std::views::iota(std::size_t { 0 }, _module.code.size()))
            {
                if (isStopped())
                    return;
                auto const functionIndex = _importedFunctions + static_cast<uint32_t>(index);
                auto const& function = _module.code[index];
                auto const locals = innerNames(NameKind::Local, functionIndex);
                startLine(level);
                put("(func");
                put(directName(NameKind::Function, functionIndex));
                putTypeUse(_module.functions.at(index), locals);
                put('\n');
                auto const typeIndex = _module.functions.at(index);
                auto const paramCount =
                    typeIndex < _module.types.size() ? _module.types[typeIndex].params.size() : 0;
                writeLocals(function, locals, paramCount, level + 1);
                writeBody(functionIndex, function, level + 1);
                startLine(level);
                put(")\n");
            }
        }

        void writeTables(uint32_t level)
        {
            auto const imported = static_cast<uint32_t>(_module.importedCount(ExternKind::Table));
            for (auto const index: std::views::iota(std::size_t { 0 }, _module.tables.size()))
            {
                auto const& table = _module.tables[index];
                startLine(level);
                put("(table");
                put(directName(NameKind::Table, imported + static_cast<uint32_t>(index)));
                putTableType(table.type);
                putExpression(table.init);
                put(")\n");
            }
        }

        void writeMemories(uint32_t level)
        {
            auto const imported = static_cast<uint32_t>(_module.importedCount(ExternKind::Memory));
            for (auto const index: std::views::iota(std::size_t { 0 }, _module.memories.size()))
            {
                startLine(level);
                put("(memory");
                put(directName(NameKind::Memory, imported + static_cast<uint32_t>(index)));
                putMemoryType(_module.memories[index]);
                put(")\n");
            }
        }

        void writeTags(uint32_t level)
        {
            auto const imported = static_cast<uint32_t>(_module.importedCount(ExternKind::Tag));
            for (auto const index: std::views::iota(std::size_t { 0 }, _module.tags.size()))
            {
                if (isStopped())
                    return;
                auto const tagIndex = imported + static_cast<uint32_t>(index);
                startLine(level);
                put("(tag");
                put(directName(NameKind::Tag, tagIndex));
                putTypeUse(_module.tags[index].typeIndex, innerNames(NameKind::TagParameter, tagIndex));
                put(")\n");
            }
        }

        void writeGlobals(uint32_t level)
        {
            auto const imported = static_cast<uint32_t>(_module.importedCount(ExternKind::Global));
            for (auto const index: std::views::iota(std::size_t { 0 }, _module.globals.size()))
            {
                if (isStopped())
                    return;
                auto const& global = _module.globals[index];
                startLine(level);
                put("(global");
                put(directName(NameKind::Global, imported + static_cast<uint32_t>(index)));
                putGlobalType(global.type);
                putExpression(global.init);
                put(")\n");
            }
        }

        void writeExports(uint32_t level)
        {
            for (auto const& entry: _module.exports)
            {
                if (isStopped())
                    return;
                auto const row = std::ranges::find(ExternRows, entry.kind, &ExternRow::kind);
                startLine(level);
                put("(export ");
                putName(entry.name);
                put(std::format(" ({} {}))\n", row->keyword, entry.index));
            }
            if (_module.start)
            {
                startLine(level);
                put(std::format("(start {})\n", *_module.start));
            }
        }

        /// A segment's offset, `(offset …)`.
        void putOffset(ConstExpr const& offset)
        {
            put(" (offset");
            putExpression(offset);
            put(')');
        }

        void writeElements(uint32_t level)
        {
            for (auto const index: std::views::iota(std::size_t { 0 }, _module.elements.size()))
            {
                if (isStopped())
                    return;
                auto const& segment = _module.elements[index];
                startLine(level);
                put("(elem");
                put(directName(NameKind::Element, static_cast<uint32_t>(index)));
                switch (segment.mode)
                {
                    case SegmentMode::Passive: break;
                    case SegmentMode::Declarative: put(" declare"); break;
                    case SegmentMode::Active:
                        put(std::format(" (table {})", segment.table));
                        putOffset(segment.offset);
                        break;
                }
                if (auto const* indices = std::get_if<std::vector<uint32_t>>(&segment.items))
                {
                    put(" func");
                    for (auto const function: *indices)
                    {
                        if (isStopped())
                            return;
                        putNumber(function);
                    }
                }
                else
                {
                    put(' ');
                    putValueType(segment.type);
                    for (auto const& item: std::get<std::vector<ConstExpr>>(segment.items))
                    {
                        if (isStopped())
                            return;
                        put(" (item");
                        putExpression(item);
                        put(')');
                    }
                }
                put(")\n");
            }
        }

        void writeData(uint32_t level)
        {
            for (auto const index: std::views::iota(std::size_t { 0 }, _module.data.size()))
            {
                if (isStopped())
                    return;
                auto const& segment = _module.data[index];
                startLine(level);
                put("(data");
                put(directName(NameKind::Data, static_cast<uint32_t>(index)));
                if (segment.mode == SegmentMode::Active)
                {
                    put(std::format(" (memory {})", segment.memory));
                    putOffset(segment.offset);
                }
                put(' ');
                putString(segment.bytes);
                put(")\n");
            }
        }

        void writeCustoms(uint32_t level)
        {
            for (auto const& custom: _module.customs)
            {
                if (isStopped())
                    return;
                auto const isName = custom.name == binary::NameSectionName;
                if ((isName && _names.rendering == NameRendering::Identifiers)
                    || isSynthesizedHintSection(custom, _hints))
                    continue;
                startLine(level);
                put("(@custom ");
                putName(custom.name);
                if (custom.after == SectionAnchor::BeforeFirst)
                    put(" (before first)");
                else
                {
                    auto const word = std::ranges::find(SectionWords, custom.after, &SectionWord::section);
                    put(std::format(" (after {})", word->word));
                }
                put(' ');
                putString(custom.bytes);
                put(")\n");
            }
        }

        void writeModule()
        {
            startLine(_baseLevel);
            put("(module");
            put(_names.module);
            put('\n');
            auto const level = _baseLevel + 1;
            writeTypes(level);
            writeImports(level);
            writeFunctions(level);
            writeTables(level);
            writeMemories(level);
            writeTags(level);
            writeGlobals(level);
            writeExports(level);
            writeElements(level);
            writeData(level);
            writeCustoms(level);
            startLine(_baseLevel);
            put(")\n");
        }

        Module const& _module;            ///< The module written.
        WriterOptions const& _options;    ///< The layout and the output limit.
        uint32_t _baseLevel;              ///< The level of the `(module` line.
        HintRendering _hints;             ///< How the branch hint section is written.
        Names _names;                     ///< How the name section is written, and the names.
        uint32_t _importedFunctions;      ///< The index of the first defined function.
        std::string _out;                 ///< The text written so far.
        std::optional<WriteError> _error; ///< Why writing failed, once it has.
    };
} // namespace

std::expected<std::string, WriteError> ModuleWriter::write(binary::Module const& module) const
{
    return write(module, 0);
}

std::expected<std::string, WriteError> ModuleWriter::write(binary::Module const& module,
                                                           uint32_t indentLevel) const
{
    return Printer { module, _options, indentLevel }.print();
}

} // namespace isoline::text
