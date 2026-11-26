// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>

using isoline::binary::Extension;
using isoline::binary::ImmediateKind;
using isoline::binary::MemoryAccessKind;
using isoline::binary::memoryAccessOf;
using isoline::binary::MemoryAccessTable;
using isoline::binary::Opcode;
using isoline::binary::OpcodeTable;
using isoline::binary::ValueKind;

TEST_CASE("MemoryAccess.everyMemArgOpcodeHasARow")
{
    // A memory argument is what a load or store carries, and nothing else in P1 does.
    for (auto const& row: OpcodeTable)
    {
        INFO(row.text);
        auto const* const access = memoryAccessOf(row.opcode);
        CHECK((access != nullptr) == (row.immediate == ImmediateKind::MemArg));
        if (access == nullptr)
            continue;
        CHECK(access->opcode == row.opcode);
        // The text format names a store a store, and every other access a load.
        auto const isStoreText = row.text.contains(".store");
        CHECK((access->access == MemoryAccessKind::Store) == isStoreText);
        // Only the signed narrow loads (`_s`) sign-extend.
        CHECK((access->extension == Extension::Sign) == row.text.ends_with("_s"));
    }
}

TEST_CASE("MemoryAccess.widthAndAlignmentAreOneFact")
{
    for (auto const& row: MemoryAccessTable)
        CHECK((uint32_t { 1 } << row.naturalAlignment()) == row.width);

    struct Expected
    {
        Opcode opcode;
        ValueKind value;
        uint8_t width;
        uint32_t alignment;
    };
    for (auto const& expected: { Expected { Opcode::I64Load, ValueKind::I64, 8, 3 },
                                 Expected { Opcode::F32Store, ValueKind::F32, 4, 2 },
                                 Expected { Opcode::I32Load16S, ValueKind::I32, 2, 1 },
                                 Expected { Opcode::I64Store8, ValueKind::I64, 1, 0 },
                                 Expected { Opcode::I64Load32U, ValueKind::I64, 4, 2 } })
    {
        auto const* const access = memoryAccessOf(expected.opcode);
        REQUIRE(access != nullptr);
        CHECK(access->value == expected.value);
        CHECK(access->width == expected.width);
        CHECK(access->naturalAlignment() == expected.alignment);
    }
    CHECK(memoryAccessOf(Opcode::MemoryFill) == nullptr);
    CHECK(memoryAccessOf(Opcode::I32Add) == nullptr);
}
