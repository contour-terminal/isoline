// SPDX-License-Identifier: Apache-2.0
#include <isoline/binary/Opcode.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <isoline/binary/OpcodeNames.hpp>
#include <isoline/meta/EnumRows.hpp>

namespace isoline::binary
{

namespace
{

    /// The prefix bytes of the prefixed opcodes.
    constexpr auto MiscPrefix = uint8_t { 0xFC };
    constexpr auto SimdPrefix = uint8_t { 0xFD };

    /// A lookup slot: zero when no opcode has that encoding, else the opcode's index plus one. An
    /// integer rather than an optional keeps the arrays constants on every standard library.
    using Slot = uint16_t;

    static_assert(OpcodeCount < UINT16_MAX, "every opcode index plus one must fit a Slot");

    /// The number of slots the codes under @p prefix need: the largest code plus one, or zero.
    [[nodiscard]] consteval std::size_t slotCount(uint8_t prefix) noexcept
    {
        auto count = std::size_t { 0 };
        for (auto const& row: OpcodeTable)
            if (row.prefix == prefix)
                count = std::max(count, std::size_t { row.code } + 1);
        return count;
    }

    /// The slots of every opcode under @p Prefix, indexed by code.
    template <uint8_t Prefix>
    [[nodiscard]] consteval auto buildSlots() noexcept
    {
        auto slots = std::array<Slot, slotCount(Prefix)> {};
        for (auto const& row: OpcodeTable)
            if (row.prefix == Prefix)
                slots[row.code] = static_cast<Slot>(static_cast<std::size_t>(row.opcode) + 1);
        return slots;
    }

    constexpr auto PlainSlots = buildSlots<NoPrefix>();
    constexpr auto MiscSlots = buildSlots<MiscPrefix>();
    constexpr auto SimdSlots = buildSlots<SimdPrefix>();

    /// The lookup array of one prefix.
    struct PrefixRow
    {
        uint8_t prefix;
        std::span<Slot const> slots;
    };

    /// Every prefix the opcode table uses; a prefix not listed names no opcode.
    constexpr auto PrefixTable = std::to_array<PrefixRow>({
        { .prefix = NoPrefix, .slots = PlainSlots },
        { .prefix = MiscPrefix, .slots = MiscSlots },
        { .prefix = SimdPrefix, .slots = SimdSlots },
    });

    static_assert(
        [] {
            for (auto const& row: OpcodeTable)
                if (std::ranges::none_of(
                        PrefixTable, [&](PrefixRow const& prefix) { return prefix.prefix == row.prefix; }))
                    return false;
            return true;
        }(),
        "every opcode's prefix must have a row in PrefixTable");

    static_assert(meta::firstMisplacedRow<LastOpcode>(OpcodeTable, &OpcodeInfo::opcode)
                      == meta::NoMisplacedRow,
                  "OpcodeTable must hold each Opcode at the index of its enumerator");

    static_assert(OpcodeNames.size() == OpcodeCount, "OpcodeNames must hold one name per Opcode");
    static_assert(OpcodeTexts.size() == OpcodeCount, "OpcodeTexts must hold one text per Opcode");

    // C++23 cannot compute a static_assert's message, so each check names its first offender by index
    // alone, in the comparison the compiler prints: an opcode's is its row of Opcodes.def, from zero.
    static_assert(firstMisnamedOpcode(OpcodeNames, OpcodeTexts, OpcodeNameExceptions) == OpcodeCount,
                  "every Opcode's name must be its text in PascalCase (namedByRule), or have exactly one row "
                  "of OpcodeNameExceptions and none otherwise; the index is the first that does not");
    static_assert(firstUnusedException(OpcodeNames, OpcodeTexts, OpcodeNameExceptions)
                      == OpcodeNameExceptions.size(),
                  "every row of OpcodeNameExceptions must name exactly one Opcode, by name and text, that "
                  "the naming rule does not hold for; the index is the first that does not");

} // namespace

OpcodeInfo const& opcodeInfo(Opcode opcode) noexcept
{
    return OpcodeTable[static_cast<std::size_t>(opcode)];
}

std::optional<Opcode> lookupOpcode(uint8_t prefix, uint32_t code) noexcept
{
    auto const row = std::ranges::find_if(
        PrefixTable, [prefix](PrefixRow const& candidate) { return candidate.prefix == prefix; });
    if (row == PrefixTable.end() || code >= row->slots.size())
        return std::nullopt;
    auto const slot = row->slots[code];
    if (slot == 0)
        return std::nullopt;
    return static_cast<Opcode>(slot - 1);
}

} // namespace isoline::binary
