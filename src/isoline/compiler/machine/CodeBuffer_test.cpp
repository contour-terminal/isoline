// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <vector>

#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/machine/CodeBuffer.hpp>

using isoline::compiler::CompileErrorCode;
using isoline::compiler::machine::Anchor;
using isoline::compiler::machine::CodeBuffer;
using isoline::compiler::machine::displacementBits;
using isoline::compiler::machine::fieldBytes;
using isoline::compiler::machine::FixupKind;
using isoline::compiler::machine::FixupKindRow;
using isoline::compiler::machine::fixupKindRow;
using isoline::compiler::machine::FixupRequest;
using isoline::compiler::machine::isWellFormed;
using isoline::compiler::machine::maximumDisplacement;
using isoline::compiler::machine::minimumDisplacement;
using isoline::compiler::machine::Signedness;

namespace
{

/// What every test allocates from.
struct Arena
{
    std::pmr::monotonic_buffer_resource memory { std::pmr::new_delete_resource() };
};

/// x86-64's jmp rel32 to @p target: E9 and a rel32 counted from the instruction's end.
void jumpTo(CodeBuffer& buffer, isoline::compiler::machine::CodeLabel target)
{
    buffer.emitLittleEndian(0xE9, 1);
    buffer.emitFixup(FixupRequest { .kind = FixupKind::Rel32, .target = target, .anchor = {}, .addend = -4 });
    buffer.emitLittleEndian(0, 4);
}

/// A forward jump, a backward jump, a ret and a two-entry jump table, resolved.
std::vector<uint8_t> emitSample()
{
    auto arena = Arena {};
    auto buffer = CodeBuffer { arena.memory };
    auto const back = buffer.createLabel();
    auto const forward = buffer.createLabel();
    auto const table = buffer.createLabel();
    buffer.bind(back);
    buffer.emitLittleEndian(0x90, 1); // 0: nop
    jumpTo(buffer, forward);          // 1: forward, before the label is bound
    jumpTo(buffer, back);             // 6: backward
    buffer.bind(forward);
    buffer.emitLittleEndian(0xC3, 1); // 11: ret
    buffer.bind(table);               // 12: the table, entries relative to itself
    for (auto const target: { back, forward })
    {
        buffer.emitFixup(
            FixupRequest { .kind = FixupKind::TableEntry32, .target = target, .anchor = table, .addend = 0 });
        buffer.emitLittleEndian(0, 4);
    }
    REQUIRE(buffer.resolve().has_value());
    return { buffer.bytes().begin(), buffer.bytes().end() };
}

/// Resolves one Rel32 whose target is its own site, displaced by @p addend.
std::expected<void, isoline::compiler::CompileError> resolveDisplacement(int64_t addend,
                                                                         std::vector<uint8_t>& bytes)
{
    auto arena = Arena {};
    auto buffer = CodeBuffer { arena.memory };
    auto const site = buffer.createLabel();
    buffer.bind(site);
    buffer.emitFixup(
        FixupRequest { .kind = FixupKind::Rel32, .target = site, .anchor = {}, .addend = addend });
    buffer.emitLittleEndian(0, 4);
    auto const resolved = buffer.resolve();
    bytes.assign(buffer.bytes().begin(), buffer.bytes().end());
    return resolved;
}

} // namespace

TEST_CASE("CodeBuffer.fixupsResolveDeterministically")
{
    auto const expected = std::vector<uint8_t> {
        0x90,                         // nop
        0xE9, 0x05, 0x00, 0x00, 0x00, // jmp +5 (to 11, from 6)
        0xE9, 0xF5, 0xFF, 0xFF, 0xFF, // jmp -11 (to 0, from 11)
        0xC3,                         // ret
        0xF4, 0xFF, 0xFF, 0xFF,       // 0 - 12
        0xFF, 0xFF, 0xFF, 0xFF,       // 11 - 12
    };
    auto const first = emitSample();
    CHECK(first == expected);
    // The same emission, again: the same bytes.
    CHECK(emitSample() == first);
}

TEST_CASE("CodeBuffer.labelsAreBoundOnce")
{
    auto arena = Arena {};
    auto buffer = CodeBuffer { arena.memory };
    auto const label = buffer.createLabel();
    CHECK(buffer.labelOffset(label) == std::nullopt);
    buffer.emitLittleEndian(0x0102030405060708, 8);
    buffer.bind(label);
    CHECK(buffer.labelOffset(label) == 8);
    CHECK(buffer.offset() == 8);
    buffer.emit(std::to_array<uint8_t>({ 0xAA, 0xBB }));
    CHECK(buffer.offset() == 10);
    CHECK(buffer.bytes()[9] == 0xBB);
    CHECK(buffer.bytes()[0] == 0x08);
    CHECK(buffer.bytes()[7] == 0x01);
    // A buffer without fixups resolves to itself.
    CHECK(buffer.resolve().has_value());
}

TEST_CASE("CodeBuffer.outOfRangeFixupIsRefused")
{
    // (amended for P6, 2026-11-26: F3) Rel32 takes exactly ±2 GiB: both ends resolve, one past either is
    // refused with BranchOutOfRange, and nothing is truncated into the field.
    constexpr auto Largest = int64_t { std::numeric_limits<int32_t>::max() };
    constexpr auto Smallest = int64_t { std::numeric_limits<int32_t>::min() };
    auto bytes = std::vector<uint8_t> {};

    REQUIRE(resolveDisplacement(Largest, bytes).has_value());
    CHECK(bytes == std::vector<uint8_t> { 0xFF, 0xFF, 0xFF, 0x7F });
    REQUIRE(resolveDisplacement(Smallest, bytes).has_value());
    CHECK(bytes == std::vector<uint8_t> { 0x00, 0x00, 0x00, 0x80 });

    auto const above = resolveDisplacement(Largest + 1, bytes);
    REQUIRE_FALSE(above.has_value());
    CHECK(above.error().code == CompileErrorCode::BranchOutOfRange);
    CHECK(above.error().requested == uint64_t { 1 } << 31U);
    CHECK(above.error().limit == (uint64_t { 1 } << 31U) - 1);

    auto const below = resolveDisplacement(Smallest - 1, bytes);
    REQUIRE_FALSE(below.has_value());
    CHECK(below.error().code == CompileErrorCode::BranchOutOfRange);
    CHECK(below.error().requested == (uint64_t { 1 } << 31U) + 1);
    CHECK(below.error().limit == uint64_t { 1 } << 31U);

    auto const farAway = resolveDisplacement(int64_t { 1 } << 40U, bytes);
    REQUIRE_FALSE(farAway.has_value());
    CHECK(farAway.error().code == CompileErrorCode::BranchOutOfRange);
}

TEST_CASE("CodeBuffer.fixupRangesFollowFromTheRows")
{
    // The table's rows.
    CHECK(minimumDisplacement(fixupKindRow(FixupKind::Rel32)) == std::numeric_limits<int32_t>::min());
    CHECK(maximumDisplacement(fixupKindRow(FixupKind::Rel32)) == std::numeric_limits<int32_t>::max());
    CHECK(fieldBytes(fixupKindRow(FixupKind::Rel32)) == 4);
    CHECK(fixupKindRow(FixupKind::TableEntry32).anchor == Anchor::Label);
    CHECK(fieldBytes(fixupKindRow(FixupKind::TableEntry32)) == 4);

    // A row of arm64's shape (P6's Branch19: bits 5..23 hold the displacement's bits 2..20): its range is
    // asymmetric and aligned, as derived, with no code of its own.
    auto const branch19 = FixupKindRow { .kind = FixupKind::Rel32,
                                         .name = "branch19",
                                         .parts = { { { .bitOffset = 5, .width = 19, .sourceShift = 2 } } },
                                         .partCount = 1,
                                         .signedness = Signedness::Signed,
                                         .anchor = Anchor::FixupSite };
    CHECK(isWellFormed(branch19));
    CHECK(displacementBits(branch19) == 21);
    CHECK(minimumDisplacement(branch19) == -(int64_t { 1 } << 20U));
    CHECK(maximumDisplacement(branch19) == (int64_t { 1 } << 20U) - 4);
    CHECK(fieldBytes(branch19) == 3);

    // arm64's adr: two parts, immlo (bits 29..30) and immhi (bits 5..23), the displacement's bits 0..20.
    auto const adr = FixupKindRow {
        .kind = FixupKind::Rel32,
        .name = "adr",
        .parts = { { { .bitOffset = 29, .width = 2, .sourceShift = 0 },
                     { .bitOffset = 5, .width = 19, .sourceShift = 2 } } },
        .partCount = 2,
        .signedness = Signedness::Signed,
        .anchor = Anchor::FixupSite,
    };
    CHECK(isWellFormed(adr));
    CHECK(minimumDisplacement(adr) == -(int64_t { 1 } << 20U));
    CHECK(maximumDisplacement(adr) == (int64_t { 1 } << 20U) - 1);

    // An unsigned row counts from 0.
    auto const unsigned8 = FixupKindRow { .kind = FixupKind::Rel32,
                                          .name = "unsigned8",
                                          .parts = { { { .bitOffset = 0, .width = 8, .sourceShift = 0 } } },
                                          .partCount = 1,
                                          .signedness = Signedness::Unsigned,
                                          .anchor = Anchor::FixupSite };
    CHECK(minimumDisplacement(unsigned8) == 0);
    CHECK(maximumDisplacement(unsigned8) == 255);

    // Malformed rows: overlapping parts, a gap in the displacement's bits, no parts.
    auto const overlapping = FixupKindRow {
        .kind = FixupKind::Rel32,
        .name = "overlapping",
        .parts = { { { .bitOffset = 0, .width = 8, .sourceShift = 0 },
                     { .bitOffset = 4, .width = 8, .sourceShift = 8 } } },
        .partCount = 2,
        .signedness = Signedness::Signed,
        .anchor = Anchor::FixupSite,
    };
    CHECK_FALSE(isWellFormed(overlapping));
    auto const gap = FixupKindRow {
        .kind = FixupKind::Rel32,
        .name = "gap",
        .parts = { { { .bitOffset = 0, .width = 8, .sourceShift = 0 },
                     { .bitOffset = 8, .width = 8, .sourceShift = 9 } } },
        .partCount = 2,
        .signedness = Signedness::Signed,
        .anchor = Anchor::FixupSite,
    };
    CHECK_FALSE(isWellFormed(gap));
    auto const empty = FixupKindRow {
        .kind = FixupKind::Rel32,
        .name = "empty",
        .parts = {},
        .partCount = 0,
        .signedness = Signedness::Signed,
        .anchor = Anchor::FixupSite,
    };
    CHECK_FALSE(isWellFormed(empty));
    CHECK(minimumDisplacement(empty) == 0);
    CHECK(maximumDisplacement(empty) == 0);
    auto zeroWidth = empty;
    zeroWidth.partCount = 1; // one part of no bits
    CHECK_FALSE(isWellFormed(zeroWidth));
}

namespace
{

/// Resolves one fixup of @p kind whose target is its own site, displaced by @p displacement, in the
/// instruction word @p word; @p resolved is the word afterwards.
std::expected<void, isoline::compiler::CompileError> resolveInWord(FixupKind kind,
                                                                   int64_t displacement,
                                                                   uint32_t word,
                                                                   uint32_t& resolved)
{
    auto arena = Arena {};
    auto buffer = CodeBuffer { arena.memory };
    auto const site = buffer.createLabel();
    buffer.bind(site);
    buffer.emitFixup(FixupRequest { .kind = kind, .target = site, .anchor = {}, .addend = displacement });
    buffer.emitLittleEndian(word, 4);
    auto const result = buffer.resolve();
    resolved = 0;
    for (auto const index: std::views::iota(uint32_t { 0 }, uint32_t { 4 }))
        resolved |= uint32_t { buffer.bytes()[index] } << (8U * index);
    return result;
}

/// One arm64 branch row's published range: what its instructions reach.
struct Arm64BranchRange
{
    FixupKind kind;
    int64_t minimum;
    int64_t maximum;
};

/// b and bl (±128 MiB), b.cond, cbz and cbnz (−1 MiB .. 1 MiB − 4), tbz and tbnz (±32 KiB).
constexpr auto Arm64BranchRanges = std::to_array<Arm64BranchRange>({
    { .kind = FixupKind::Branch26,
      .minimum = -(int64_t { 1 } << 27U),
      .maximum = (int64_t { 1 } << 27U) - 4 },
    { .kind = FixupKind::Branch19,
      .minimum = -(int64_t { 1 } << 20U),
      .maximum = (int64_t { 1 } << 20U) - 4 },
    { .kind = FixupKind::Branch14,
      .minimum = -(int64_t { 1 } << 15U),
      .maximum = (int64_t { 1 } << 15U) - 4 },
});

} // namespace

TEST_CASE("CodeBuffer.arm64FixupRowsPatchTheirFields")
{
    // Each word as llvm-mc -triple=aarch64 reads it after the patch; the instruction's other bits (the
    // condition, the register, the tested bit) are kept.
    struct Sample
    {
        FixupKind kind;
        int64_t displacement;
        uint32_t word;     ///< The instruction with its displacement field zero.
        uint32_t expected; ///< The instruction with it filled in.
    };
    constexpr auto Samples = std::to_array<Sample>({
        { .kind = FixupKind::Branch19,
          .displacement = 4096,
          .word = 0x5400'0001,
          .expected = 0x5400'8001 }, // b.ne
        { .kind = FixupKind::Branch19,
          .displacement = 8,
          .word = 0xB400'0007,
          .expected = 0xB400'0047 }, // cbz x7
        { .kind = FixupKind::Branch26,
          .displacement = 4096,
          .word = 0x1400'0000,
          .expected = 0x1400'0400 }, // b
        { .kind = FixupKind::Branch26,
          .displacement = -8,
          .word = 0x1400'0000,
          .expected = 0x17FF'FFFE }, // b
        { .kind = FixupKind::Branch14,
          .displacement = 4096,
          .word = 0x3618'0000,
          .expected = 0x3618'8000 }, // tbz w0, #3
        // Backward, over words whose other bits are set: the sign fills the field only, never the opcode.
        { .kind = FixupKind::Branch19,
          .displacement = -8,
          .word = 0x5400'0001,
          .expected = 0x54FF'FFC1 }, // b.ne
        { .kind = FixupKind::Branch19,
          .displacement = -4096,
          .word = 0x3500'0003,
          .expected = 0x35FF'8003 }, // cbnz w3
        { .kind = FixupKind::Branch14,
          .displacement = -8,
          .word = 0xB708'0005,
          .expected = 0xB70F'FFC5 }, // tbnz x5, #33
    });
    for (auto const& sample: Samples)
    {
        INFO(fixupKindRow(sample.kind).name << " by " << sample.displacement);
        auto resolved = uint32_t { 0 };
        REQUIRE(resolveInWord(sample.kind, sample.displacement, sample.word, resolved).has_value());
        CHECK(resolved == sample.expected);
    }

    // Through a label bound 4 KiB on, as an encoder records it: b.ne at 0, its target at 4096.
    auto arena = Arena {};
    auto buffer = CodeBuffer { arena.memory };
    auto const target = buffer.createLabel();
    buffer.emitFixup(
        FixupRequest { .kind = FixupKind::Branch19, .target = target, .anchor = {}, .addend = 0 });
    buffer.emitLittleEndian(0x5400'0001, 4);
    buffer.emit(std::vector<uint8_t>(4092, 0));
    buffer.bind(target);
    REQUIRE(buffer.resolve().has_value());
    CHECK(std::vector<uint8_t> { buffer.bytes().begin(), buffer.bytes().begin() + 4 }
          == std::vector<uint8_t> { 0x01, 0x80, 0x00, 0x54 });
}

TEST_CASE("CodeBuffer.arm64FixupRangesAreAsymmetric")
{
    for (auto const& range: Arm64BranchRanges)
    {
        auto const& row = fixupKindRow(range.kind);
        INFO(row.name);
        CHECK(isWellFormed(row));
        CHECK(row.anchor == Anchor::FixupSite);
        CHECK(fieldBytes(row) <= 4);
        CHECK(minimumDisplacement(row) == range.minimum);
        CHECK(maximumDisplacement(row) == range.maximum);

        // Both ends resolve; one instruction past either is refused, with what it asked and what the row
        // takes.
        auto const pastMaximum = static_cast<uint64_t>(range.maximum + 4);
        auto const maximum = static_cast<uint64_t>(range.maximum);
        auto const pastMinimum = static_cast<uint64_t>(-(range.minimum - 4));
        auto const minimum = static_cast<uint64_t>(-range.minimum);
        auto resolved = uint32_t { 0 };
        CHECK(resolveInWord(range.kind, range.minimum, 0, resolved).has_value());
        CHECK(resolveInWord(range.kind, range.maximum, 0, resolved).has_value());
        auto const above = resolveInWord(range.kind, range.maximum + 4, 0, resolved);
        REQUIRE_FALSE(above.has_value());
        CHECK(above.error().code == CompileErrorCode::BranchOutOfRange);
        CHECK(above.error().requested == pastMaximum);
        CHECK(above.error().limit == maximum);
        auto const below = resolveInWord(range.kind, range.minimum - 4, 0, resolved);
        REQUIRE_FALSE(below.has_value());
        CHECK(below.error().code == CompileErrorCode::BranchOutOfRange);
        CHECK(below.error().requested == pastMinimum);
        CHECK(below.error().limit == minimum);
    }
}
