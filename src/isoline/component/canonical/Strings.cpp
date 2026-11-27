// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/canonical/Strings.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

#include <isoline/api/Types.hpp>
#include <isoline/component/canonical/GuestMemory.hpp>
#include <isoline/component/canonical/LiftLower.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Unicode.hpp>
#include <isoline/component/canonical/Values.hpp>

namespace isoline::component::canonical
{

namespace
{
    /// Every source form a string can have: its encoding and a simple encoding that encoding admits.
    struct SourceForm
    {
        StringEncoding from;
        SimpleEncoding fromSimple;
    };

    constexpr auto SourceForms = std::to_array<SourceForm>({
        { .from = StringEncoding::Utf8, .fromSimple = SimpleEncoding::Utf8 },
        { .from = StringEncoding::Utf16, .fromSimple = SimpleEncoding::Utf16 },
        { .from = StringEncoding::Latin1Utf16, .fromSimple = SimpleEncoding::Utf16 },
        { .from = StringEncoding::Latin1Utf16, .fromSimple = SimpleEncoding::Latin1 },
    });

    static_assert(
        [] {
            // Each source form meets each destination in exactly one row, and every row is one of them.
            for (auto const& form: SourceForms)
                for (auto const& destination: StringEncodingTable)
                {
                    auto const rows = std::ranges::count_if(TranscodeTable, [&](TranscodeRow const& row) {
                        return row.from == form.from && row.fromSimple == form.fromSimple
                               && row.to == destination.encoding;
                    });
                    if (rows != 1)
                        return false;
                }
            return TranscodeTable.size() == SourceForms.size() * StringEncodingTable.size();
        }(),
        "TranscodeTable must hold one row per source form and destination encoding");

    /// The alignment of a string's bytes in @p encoding.
    [[nodiscard]] constexpr uint32_t alignmentOf(StringEncoding encoding) noexcept
    {
        return StringEncodingTable[static_cast<std::size_t>(encoding)].alignment;
    }

    /// The trap of a string's bytes that are not text in @p encoding.
    [[nodiscard]] Trap invalidEncoding(SimpleEncoding encoding)
    {
        return trapOf(TrapCode::InvalidEncoding, simpleEncodingRow(encoding).name);
    }

    /// The trap of a string longer than MaxStringByteLength.
    [[nodiscard]] Trap tooLong(uint64_t byteLength)
    {
        return trapOf(TrapCode::OutOfBounds,
                      std::format("a string of {} bytes, more than the {} a string holds",
                                  byteLength,
                                  MaxStringByteLength));
    }

    /// Decodes @p reader's code points as far as the first @p available of its @p total bytes hold whole
    /// ones, all of them once every byte is there, calling @p onCodePoint with each: one validating pass
    /// over a string that arrives a granule at a time.
    /// @return Nothing, or InvalidEncoding at the first fault.
    template <typename F>
    [[nodiscard]] std::expected<void, Trap> decodeUpTo(CodePointReader& reader,
                                                       std::size_t available,
                                                       std::size_t total,
                                                       SimpleEncoding encoding,
                                                       F const& onCodePoint)
    {
        while (available == total || reader.position() + MaxCodePointBytes <= available)
        {
            auto const codePoint = reader.next();
            if (!codePoint)
                return std::unexpected { invalidEncoding(encoding) };
            if (!*codePoint)
                break;
            onCodePoint(**codePoint);
        }
        return {};
    }

    /// One store_string_into_range: the source's code points, the destination's memory, and the
    /// `realloc` calls between them, each written as definitions.py's function of the same name.
    class StringStore
    {
      public:
        StringStore(LiftContext& context, StringValue const& value) noexcept:
            _context { &context },
            _value { value },
            _simple { value.simpleEncoding() },
            _units { codeUnitsOf(value.encoding, value.taggedCodeUnits) }
        {
        }

        using Result = std::expected<std::pair<uint32_t, uint32_t>, Trap>;

        [[nodiscard]] Result run(TranscodeFunction function)
        {
            switch (function)
            {
                case TranscodeFunction::Utf8Copy: return stringCopy(1, 1);
                case TranscodeFunction::Utf16Copy: return stringCopy(2, 2);
                case TranscodeFunction::Latin1ToUtf16Copy: return stringCopy(2, 2);
                case TranscodeFunction::Latin1Copy: return stringCopy(1, 2);
                case TranscodeFunction::Utf16ToUtf8: return stringToUtf8(uint64_t { _units } * 3);
                case TranscodeFunction::Latin1ToUtf8: return stringToUtf8(uint64_t { _units } * 2);
                case TranscodeFunction::Utf8ToUtf16: return utf8ToUtf16();
                case TranscodeFunction::StringToLatin1OrUtf16: return stringToLatin1OrUtf16();
                case TranscodeFunction::ProbablyUtf16ToLatin1OrUtf16: return probablyUtf16ToLatin1OrUtf16();
            }
            Require(false);
            return {}; // uncovered(exhaustive-switch)
        }

      private:
        /// The next code point of the source, counted as host work; empty at its end.
        [[nodiscard]] std::expected<std::optional<char32_t>, Trap> next(CodePointReader& reader)
        {
            auto const before = reader.position();
            auto const codePoint = reader.next();
            if (!codePoint)
                return std::unexpected { invalidEncoding(_simple) };
            if (auto polled = polls(reader.position() - before); !polled)
                return std::unexpected { std::move(polled.error()) };
            return *codePoint;
        }

        [[nodiscard]] std::expected<void, Trap> polls(uint64_t bytes)
        {
            return _context->polls.copied(memory(), bytes);
        }

        [[nodiscard]] IGuestMemoryAccess& memory() const noexcept { return *_context->memory; }

        [[nodiscard]] CodePointReader reader() const noexcept
        {
            return CodePointReader { _value.bytes, _simple };
        }

        /// definitions.py's store_string_copy: the source's code units, each @p unitSize bytes wide, in
        /// a block of that many bytes aligned to @p alignment. Only Latin-1 into UTF-16 changes the bytes,
        /// each widened to a code unit.
        [[nodiscard]] Result stringCopy(uint32_t unitSize, uint32_t alignment)
        {
            auto const size = unitSize * _units;
            auto block = reallocate(memory(), 0, 0, alignment, size);
            if (!block)
                return std::unexpected { std::move(block.error()) };
            auto writer = BlockWriter { *_context, *block, size };
            auto written = std::expected<void, Trap> {};
            if (_simple != SimpleEncoding::Latin1)
                written = copyValidated(writer, [](char32_t /*codePoint*/) {});
            else if (unitSize == 1)
                written = writer.put(_value.bytes);
            else
                for (auto const byte: _value.bytes)
                {
                    written = writer.put(std::array { byte, std::byte {} });
                    if (!written)
                        break;
                }
            return written.and_then([&] { return writer.flush(); }).transform([&] {
                return std::pair { *block, _units };
            });
        }

        /// definitions.py's store_string_to_utf8: ASCII copied into a block of the source's code units;
        /// at the first code point past ASCII, the block grows to @p worstCaseSize, the rest is encoded
        /// after the ASCII, and the block shrinks to the bytes written.
        [[nodiscard]] Result stringToUtf8(uint64_t worstCaseSize)
        {
            auto block = reallocate(memory(), 0, 0, 1, _units);
            if (!block)
                return std::unexpected { std::move(block.error()) };
            auto writer = BlockWriter { *_context, *block, _units };
            auto source = reader();
            while (true)
            {
                auto const codePoint = next(source);
                if (!codePoint)
                    return std::unexpected { codePoint.error() };
                if (!*codePoint)
                    break;
                if (**codePoint < AsciiEnd)
                {
                    if (auto put = writer.put(encodeUtf8(**codePoint).view()); !put)
                        return std::unexpected { std::move(put.error()) };
                    continue;
                }
                auto const worst = static_cast<uint32_t>(worstCaseSize);
                auto grown =
                    writer.flush().and_then([&] { return reallocate(memory(), *block, _units, 1, worst); });
                if (!grown)
                    return std::unexpected { std::move(grown.error()) };
                block = *grown;
                writer.moveTo(*block, worst);
                auto encoded = writer.put(encodeUtf8(**codePoint).view());
                if (encoded)
                    encoded = encodeRest(source, writer, encodeUtf8);
                if (!encoded)
                    return std::unexpected { std::move(encoded.error()) };
                auto const length = static_cast<uint32_t>(writer.offset());
                if (worst > length)
                    block = reallocate(memory(), *block, worst, 1, length);
                return block.transform([length](uint32_t shrunk) { return std::pair { shrunk, length }; });
            }
            return writer.flush().transform([&] { return std::pair { *block, _units }; });
        }

        /// definitions.py's store_utf8_to_utf16: the source in UTF-16, in a block of twice its bytes,
        /// shrunk to the bytes written.
        [[nodiscard]] Result utf8ToUtf16()
        {
            auto const worst = 2 * _units;
            auto block = reallocate(memory(), 0, 0, 2, worst);
            if (!block)
                return std::unexpected { std::move(block.error()) };
            auto writer = BlockWriter { *_context, *block, worst };
            auto source = reader();
            if (auto encoded = encodeRest(source, writer, encodeUtf16); !encoded)
                return std::unexpected { std::move(encoded.error()) };
            auto const length = static_cast<uint32_t>(writer.offset());
            if (length < worst)
                block = reallocate(memory(), *block, worst, 2, length);
            return block.transform([length](uint32_t shrunk) { return std::pair { shrunk, length / 2 }; });
        }

        /// definitions.py's store_string_to_latin1_or_utf16: Latin-1 into a block of the source's code
        /// units; at the first code point past Latin-1, the block grows to twice that, the Latin-1 written
        /// so far is widened to UTF-16 in place, the rest follows in UTF-16, and the block shrinks to the
        /// bytes written; else it shrinks to the Latin-1 written.
        [[nodiscard]] Result stringToLatin1OrUtf16()
        {
            auto block = reallocate(memory(), 0, 0, 2, _units);
            if (!block)
                return std::unexpected { std::move(block.error()) };
            auto writer = BlockWriter { *_context, *block, _units };
            auto source = reader();
            while (true)
            {
                auto const codePoint = next(source);
                if (!codePoint)
                    return std::unexpected { codePoint.error() };
                if (!*codePoint)
                    break;
                if (**codePoint < Latin1End)
                {
                    if (auto put = writer.put(std::array { static_cast<std::byte>(**codePoint) }); !put)
                        return std::unexpected { std::move(put.error()) };
                    continue;
                }
                auto const worst = 2 * _units;
                auto const latin1 = writer.offset();
                auto grown =
                    writer.flush().and_then([&] { return reallocate(memory(), *block, _units, 2, worst); });
                if (!grown)
                    return std::unexpected { std::move(grown.error()) };
                block = *grown;
                if (auto widened = widenInPlace(*block, latin1); !widened)
                    return std::unexpected { std::move(widened.error()) };
                writer.moveTo(*block, worst);
                writer.seek(2 * latin1);
                auto encoded = writer.put(encodeUtf16(**codePoint).view());
                if (encoded)
                    encoded = encodeRest(source, writer, encodeUtf16);
                if (!encoded)
                    return std::unexpected { std::move(encoded.error()) };
                auto const length = static_cast<uint32_t>(writer.offset());
                if (worst > length)
                    block = reallocate(memory(), *block, worst, 2, length);
                return block.transform(
                    [length](uint32_t shrunk) { return std::pair { shrunk, (length / 2) | Utf16Tag }; });
            }
            if (auto flushed = writer.flush(); !flushed)
                return std::unexpected { std::move(flushed.error()) };
            auto const latin1 = static_cast<uint32_t>(writer.offset());
            if (latin1 < _units)
                block = reallocate(memory(), *block, _units, 2, latin1);
            return block.transform([latin1](uint32_t shrunk) { return std::pair { shrunk, latin1 }; });
        }

        /// definitions.py's store_probably_utf16_to_latin1_or_utf16: the UTF-16 copied; when Latin-1
        /// holds every code point, its low bytes are packed into the block's first half, which the
        /// block shrinks to.
        [[nodiscard]] Result probablyUtf16ToLatin1OrUtf16()
        {
            auto const size = 2 * _units;
            auto block = reallocate(memory(), 0, 0, 2, size);
            if (!block)
                return std::unexpected { std::move(block.error()) };
            auto writer = BlockWriter { *_context, *block, size };
            // The copy's decoding pass both validates the UTF-16 and finds whether Latin-1 holds it.
            auto latin1 = true;
            auto const copied = copyValidated(
                writer, [&latin1](char32_t codePoint) { latin1 = latin1 && codePoint < Latin1End; });
            if (auto flushed = copied.and_then([&] { return writer.flush(); }); !flushed)
                return std::unexpected { std::move(flushed.error()) };
            auto const units = std::span { _value.bytes };
            if (!latin1)
                return std::pair { *block, _units | Utf16Tag };
            writer.seek(0);
            for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { _units }))
            {
                if (auto put = writer.put(units.subspan(2 * index, 1)); !put)
                    return std::unexpected { std::move(put.error()) };
            }
            if (auto flushed = writer.flush(); !flushed)
                return std::unexpected { std::move(flushed.error()) };
            return reallocate(memory(), *block, size, 2, _units).transform([this](uint32_t shrunk) {
                return std::pair { shrunk, _units };
            });
        }

        /// Copies the source's bytes as they are, a granule at a time, each decoded first as far as it holds
        /// whole code points (decodeUpTo()): a host string that is not text traps InvalidEncoding instead of
        /// reaching a guest, which relies on the strings it is given. The writes poll.
        template <typename F>
        [[nodiscard]] std::expected<void, Trap> copyValidated(BlockWriter& writer, F const& onCodePoint)
        {
            auto source = reader();
            auto const bytes = _value.bytes;
            auto copied = std::size_t { 0 };
            while (copied < bytes.size())
            {
                auto const count = std::min<std::size_t>(bytes.size() - copied, isoline::api::BulkPollBytes);
                if (auto decoded = decodeUpTo(source, copied + count, bytes.size(), _simple, onCodePoint);
                    !decoded)
                    return decoded;
                if (auto put = writer.put(bytes.subspan(copied, count)); !put)
                    return put;
                copied += count;
            }
            return {};
        }

        /// Encodes the source's code points from @p source on with @p encode, then flushes.
        template <typename Encode>
        [[nodiscard]] std::expected<void, Trap> encodeRest(CodePointReader& source,
                                                           BlockWriter& writer,
                                                           Encode encode)
        {
            while (true)
            {
                auto const codePoint = next(source);
                if (!codePoint)
                    return std::unexpected { codePoint.error() };
                if (!*codePoint)
                    return writer.flush();
                if (auto put = writer.put(encode(**codePoint).view()); !put)
                    return put;
            }
        }

        /// Widens the @p latin1 bytes at @p block to UTF-16 code units, in place: read back from guest
        /// memory as definitions.py reads them, last chunk first, so that no byte is overwritten before
        /// it is read.
        [[nodiscard]] std::expected<void, Trap> widenInPlace(uint32_t block, uint64_t latin1)
        {
            constexpr auto Chunk = BlockWriter::BufferBytes / 2;
            auto narrow = std::array<std::byte, Chunk> {};
            auto wide = std::array<std::byte, 2 * Chunk> {};
            auto end = latin1;
            while (end > 0)
            {
                auto const begin = end - std::min<uint64_t>(end, Chunk);
                auto const count = static_cast<std::size_t>(end - begin);
                auto const at = static_cast<uint32_t>(block + begin);
                if (auto read = memory().read(at, std::span { narrow }.first(count)); !read)
                    return read;
                for (auto const index: std::views::iota(std::size_t { 0 }, count))
                {
                    wide[2 * index] = narrow[index];
                    wide[(2 * index) + 1] = std::byte {};
                }
                auto const target = static_cast<uint32_t>(block + (2 * begin));
                if (auto written = memory().write(target, std::span { wide }.first(2 * count)); !written)
                    return written;
                if (auto polled = polls(3 * count); !polled)
                    return polled;
                end = begin;
            }
            return {};
        }

        LiftContext* _context;
        StringValue _value;
        SimpleEncoding _simple;
        uint32_t _units;
    };

} // namespace

TranscodeRow const& transcodeRow(StringEncoding from, SimpleEncoding fromSimple, StringEncoding to)
{
    auto const row = std::ranges::find_if(TranscodeTable, [&](TranscodeRow const& candidate) {
        return candidate.from == from && candidate.fromSimple == fromSimple && candidate.to == to;
    });
    Require(row != TranscodeTable.end());
    return *row;
}

std::expected<void, Trap> liftString(LiftContext& context,
                                     ValueBuilder& builder,
                                     uint32_t address,
                                     uint32_t taggedCodeUnits)
{
    Require(context.memory != nullptr);
    auto& memory = *context.memory;
    auto const byteLength = stringByteLength(context.encoding, taggedCodeUnits);
    auto const simple = simpleEncodingOf(context.encoding, taggedCodeUnits);
    if (byteLength > MaxStringByteLength)
        return std::unexpected { tooLong(byteLength) };
    if (auto inRange = checkRange(address, byteLength, alignmentOf(context.encoding), memory.size());
        !inRange)
        return inRange;
    auto const bytes = builder.encodedString(context.encoding, taggedCodeUnits);
    if (auto const& refused = builder.error(); refused)
        return std::unexpected { builderTrap(context, *refused) };
    // Copied in granules straight into the values, each validated as far as it holds whole code points,
    // then the store polled: a string of any length ends within a granule of an interrupt.
    auto reader = CodePointReader { bytes, simple };
    auto copied = std::size_t { 0 };
    while (copied < bytes.size())
    {
        auto const count = std::min<std::size_t>(bytes.size() - copied, isoline::api::BulkPollBytes);
        if (auto read = memory.read(static_cast<uint32_t>(address + copied), bytes.subspan(copied, count));
            !read)
            return read;
        copied += count;
        if (auto decoded = decodeUpTo(reader, copied, bytes.size(), simple, [](char32_t /*codePoint*/) {});
            !decoded)
            return decoded;
        if (auto polled = context.polls.copied(memory, count); !polled)
            return polled;
    }
    return {};
}

std::expected<std::pair<uint32_t, uint32_t>, Trap> storeString(LiftContext& context, StringValue const& value)
{
    Require(context.memory != nullptr);
    if (value.bytes.size() > MaxStringByteLength)
        return std::unexpected { tooLong(value.bytes.size()) };
    auto const& row = transcodeRow(value.encoding, value.simpleEncoding(), context.encoding);
    return StringStore { context, value }.run(row.function);
}

} // namespace isoline::component::canonical
