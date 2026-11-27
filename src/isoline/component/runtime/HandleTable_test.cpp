// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <ranges>
#include <variant>
#include <vector>

#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/runtime/HandleTable.hpp>
#include <isoline/component/runtime/testing/ResourceDoubles.hpp>

using isoline::component::canonical::ResourceTypeIndex;
using isoline::component::canonical::TrapCode;
using isoline::component::canonical::trapOf;
using isoline::component::runtime::FreeSlot;
using isoline::component::runtime::HandleBudget;
using isoline::component::runtime::HandleEntry;
using isoline::component::runtime::HandleTable;
using isoline::component::runtime::HandleTableFirstChunkLength;
using isoline::component::runtime::HandleTableMaxLength;
using isoline::component::runtime::Ownership;
using isoline::component::runtime::ResourceHandle;
using isoline::component::runtime::testing::trapIn;
using isoline::component::runtime::testing::valueIn;

namespace
{

/// An own handle of resource type 1 whose representation is @p rep.
[[nodiscard]] HandleEntry ownOf(uint32_t rep)
{
    return ResourceHandle { .type = ResourceTypeIndex { 1 }, .rep = rep, .ownership = Ownership::Own };
}

/// The representation of the handle at @p index.
[[nodiscard]] uint32_t repAt(HandleTable& table, uint32_t index)
{
    auto const entry = table.get(index);
    REQUIRE(entry.has_value());
    return std::get<ResourceHandle>(entry->get()).rep;
}

} // namespace

TEST_CASE("HandleTable.indexReuseIsLifo")
{
    auto budget = HandleBudget { 100 };
    auto table = HandleTable { budget };

    SECTION("the brief's sequence")
    {
        CHECK(valueIn(table.add(ownOf(10))) == 1);
        CHECK(valueIn(table.add(ownOf(20))) == 2);
        CHECK(valueIn(table.add(ownOf(30))) == 3);
        REQUIRE(table.remove(2).has_value());
        REQUIRE(table.remove(1).has_value());
        CHECK(valueIn(table.add(ownOf(40))) == 1);
        CHECK(valueIn(table.add(ownOf(50))) == 2);
        CHECK(valueIn(table.add(ownOf(60))) == 4);
    }

    SECTION("handle-table.wast's run")
    {
        CHECK(valueIn(table.add(ownOf(100))) == 1);
        CHECK(valueIn(table.add(ownOf(200))) == 2);
        CHECK(valueIn(table.add(ownOf(300))) == 3);
        REQUIRE(table.remove(2).has_value());
        CHECK(valueIn(table.add(ownOf(400))) == 2);
        CHECK(repAt(table, 1) == 100);
        CHECK(repAt(table, 2) == 400);
        CHECK(repAt(table, 3) == 300);
        REQUIRE(table.remove(1).has_value());
        REQUIRE(table.remove(2).has_value());
        REQUIRE(table.remove(3).has_value());
        CHECK(valueIn(table.add(ownOf(500))) == 3);
        CHECK(valueIn(table.add(ownOf(600))) == 2);
        CHECK(valueIn(table.add(ownOf(700))) == 1);
        CHECK(valueIn(table.add(ownOf(800))) == 4);
    }

    CHECK(table.length() >= 4);
}

TEST_CASE("HandleTable.budgetIsStoreWide")
{
    // The budget counts the slots of every table of the store: three slots across two tables, and a
    // fourth anywhere is refused. A freed slot is its table's to reuse; the budget gets it back when
    // the table goes.
    auto budget = HandleBudget { 3 };
    CHECK(budget.limit() == 3);
    auto first = HandleTable { budget };
    {
        auto second = HandleTable { budget };
        CHECK(valueIn(first.add(ownOf(1))) == 1);
        CHECK(valueIn(first.add(ownOf(2))) == 2);
        CHECK(valueIn(second.add(ownOf(3))) == 1);
        CHECK(budget.used() == 3);

        auto const refused = trapIn(first.add(ownOf(4)));
        CHECK(refused == trapOf(TrapCode::Handles, 3));
        CHECK(refused.message() == "handles count exceeds limit of 3");
        CHECK(trapIn(second.add(ownOf(4))).code == TrapCode::Handles);
        // A refused add changes nothing.
        CHECK(first.length() == 3);
        CHECK(second.length() == 2);

        REQUIRE(first.remove(1).has_value());
        CHECK(budget.used() == 3);
        CHECK(trapIn(second.add(ownOf(5))).code == TrapCode::Handles);
        CHECK(valueIn(first.add(ownOf(6))) == 1);
    }
    // The second table's slot is back.
    CHECK(budget.used() == 2);
    CHECK(valueIn(first.add(ownOf(7))) == 3);
    CHECK(budget.used() == 3);
}

TEST_CASE("HandleTable.unknownIndicesTrap")
{
    auto budget = HandleBudget { 100 };
    auto table = HandleTable { budget };
    // A table that never grew.
    CHECK(trapIn(table.get(0)) == trapOf(TrapCode::UnknownHandle, 0U));
    CHECK(trapIn(table.remove(1)) == trapOf(TrapCode::UnknownHandle, 1U));
    REQUIRE(table.add(ownOf(1)).has_value());
    REQUIRE(table.add(ownOf(2)).has_value());

    for (auto const index: { uint32_t { 0 }, uint32_t { 3 }, uint32_t { 64 }, uint32_t { 0xFFFF'FFFFU } })
    {
        CAPTURE(index);
        CHECK(trapIn(table.get(index)) == trapOf(TrapCode::UnknownHandle, index));
        CHECK(trapIn(table.remove(index)) == trapOf(TrapCode::UnknownHandle, index));
    }
    CHECK(trapIn(table.get(4294967295U)).message() == "unknown handle index 4294967295");

    REQUIRE(table.remove(1).has_value());
    CHECK(trapIn(table.get(1)).message() == "unknown handle index 1");
    CHECK(trapIn(table.remove(1)).message() == "unknown handle index 1");
    // A failed remove leaves the free list alone: the next add still takes slot 1, once.
    CHECK(valueIn(table.add(ownOf(3))) == 1);
    CHECK(valueIn(table.add(ownOf(4))) == 3);
}

TEST_CASE("HandleTable.maxIndexTraps")
{
    // definitions.py traps an add whose new index passes MAX_LENGTH; a small maximum reaches it.
    auto budget = HandleBudget { 100 };
    auto table = HandleTable { budget, 3 };
    for (auto const expected: std::views::iota(uint32_t { 1 }, uint32_t { 4 }))
        CHECK(valueIn(table.add(ownOf(expected))) == expected);
    CHECK(trapIn(table.add(ownOf(4))) == trapOf(TrapCode::Handles, 3));
    CHECK(budget.used() == 3);
    REQUIRE(table.remove(2).has_value());
    CHECK(valueIn(table.add(ownOf(5))) == 2);

    // The real maximum fits 28 bits.
    CHECK(HandleTableMaxLength == (uint32_t { 1 } << 28U) - 1);
}

TEST_CASE("HandleTable.entriesStayPutAcrossGrowth")
{
    // Chunked storage: growing through several chunks never moves an entry.
    auto budget = HandleBudget { 100'000 };
    auto table = HandleTable { budget };
    REQUIRE(table.add(ownOf(7)).has_value());
    auto const* const first = &valueIn(table.get(1)).get();
    for (auto const rep: std::views::iota(uint32_t { 2 }, uint32_t { 10'000 }))
        REQUIRE(valueIn(table.add(ownOf(rep))) == rep);
    CHECK(&valueIn(table.get(1)).get() == first);
    // Every chunk boundary holds what was added there.
    for (auto const index: { HandleTableFirstChunkLength - 1,
                             HandleTableFirstChunkLength,
                             (3 * HandleTableFirstChunkLength) - 1,
                             3 * HandleTableFirstChunkLength,
                             uint32_t { 9'999 } })
        CHECK(repAt(table, index) == index);
    CHECK(repAt(table, 1) == 7);
    CHECK(budget.used() == 9'999);
}

TEST_CASE("HandleTable.removeReturnsTheEntry")
{
    auto budget = HandleBudget { 10 };
    auto table = HandleTable { budget };
    REQUIRE(table.add(ownOf(42)).has_value());
    auto const removed = table.remove(1);
    REQUIRE(removed.has_value());
    CHECK(*removed == ownOf(42));

    auto slots = std::vector<HandleEntry> {};
    table.forEachSlot([&](uint32_t, HandleEntry const& entry) { slots.push_back(entry); });
    CHECK(slots == std::vector<HandleEntry> { FreeSlot {} });
}
