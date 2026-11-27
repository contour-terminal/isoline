// SPDX-License-Identifier: Apache-2.0
#include <isoline/binary/NameSection.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/ByteReader.hpp>
#include <isoline/binary/ByteWriter.hpp>
#include <isoline/binary/DecodeError.hpp>

namespace isoline::binary
{

namespace
{
    /// The NameSubsections row of subsection id @p id, or nullptr when Isoline reads no such id.
    [[nodiscard]] NameSubsection const* subsectionOf(uint8_t id) noexcept
    {
        auto const row = std::ranges::find(NameSubsections, id, &NameSubsection::id);
        return row == NameSubsections.end() ? nullptr : &*row;
    }

    /// Reads a vector whose entries carry strictly increasing indices.
    /// @param reader The reader positioned at the vector's count.
    /// @param readEntry Reads one entry, as an expected of a type with an `index`.
    /// @return The entries, or the first error; an index not above its predecessor's is
    ///         NameIndexOutOfOrder at its entry's offset.
    template <typename Entry, typename ReadEntry>
    [[nodiscard]] std::expected<std::vector<Entry>, DecodeError> readIndexedVector(ByteReader& reader,
                                                                                   ReadEntry readEntry)
    {
        auto const count = reader.readLength();
        if (!count)
            return std::unexpected { count.error() };
        auto entries = std::vector<Entry> {};
        for ([[maybe_unused]] auto const ordinal: std::views::iota(uint32_t { 0 }, *count))
        {
            auto const offset = reader.offset();
            auto entry = readEntry(reader);
            if (!entry)
                return std::unexpected { entry.error() };
            if (!entries.empty() && entry->index <= entries.back().index)
                return std::unexpected { DecodeError { DecodeErrorCode::NameIndexOutOfOrder, offset } };
            entries.push_back(std::move(*entry));
        }
        return entries;
    }

    [[nodiscard]] std::expected<NameAssoc, DecodeError> readNameAssoc(ByteReader& reader)
    {
        auto const index = reader.readU32();
        if (!index)
            return std::unexpected { index.error() };
        return reader.readName().transform([&](std::string_view name) {
            return NameAssoc { .index = *index, .name = std::string { name } };
        });
    }

    [[nodiscard]] std::expected<std::vector<NameAssoc>, DecodeError> readNameMap(ByteReader& reader)
    {
        return readIndexedVector<NameAssoc>(reader, readNameAssoc);
    }

    [[nodiscard]] std::expected<IndirectNameMap, DecodeError> readIndirectNameAssoc(ByteReader& reader)
    {
        auto const index = reader.readU32();
        if (!index)
            return std::unexpected { index.error() };
        return readNameMap(reader).transform([&](std::vector<NameAssoc>&& names) {
            return IndirectNameMap { .index = *index, .names = std::move(names) };
        });
    }

    /// @p entries' addresses, by ascending index.
    /// Precondition (checked): no two entries share an index.
    template <typename Entry>
    [[nodiscard]] std::vector<Entry const*> byIndex(std::span<Entry const> entries)
    {
        auto ordered = std::vector<Entry const*> {};
        ordered.reserve(entries.size());
        for (auto const& entry: entries)
            ordered.push_back(&entry);
        std::ranges::sort(ordered, {}, &Entry::index);
        Require(std::ranges::adjacent_find(ordered, {}, &Entry::index) == ordered.end());
        return ordered;
    }

    /// Writes @p name. Precondition (checked): it is valid UTF-8, as decodeNameSection requires.
    void writeValidName(Bytes& out, std::string_view name)
    {
        Require(isValidUtf8(std::as_bytes(std::span { name })));
        writeName(out, name);
    }

    void writeNameMap(Bytes& out, std::span<NameAssoc const> map)
    {
        writeLength(out, map.size());
        for (auto const* entry: byIndex(map))
        {
            writeUnsigned(out, entry->index);
            writeValidName(out, entry->name);
        }
    }

    void writeIndirectNameMap(Bytes& out, std::span<IndirectNameMap const> map)
    {
        writeLength(out, map.size());
        for (auto const* entry: byIndex(map))
        {
            writeUnsigned(out, entry->index);
            writeNameMap(out, entry->names);
        }
    }

    /// A subsection as it is framed, before its contents are read.
    struct RawSubsection
    {
        uint8_t id;                          ///< Its id.
        std::size_t offset;                  ///< The offset of its contents within the payload.
        std::span<std::byte const> contents; ///< Its contents.
    };

    /// Reads a subsection's id and size, and takes its contents as they are.
    [[nodiscard]] std::expected<RawSubsection, DecodeError> readRawSubsection(ByteReader& reader)
    {
        return reader.readByte().and_then([&](std::byte id) {
            return reader.readLength().and_then([&](uint32_t size) {
                auto const offset = reader.offset();
                return reader.readBytes(size).transform([&](std::span<std::byte const> contents) {
                    return RawSubsection { .id = std::to_integer<uint8_t>(id),
                                           .offset = offset,
                                           .contents = contents };
                });
            });
        });
    }

    /// How one NameShape is read, written, and found to have nothing to name; the subsection's kind
    /// selects its row of NameSection::direct or NameSection::indirect.
    struct ShapeCodec
    {
        NameShape shape; ///< The shape; equal to the row's index.
        /// Reads a subsection's contents into the names.
        std::expected<void, DecodeError> (*read)(ByteReader& reader, std::size_t kind, NameSection& names);
        /// Writes a subsection's contents from the names.
        void (*write)(Bytes& out, std::size_t kind, NameSection const& names);
        /// Whether the names hold nothing for the subsection, which is then omitted.
        bool (*isEmpty)(std::size_t kind, NameSection const& names);
    };

    std::expected<void, DecodeError> readSingle(ByteReader& reader, std::size_t /*kind*/, NameSection& names)
    {
        return reader.readName().transform(
            [&](std::string_view name) { names.module = std::string { name }; });
    }

    std::expected<void, DecodeError> readDirect(ByteReader& reader, std::size_t kind, NameSection& names)
    {
        return readNameMap(reader).transform(
            [&](std::vector<NameAssoc>&& map) { names.direct[kind] = std::move(map); });
    }

    std::expected<void, DecodeError> readIndirect(ByteReader& reader, std::size_t kind, NameSection& names)
    {
        return readIndexedVector<IndirectNameMap>(reader, readIndirectNameAssoc)
            .transform([&](std::vector<IndirectNameMap>&& map) { names.indirect[kind] = std::move(map); });
    }

    void writeSingle(Bytes& out, std::size_t /*kind*/, NameSection const& names)
    {
        writeValidName(out, *names.module);
    }

    void writeDirect(Bytes& out, std::size_t kind, NameSection const& names)
    {
        writeNameMap(out, names.direct[kind]);
    }

    void writeIndirect(Bytes& out, std::size_t kind, NameSection const& names)
    {
        writeIndirectNameMap(out, names.indirect[kind]);
    }

    bool isSingleEmpty(std::size_t /*kind*/, NameSection const& names)
    {
        return !names.module.has_value();
    }

    bool isDirectEmpty(std::size_t kind, NameSection const& names)
    {
        return names.direct[kind].empty();
    }

    bool isIndirectEmpty(std::size_t kind, NameSection const& names)
    {
        return names.indirect[kind].empty();
    }

    /// One row per NameShape, in enumerator order.
    constexpr auto ShapeCodecs = std::to_array<ShapeCodec>({
        { .shape = NameShape::Single, .read = readSingle, .write = writeSingle, .isEmpty = isSingleEmpty },
        { .shape = NameShape::Direct, .read = readDirect, .write = writeDirect, .isEmpty = isDirectEmpty },
        { .shape = NameShape::Indirect,
          .read = readIndirect,
          .write = writeIndirect,
          .isEmpty = isIndirectEmpty },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: ShapeCodecs)
            {
                if (std::to_underlying(row.shape) != index)
                    return false;
                ++index;
            }
            return index == std::to_underlying(NameShape::Indirect) + 1U;
        }(),
        "ShapeCodecs must hold one row per NameShape, at the index of its enumerator");

    /// The ShapeCodecs row of @p row's shape.
    [[nodiscard]] ShapeCodec const& codecOf(NameSubsection const& row) noexcept
    {
        return ShapeCodecs[std::to_underlying(row.shape)];
    }
} // namespace

std::expected<NameSection, DecodeError> decodeNameSection(std::span<std::byte const> payload)
{
    auto reader = ByteReader { payload };
    auto names = NameSection {};
    auto lastId = std::optional<uint8_t> {};
    while (!reader.atEnd())
    {
        auto const raw = readRawSubsection(reader);
        if (!raw)
            return std::unexpected { raw.error() };

        auto const id = raw->id;
        auto const inOrder = !lastId || id > *lastId;
        lastId = std::max(lastId.value_or(id), id);
        auto const* row = subsectionOf(id);
        if (!inOrder || row == nullptr)
            continue;

        auto subsection = ByteReader { raw->contents, raw->offset };
        if (auto const read = codecOf(*row).read(subsection, std::to_underlying(row->kind), names); !read)
            return std::unexpected { read.error() };
        if (!subsection.atEnd())
            return std::unexpected { DecodeError { DecodeErrorCode::NameSubsectionSizeMismatch,
                                                   subsection.offset() } };
    }
    return names;
}

std::vector<std::byte> encodeNameSection(NameSection const& names)
{
    auto out = Bytes {};
    auto contents = Bytes {};
    for (auto const& row: NameSubsections)
    {
        auto const& codec = codecOf(row);
        auto const kind = std::size_t { std::to_underlying(row.kind) };
        Require(row.shape == NameShape::Direct || names.direct[kind].empty());
        Require(row.shape == NameShape::Indirect || names.indirect[kind].empty());
        if (codec.isEmpty(kind, names))
            continue;
        contents.clear();
        codec.write(contents, kind, names);
        writeByte(out, row.id);
        writeLength(out, contents.size());
        writeBytes(out, contents);
    }
    return out;
}

} // namespace isoline::binary
