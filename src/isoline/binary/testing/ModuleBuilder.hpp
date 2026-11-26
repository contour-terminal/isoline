// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>

namespace isoline::binary::testing
{

/// The unsigned LEB128 encoding of @p value, in its shortest form.
/// @param value The value to encode.
/// @return Its bytes.
[[nodiscard]] inline std::vector<uint8_t> encodeU64(uint64_t value)
{
    auto bytes = std::vector<uint8_t> {};
    do
    {
        auto byte = static_cast<uint8_t>(value & 0x7Fu);
        value >>= 7;
        if (value != 0)
            byte |= 0x80u;
        bytes.push_back(byte);
    } while (value != 0);
    return bytes;
}

/// The unsigned LEB128 encoding of @p value, in its shortest form.
/// @param value The value to encode.
/// @return Its bytes.
[[nodiscard]] inline std::vector<uint8_t> encodeU32(uint32_t value)
{
    return encodeU64(value);
}

/// The binary encoding of @p name: its u32 byte length, then its bytes.
/// @param name The name to encode.
/// @return Its bytes.
[[nodiscard]] inline std::vector<uint8_t> encodeName(std::string_view name)
{
    auto bytes = encodeU32(static_cast<uint32_t>(name.size()));
    bytes.insert(bytes.end(), name.begin(), name.end());
    return bytes;
}

/// The one-byte encoding of a number, vector or abstract reference type.
///
/// Precondition (checked): a reference is funcref or externref, the only reference types with a
/// one-byte encoding this builder writes; any other ends the test rather than encode as another.
/// @param type The type to encode.
/// @return Its byte.
[[nodiscard]] inline uint8_t encodeValueType(ValueType const& type)
{
    switch (type.kind)
    {
        case ValueKind::I32: return 0x7F;
        case ValueKind::I64: return 0x7E;
        case ValueKind::F32: return 0x7D;
        case ValueKind::F64: return 0x7C;
        case ValueKind::V128: return 0x7B;
        case ValueKind::Ref: break;
    }
    Require(type == FuncRef || type == ExternRef);
    return type == FuncRef ? 0x70 : 0x6F;
}

/// The binary encoding of @p limits: the flags byte, the minimum and, if bounded, the maximum.
///
/// The flags say whether a maximum follows (0x01) and whether the addresses are i64 (0x04, memory64);
/// the bounds are u64s, which for values below 2^32 are the bytes of the u32s WebAssembly 2.0 reads.
/// @param limits The limits to encode.
/// @return Its bytes.
[[nodiscard]] inline std::vector<uint8_t> encodeLimits(Limits const& limits)
{
    constexpr auto HasMaximum = uint8_t { 0x01 };
    constexpr auto I64Addresses = uint8_t { 0x04 };
    auto const flags = static_cast<uint8_t>((limits.max ? HasMaximum : 0)
                                            | (limits.addressType == AddressType::I64 ? I64Addresses : 0));
    auto bytes = std::vector<uint8_t> { flags };
    auto const min = encodeU64(limits.min);
    bytes.insert(bytes.end(), min.begin(), min.end());
    if (limits.max)
    {
        auto const max = encodeU64(*limits.max);
        bytes.insert(bytes.end(), max.begin(), max.end());
    }
    return bytes;
}

/// The binary encoding of @p type: its limits, with the custom-page-sizes flag (0x08) and the page
/// size's exponent after the bounds when the page size is not the default 64 KiB.
/// @param type The memory type to encode.
/// @return Its bytes.
[[nodiscard]] inline std::vector<uint8_t> encodeMemoryType(MemoryType const& type)
{
    constexpr auto CustomPageSize = uint8_t { 0x08 };
    auto bytes = encodeLimits(type.limits);
    if (type.pageSizeLog2 == DefaultPageSizeLog2)
        return bytes;
    bytes.front() |= CustomPageSize;
    auto const exponent = encodeU32(type.pageSizeLog2);
    bytes.insert(bytes.end(), exponent.begin(), exponent.end());
    return bytes;
}

/// Assembles a binary module in tests.
///
/// The structured methods fill the type, import, function, table, memory, global, export, start,
/// element, data count, code and data sections, which build() writes in the binary format's order; raw
/// sections follow them in the order they were added, so a test that needs a section elsewhere, or out of
/// order, writes every section raw.
class ModuleBuilder
{
  public:
    /// Appends a function type to the type section.
    /// @param type The function type.
    /// @return This builder.
    ModuleBuilder& type(FuncType type)
    {
        _types.push_back(std::move(type));
        return *this;
    }

    /// Appends a function import to the import section.
    /// @param module The module name.
    /// @param name The field name.
    /// @param typeIndex The index of its function type.
    /// @return This builder.
    ModuleBuilder& importFunction(std::string_view module, std::string_view name, uint32_t typeIndex)
    {
        auto entry = importHeader(module, name, 0x00);
        append(entry, encodeU32(typeIndex));
        _imports.push_back(std::move(entry));
        return *this;
    }

    /// Appends a global import to the import section.
    /// @param module The module name.
    /// @param name The field name.
    /// @param type The global's type.
    /// @return This builder.
    ModuleBuilder& importGlobal(std::string_view module, std::string_view name, GlobalType const& type)
    {
        auto entry = importHeader(module, name, 0x03);
        appendGlobalType(entry, type);
        _imports.push_back(std::move(entry));
        return *this;
    }

    /// Appends a table import to the import section.
    /// @param module The module name.
    /// @param name The field name.
    /// @param type The table's required type.
    /// @return This builder.
    ModuleBuilder& importTable(std::string_view module, std::string_view name, TableType const& type)
    {
        auto entry = importHeader(module, name, 0x01);
        entry.push_back(encodeValueType(type.element));
        append(entry, encodeLimits(type.limits));
        _imports.push_back(std::move(entry));
        return *this;
    }

    /// Appends a memory import of 64 KiB pages to the import section.
    /// @param module The module name.
    /// @param name The field name.
    /// @param limits The memory's required size bounds, in pages.
    /// @return This builder.
    ModuleBuilder& importMemory(std::string_view module, std::string_view name, Limits const& limits)
    {
        return importMemory(module, name, MemoryType { .limits = limits });
    }

    /// Appends a memory import to the import section.
    /// @param module The module name.
    /// @param name The field name.
    /// @param type The memory's required type.
    /// @return This builder.
    ModuleBuilder& importMemory(std::string_view module, std::string_view name, MemoryType const& type)
    {
        auto entry = importHeader(module, name, 0x02);
        append(entry, encodeMemoryType(type));
        _imports.push_back(std::move(entry));
        return *this;
    }

    /// Appends a defined function: its type to the function section, its code to the code section.
    /// @param typeIndex The index of its function type.
    /// @param locals Its locals, one entry per local; runs of one type are encoded as one entry.
    /// @param bodyWithoutEnd Its instructions; build() appends the final end.
    /// @return This builder.
    ModuleBuilder& function(uint32_t typeIndex,
                            std::vector<ValueType> locals,
                            std::vector<uint8_t> bodyWithoutEnd)
    {
        _functions.push_back(FunctionEntry {
            .typeIndex = typeIndex, .locals = std::move(locals), .body = std::move(bodyWithoutEnd) });
        return *this;
    }

    /// Appends a table to the table section.
    /// @param type Its element type and size bounds.
    /// @return This builder.
    ModuleBuilder& table(TableType const& type)
    {
        _tables.push_back(type);
        return *this;
    }

    /// Appends a defined global to the global section.
    /// @param type Its type.
    /// @param initWithoutEnd Its initializer's instructions; build() appends the final end.
    /// @return This builder.
    ModuleBuilder& global(GlobalType const& type, std::vector<uint8_t> initWithoutEnd)
    {
        auto entry = std::vector<uint8_t> {};
        appendGlobalType(entry, type);
        append(entry, initWithoutEnd);
        entry.push_back(0x0B); // end
        _globals.push_back(std::move(entry));
        return *this;
    }

    /// Appends a memory of 64 KiB pages to the memory section.
    /// @param limits Its size bounds, in pages.
    /// @return This builder.
    ModuleBuilder& memory(Limits limits) { return memory(MemoryType { .limits = limits }); }

    /// Appends a memory to the memory section.
    /// @param type Its size bounds and page size.
    /// @return This builder.
    ModuleBuilder& memory(MemoryType const& type)
    {
        _memories.push_back(type);
        return *this;
    }

    /// Appends a function export to the export section.
    /// @param name The exported name.
    /// @param index The function's index.
    /// @return This builder.
    ModuleBuilder& exportFunction(std::string name, uint32_t index)
    {
        return exportEntity(std::move(name), ExternKind::Func, index);
    }

    /// Appends an export of any kind to the export section.
    /// @param name The exported name.
    /// @param kind The index space @p index refers to.
    /// @param index The entity's index.
    /// @return This builder.
    ModuleBuilder& exportEntity(std::string name, ExternKind kind, uint32_t index)
    {
        _exports.push_back(ExportEntry { .name = std::move(name), .kind = kind, .index = index });
        return *this;
    }

    /// Sets the start section.
    /// @param index The start function's index.
    /// @return This builder.
    ModuleBuilder& start(uint32_t index)
    {
        _start = index;
        return *this;
    }

    /// Sets the data count section, which memory.init and data.drop require.
    /// @param count The number of data segments.
    /// @return This builder.
    ModuleBuilder& dataCount(uint32_t count)
    {
        _dataCount = count;
        return *this;
    }

    /// Appends an element segment to the element section, encoded by the caller.
    /// @param entry The segment's bytes, from its flags on.
    /// @return This builder.
    ModuleBuilder& element(std::vector<uint8_t> entry)
    {
        _elements.push_back(std::move(entry));
        return *this;
    }

    /// Appends a data segment to the data section, encoded by the caller.
    /// @param entry The segment's bytes, from its flags on.
    /// @return This builder.
    ModuleBuilder& data(std::vector<uint8_t> entry)
    {
        _data.push_back(std::move(entry));
        return *this;
    }

    /// Appends a section verbatim after the structured ones.
    /// @param id The section id.
    /// @param payload The section's contents; build() prefixes their size.
    /// @return This builder.
    ModuleBuilder& rawSection(uint8_t id, std::vector<uint8_t> payload)
    {
        _rawSections.emplace_back(id, std::move(payload));
        return *this;
    }

    /// The module's bytes: the preamble, then every non-empty structured section, then the raw ones.
    /// @return The binary module.
    [[nodiscard]] std::vector<std::byte> build() const
    {
        auto bytes = std::vector<uint8_t> { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 };
        appendSection(bytes, TypeSectionId, typeSection());
        appendSection(bytes, ImportSectionId, entriesOf(_imports));
        appendSection(bytes, FunctionSectionId, functionSection());
        appendSection(bytes, TableSectionId, tableSection());
        appendSection(bytes, MemorySectionId, memorySection());
        appendSection(bytes, GlobalSectionId, entriesOf(_globals));
        appendSection(bytes, ExportSectionId, exportSection());
        if (_start)
            appendRaw(bytes, StartSectionId, encodeU32(*_start));
        appendSection(bytes, ElementSectionId, entriesOf(_elements));
        if (_dataCount)
            appendRaw(bytes, DataCountSectionId, encodeU32(*_dataCount));
        appendSection(bytes, CodeSectionId, codeSection());
        appendSection(bytes, DataSectionId, entriesOf(_data));
        for (auto const& [id, payload]: _rawSections)
            appendRaw(bytes, id, payload);
        auto result = std::vector<std::byte>(bytes.size());
        std::ranges::transform(bytes, result.begin(), [](uint8_t byte) { return std::byte { byte }; });
        return result;
    }

  private:
    static constexpr auto TypeSectionId = uint8_t { 1 };
    static constexpr auto ImportSectionId = uint8_t { 2 };
    static constexpr auto FunctionSectionId = uint8_t { 3 };
    static constexpr auto TableSectionId = uint8_t { 4 };
    static constexpr auto MemorySectionId = uint8_t { 5 };
    static constexpr auto GlobalSectionId = uint8_t { 6 };
    static constexpr auto ExportSectionId = uint8_t { 7 };
    static constexpr auto StartSectionId = uint8_t { 8 };
    static constexpr auto ElementSectionId = uint8_t { 9 };
    static constexpr auto CodeSectionId = uint8_t { 10 };
    static constexpr auto DataSectionId = uint8_t { 11 };
    static constexpr auto DataCountSectionId = uint8_t { 12 };

    struct ExportEntry
    {
        std::string name;
        ExternKind kind = ExternKind::Func;
        uint32_t index = 0;
    };

    struct FunctionEntry
    {
        uint32_t typeIndex = 0;
        std::vector<ValueType> locals;
        std::vector<uint8_t> body;
    };

    /// A section's entries, or nothing when it has none.
    struct Entries
    {
        uint32_t count = 0;
        std::vector<uint8_t> bytes;
    };

    static void append(std::vector<uint8_t>& out, std::span<uint8_t const> bytes)
    {
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

    static void appendRaw(std::vector<uint8_t>& out, uint8_t id, std::span<uint8_t const> payload)
    {
        out.push_back(id);
        append(out, encodeU32(static_cast<uint32_t>(payload.size())));
        append(out, payload);
    }

    static void appendSection(std::vector<uint8_t>& out, uint8_t id, Entries const& entries)
    {
        if (entries.count == 0)
            return;
        auto payload = encodeU32(entries.count);
        append(payload, entries.bytes);
        appendRaw(out, id, payload);
    }

    static void appendGlobalType(std::vector<uint8_t>& out, GlobalType const& type)
    {
        out.push_back(encodeValueType(type.type));
        out.push_back(type.mutability == Mutability::Var ? 0x01 : 0x00);
    }

    [[nodiscard]] static std::vector<uint8_t> importHeader(std::string_view module,
                                                           std::string_view name,
                                                           uint8_t kind)
    {
        auto entry = encodeName(module);
        append(entry, encodeName(name));
        entry.push_back(kind);
        return entry;
    }

    /// A section of entries that are encoded already.
    [[nodiscard]] static Entries entriesOf(std::span<std::vector<uint8_t> const> encoded)
    {
        auto entries = Entries { .count = static_cast<uint32_t>(encoded.size()), .bytes = {} };
        for (auto const& entry: encoded)
            append(entries.bytes, entry);
        return entries;
    }

    static void appendValueTypes(std::vector<uint8_t>& out, std::span<ValueType const> types)
    {
        append(out, encodeU32(static_cast<uint32_t>(types.size())));
        for (auto const& type: types)
            out.push_back(encodeValueType(type));
    }

    [[nodiscard]] Entries typeSection() const
    {
        auto entries = Entries { .count = static_cast<uint32_t>(_types.size()), .bytes = {} };
        for (auto const& type: _types)
        {
            entries.bytes.push_back(0x60);
            appendValueTypes(entries.bytes, type.params);
            appendValueTypes(entries.bytes, type.results);
        }
        return entries;
    }

    [[nodiscard]] Entries functionSection() const
    {
        auto entries = Entries { .count = static_cast<uint32_t>(_functions.size()), .bytes = {} };
        for (auto const& function: _functions)
            append(entries.bytes, encodeU32(function.typeIndex));
        return entries;
    }

    [[nodiscard]] Entries tableSection() const
    {
        auto entries = Entries { .count = static_cast<uint32_t>(_tables.size()), .bytes = {} };
        for (auto const& type: _tables)
        {
            entries.bytes.push_back(encodeValueType(type.element));
            append(entries.bytes, encodeLimits(type.limits));
        }
        return entries;
    }

    [[nodiscard]] Entries memorySection() const
    {
        auto entries = Entries { .count = static_cast<uint32_t>(_memories.size()), .bytes = {} };
        for (auto const& type: _memories)
            append(entries.bytes, encodeMemoryType(type));
        return entries;
    }

    [[nodiscard]] Entries exportSection() const
    {
        auto entries = Entries { .count = static_cast<uint32_t>(_exports.size()), .bytes = {} };
        for (auto const& entry: _exports)
        {
            append(entries.bytes, encodeName(entry.name));
            entries.bytes.push_back(static_cast<uint8_t>(entry.kind)); // ExternKind is the kind byte
            append(entries.bytes, encodeU32(entry.index));
        }
        return entries;
    }

    [[nodiscard]] Entries codeSection() const
    {
        auto entries = Entries { .count = static_cast<uint32_t>(_functions.size()), .bytes = {} };
        for (auto const& function: _functions)
        {
            auto runs = std::vector<std::pair<uint32_t, ValueType>> {};
            for (auto const& local: function.locals)
            {
                if (!runs.empty() && runs.back().second == local)
                    ++runs.back().first;
                else
                    runs.emplace_back(1, local);
            }
            auto code = encodeU32(static_cast<uint32_t>(runs.size()));
            for (auto const& [count, type]: runs)
            {
                append(code, encodeU32(count));
                code.push_back(encodeValueType(type));
            }
            append(code, function.body);
            code.push_back(0x0B); // end
            append(entries.bytes, encodeU32(static_cast<uint32_t>(code.size())));
            append(entries.bytes, code);
        }
        return entries;
    }

    std::vector<FuncType> _types;
    std::vector<std::vector<uint8_t>> _imports;
    std::vector<FunctionEntry> _functions;
    std::vector<TableType> _tables;
    std::vector<MemoryType> _memories;
    std::vector<std::vector<uint8_t>> _globals;
    std::vector<ExportEntry> _exports;
    std::optional<uint32_t> _start;
    std::optional<uint32_t> _dataCount;
    std::vector<std::vector<uint8_t>> _elements;
    std::vector<std::vector<uint8_t>> _data;
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> _rawSections;
};

} // namespace isoline::binary::testing
