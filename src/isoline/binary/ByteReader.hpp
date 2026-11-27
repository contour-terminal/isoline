// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

#include <isoline/binary/DecodeError.hpp>

namespace isoline::binary
{

/// Whether @p bytes are well-formed UTF-8 as RFC 3629 defines it: what ByteReader::readName accepts.
/// @param bytes The bytes to check.
/// @return true when they are.
[[nodiscard]] bool isValidUtf8(std::span<std::byte const> bytes) noexcept;

/// A forward-only cursor over a byte buffer that reads the WebAssembly binary format's primitives.
///
/// Every read either consumes what it returns or reports a DecodeError whose offset is
/// absolute: the reader's base offset plus the bytes consumed, so a reader over one section reports
/// positions in the whole module. Errors are reported where the spec reference interpreter reports
/// them: running out of bytes at the end of the buffer, an oversized LEB128 at its offending byte,
/// and an overlong one at the first byte past its limit. After a failed read the cursor's position
/// is unspecified; a decoder stops at its first error.
class ByteReader
{
  public:
    /// A reader over @p bytes, whose first byte sits at @p baseOffset in the module.
    /// @param bytes The buffer to read; it must outlive the reader and everything read from it.
    /// @param baseOffset The absolute offset of @p bytes' first byte.
    explicit ByteReader(std::span<std::byte const> bytes, std::size_t baseOffset = 0) noexcept;

    /// The absolute offset of the next byte to read.
    /// @return The base offset plus the number of bytes consumed.
    [[nodiscard]] std::size_t offset() const noexcept;

    /// The number of bytes not yet consumed.
    /// @return The bytes left to read.
    [[nodiscard]] std::size_t remaining() const noexcept;

    /// Whether every byte has been consumed.
    /// @return true when remaining() is zero.
    [[nodiscard]] bool atEnd() const noexcept;

    /// Reads one byte.
    /// @return The byte, or UnexpectedEnd.
    [[nodiscard]] std::expected<std::byte, DecodeError> readByte();

    /// Reads @p count bytes as a view into the buffer; consumes nothing when fewer remain.
    /// @param count The number of bytes to read.
    /// @return The bytes, or UnexpectedEnd.
    [[nodiscard]] std::expected<std::span<std::byte const>, DecodeError> readBytes(std::size_t count);

    /// Reads an unsigned LEB128 u32 of at most 5 bytes.
    /// @return The value, or UnexpectedEnd, IntegerTooLarge or IntegerRepresentationTooLong.
    [[nodiscard]] std::expected<uint32_t, DecodeError> readU32();

    /// Reads an unsigned LEB128 u64 of at most 10 bytes.
    /// @return The value, or UnexpectedEnd, IntegerTooLarge or IntegerRepresentationTooLong.
    [[nodiscard]] std::expected<uint64_t, DecodeError> readU64();

    /// Reads a signed LEB128 s32 of at most 5 bytes.
    /// @return The value, or UnexpectedEnd, IntegerTooLarge or IntegerRepresentationTooLong.
    [[nodiscard]] std::expected<int32_t, DecodeError> readS32();

    /// Reads a signed LEB128 s33 of at most 5 bytes, as block types encode a type index.
    /// @return The value, or UnexpectedEnd, IntegerTooLarge or IntegerRepresentationTooLong.
    [[nodiscard]] std::expected<int64_t, DecodeError> readS33();

    /// Reads a signed LEB128 s64 of at most 10 bytes.
    /// @return The value, or UnexpectedEnd, IntegerTooLarge or IntegerRepresentationTooLong.
    [[nodiscard]] std::expected<int64_t, DecodeError> readS64();

    /// Reads a little-endian 32-bit word, as f32 immediates are stored.
    /// @return The word, or UnexpectedEnd.
    [[nodiscard]] std::expected<uint32_t, DecodeError> readFixedU32();

    /// Reads a little-endian 64-bit word, as f64 immediates are stored.
    /// @return The word, or UnexpectedEnd.
    [[nodiscard]] std::expected<uint64_t, DecodeError> readFixedU64();

    /// Reads a u32 length or vector count, bounded by the bytes left, as the spec interpreter's len32.
    ///
    /// The bound counts the length's own LEB128 bytes, as the interpreter does: a length can exceed
    /// the bytes after it by its own size and still pass, and reading that many then runs out.
    /// @return The length, or a LEB128 error, or LengthOutOfBounds at the length's offset when it
    ///         exceeds the bytes remaining before it was read.
    [[nodiscard]] std::expected<uint32_t, DecodeError> readLength();

    /// Reads a name: a u32 byte length followed by that many bytes of UTF-8 (RFC 3629).
    /// @return A view of the name's bytes, or UnexpectedEnd, a LEB128 error, LengthOutOfBounds as
    ///         readLength() reports it, or MalformedUtf8Encoding reported at the offset of the name's
    ///         length prefix, as the spec interpreter does.
    [[nodiscard]] std::expected<std::string_view, DecodeError> readName();

  private:
    template <unsigned Bits>
    [[nodiscard]] std::expected<uint64_t, DecodeError> readUnsigned();

    template <unsigned Bits>
    [[nodiscard]] std::expected<int64_t, DecodeError> readSigned();

    template <typename Word>
    [[nodiscard]] std::expected<Word, DecodeError> readFixed();

    [[nodiscard]] DecodeError unexpectedEnd() const noexcept;

    std::span<std::byte const> _bytes;
    std::size_t _baseOffset;
    std::size_t _position = 0;
};

} // namespace isoline::binary
