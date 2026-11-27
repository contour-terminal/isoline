// SPDX-License-Identifier: Apache-2.0
#pragma once

// The bytes an encoder emits, with labels and the fixups that refer to them. A fixup is a field of the
// emitted bytes that holds the displacement from an anchor to a label, filled in by resolve() once every
// label is bound. How a kind of fixup lays the displacement into its field is a row of FixupKindTable: the
// field's parts (each a run of bits of the displacement, placed at a bit offset of the field), whether the
// displacement is signed, and what it is relative to. Each row's range follows from its parts, and
// resolve() refuses a displacement outside it with BranchOutOfRange, never truncating it. P5 lands x86-64's
// rows; P6 appends arm64's branch rows (amended for P6, 2026-11-26: F3). Resolution depends on nothing but
// the emitted bytes and fixups, in their order: the same emission gives the same bytes.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include <isoline/compiler/CompileError.hpp>

namespace isoline::compiler::machine
{

/// A kind of fixup: how a displacement is laid into its field.
enum class FixupKind : uint8_t
{
    /// x86-64's rel32: a signed 32-bit displacement from the fixup site (the encoder's addend makes it
    /// relative to the instruction's end), as jmp, jcc, call and rip-relative operands take it.
    Rel32,
    /// A jump table's entry: a signed 32-bit displacement from the table's own label to a target.
    TableEntry32,
};

/// The last FixupKind enumerator.
inline constexpr auto LastFixupKind = FixupKind::TableEntry32;

/// Whether a fixup's displacement is signed.
enum class Signedness : uint8_t
{
    Unsigned, ///< From 0 up.
    Signed,   ///< Two's complement, around 0.
};

/// What a fixup's displacement is relative to.
enum class Anchor : uint8_t
{
    FixupSite, ///< The fixup's own offset: the start of its field.
    Label,     ///< A label the fixup names (a jump table's).
};

/// One run of a fixup field: @c width bits of the displacement, from its bit @c sourceShift, placed at
/// bit @c bitOffset of the field (little-endian, bit 0 the first byte's lowest).
struct FixupPart
{
    uint8_t bitOffset = 0;   ///< Where the run starts in the field.
    uint8_t width = 0;       ///< How many bits it takes.
    uint8_t sourceShift = 0; ///< The displacement's bit the run starts with.
};

/// The most parts a fixup field may have (arm64's adr splits its displacement in two).
inline constexpr std::size_t MaximumFixupParts = 2;

/// How one kind of fixup lays its displacement into its field.
struct FixupKindRow
{
    FixupKind kind = FixupKind::Rel32;                 ///< The kind; equal to the row's index.
    std::string_view name;                             ///< Its name, for diagnostics.
    std::array<FixupPart, MaximumFixupParts> parts {}; ///< The field's parts; the first partCount used.
    uint8_t partCount = 0;                             ///< How many parts the field has.
    Signedness signedness = Signedness::Signed;        ///< Whether the displacement is signed.
    Anchor anchor = Anchor::FixupSite;                 ///< What the displacement is relative to.
};

/// Every FixupKind's row, in enumerator order. P6 appends arm64's Branch26, Branch19 and Branch14.
inline constexpr auto FixupKindTable = std::to_array<FixupKindRow>({
    { .kind = FixupKind::Rel32,
      .name = "rel32",
      .parts = { { { .bitOffset = 0, .width = 32, .sourceShift = 0 } } },
      .partCount = 1,
      .signedness = Signedness::Signed,
      .anchor = Anchor::FixupSite },
    { .kind = FixupKind::TableEntry32,
      .name = "table-entry32",
      .parts = { { { .bitOffset = 0, .width = 32, .sourceShift = 0 } } },
      .partCount = 1,
      .signedness = Signedness::Signed,
      .anchor = Anchor::Label },
});

/// The parts of @p row in use.
[[nodiscard]] constexpr std::span<FixupPart const> partsOf(FixupKindRow const& row) noexcept
{
    return std::span { row.parts }.first(row.partCount);
}

/// The displacement bits @p row represents: one past its parts' highest source bit.
[[nodiscard]] constexpr uint32_t displacementBits(FixupKindRow const& row) noexcept
{
    auto bits = uint32_t { 0 };
    for (auto const& part: partsOf(row))
        bits = std::max(bits, uint32_t { part.sourceShift } + part.width);
    return bits;
}

/// The lowest displacement bit @p row represents; the bits below it must be zero (an alignment).
[[nodiscard]] constexpr uint32_t lowestDisplacementBit(FixupKindRow const& row) noexcept
{
    auto lowest = displacementBits(row);
    for (auto const& part: partsOf(row))
        lowest = std::min(lowest, uint32_t { part.sourceShift });
    return lowest;
}

/// The bytes @p row's field spans: enough for its parts' highest bit.
[[nodiscard]] constexpr uint32_t fieldBytes(FixupKindRow const& row) noexcept
{
    auto bits = uint32_t { 0 };
    for (auto const& part: partsOf(row))
        bits = std::max(bits, uint32_t { part.bitOffset } + part.width);
    return (bits + 7) / 8;
}

/// The smallest displacement @p row takes: −2^(n−1) signed, 0 unsigned, for n displacementBits() (0 for a
/// row without parts, which isWellFormed() refuses).
[[nodiscard]] constexpr int64_t minimumDisplacement(FixupKindRow const& row) noexcept
{
    auto const bits = displacementBits(row);
    if (row.signedness == Signedness::Unsigned || bits == 0 || bits > 63)
        return 0;
    return -(int64_t { 1 } << (bits - 1));
}

/// The largest displacement @p row takes: 2^(n−1) − 2^l signed, 2^n − 2^l unsigned, for n
/// displacementBits() and l lowestDisplacementBit(): the largest one whose bits below l are zero.
[[nodiscard]] constexpr int64_t maximumDisplacement(FixupKindRow const& row) noexcept
{
    auto const all = displacementBits(row);
    auto const bits = row.signedness == Signedness::Signed && all != 0 ? all - 1 : all;
    if (bits == 0 || bits > 62)
        return 0;
    return (int64_t { 1 } << bits) - (int64_t { 1 } << lowestDisplacementBit(row));
}

/// Whether @p row is well-formed: one or two parts, each non-empty; the parts cover the displacement bits
/// from lowestDisplacementBit() to displacementBits() once each, and the field's bits at most once; the
/// field fits 8 bytes and the displacement 62 bits.
[[nodiscard]] constexpr bool isWellFormed(FixupKindRow const& row) noexcept
{
    if (row.partCount == 0 || row.partCount > MaximumFixupParts)
        return false;
    auto sourceBits = uint64_t { 0 };
    auto fieldBits = uint64_t { 0 };
    for (auto const& part: partsOf(row))
    {
        if (part.width == 0 || uint32_t { part.sourceShift } + part.width > 62
            || uint32_t { part.bitOffset } + part.width > 64)
            return false;
        auto const mask = (part.width == 64 ? ~uint64_t { 0 } : (uint64_t { 1 } << part.width) - 1);
        if ((sourceBits & (mask << part.sourceShift)) != 0 || (fieldBits & (mask << part.bitOffset)) != 0)
            return false;
        sourceBits |= mask << part.sourceShift;
        fieldBits |= mask << part.bitOffset;
    }
    auto const low = lowestDisplacementBit(row);
    auto const covered = ((uint64_t { 1 } << displacementBits(row)) - 1) & ~((uint64_t { 1 } << low) - 1);
    return sourceBits == covered && fieldBytes(row) <= 8;
}

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: FixupKindTable)
        {
            if (static_cast<std::size_t>(row.kind) != index || !isWellFormed(row))
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastFixupKind) + 1;
    }(),
    "FixupKindTable must hold one well-formed row per FixupKind, at the index of its enumerator");

/// The row of @p kind.
[[nodiscard]] constexpr FixupKindRow const& fixupKindRow(FixupKind kind) noexcept
{
    return FixupKindTable[static_cast<std::size_t>(kind)];
}

/// A position in a CodeBuffer a fixup may refer to, bound once.
enum class CodeLabel : uint32_t
{
};

/// A fixup to record: its kind, the label its displacement reaches, the anchor label of a kind anchored at
/// one, and a constant added to the displacement.
struct FixupRequest
{
    FixupKind kind = FixupKind::Rel32;  ///< How the field holds the displacement.
    CodeLabel target {};                ///< The label reached.
    std::optional<CodeLabel> anchor {}; ///< The anchor, for a kind of Anchor::Label; none otherwise.
    /// Added to target − anchor: x86-64's rel32 counts from the instruction's end, which is the field's end
    /// plus any immediate after it, so its addend is −(4 + those bytes).
    int64_t addend = 0;
};

/// The bytes an encoder emits, its labels and its fixups. Movable, not copyable: its containers allocate
/// from the compile's arena.
class CodeBuffer
{
  public:
    /// An empty buffer, every container of which allocates from @p memory.
    explicit CodeBuffer(std::pmr::memory_resource& memory);

    /// A new label, not bound yet.
    [[nodiscard]] CodeLabel createLabel();

    /// Binds @p label to the current offset (Require: it is one of the buffer's, not bound yet).
    void bind(CodeLabel label);

    /// The offset @p label is bound to.
    /// @return It; none while it is not bound.
    [[nodiscard]] std::optional<uint64_t> labelOffset(CodeLabel label) const noexcept;

    /// Appends @p bytes.
    void emit(std::span<uint8_t const> bytes);

    /// Appends the @p count low bytes of @p value, least significant first (Require: @p count ≤ 8).
    void emitLittleEndian(uint64_t value, uint32_t count);

    /// Records a fixup whose field starts at the current offset. The caller emits the field's bytes next
    /// (fieldBytes() of its row, or the instruction word that holds them), with the field's bits zero;
    /// resolve() fills them in.
    /// @param request The fixup (Require: its anchor is given exactly when its row is anchored at a label).
    void emitFixup(FixupRequest const& request);

    /// Fills in every fixup, in the order they were recorded (Require: every label they name is bound,
    /// every field lies within the bytes, and every displacement has its row's alignment).
    /// @return Nothing; or BranchOutOfRange, the first fixup whose displacement lies outside its row's range,
    ///         with |displacement| requested and the largest |displacement| of that sign the row takes as
    ///         its limit. The buffer is then partly resolved and is not to be used.
    [[nodiscard]] std::expected<void, CompileError> resolve();

    /// The current offset: how many bytes were emitted.
    [[nodiscard]] uint64_t offset() const noexcept { return _bytes.size(); }

    /// The bytes emitted.
    [[nodiscard]] std::span<uint8_t const> bytes() const noexcept { return _bytes; }

  private:
    /// A recorded fixup.
    struct Fixup
    {
        FixupRequest request {};
        uint64_t site = 0;
    };

    /// What a label's offset is while it is not bound.
    static constexpr auto Unbound = ~uint64_t { 0 };

    std::pmr::vector<uint8_t> _bytes;
    std::pmr::vector<uint64_t> _labels; ///< Per label, its offset, or Unbound.
    std::pmr::vector<Fixup> _fixups;
};

} // namespace isoline::compiler::machine
