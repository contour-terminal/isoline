// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

using isoline::binary::AddressType;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::Limits;
using isoline::binary::MemoryType;
using isoline::binary::Mutability;
using isoline::binary::TableType;
using isoline::runtime::I32;
using isoline::runtime::PageSize;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Store;
using isoline::runtime::Trap;
using isoline::runtime::UnlimitedAllocator;
using isoline::runtime::Value;

TEST_CASE("Store.constAccessors")
{
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const one = Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 };
    auto const function = store.addHostFunction(
        FuncType {},
        [](std::span<Value const>, std::span<Value>) -> std::expected<void, Trap> { return {}; });
    auto const table = store.addTable(TableType { .element = FuncRef, .limits = one },
                                      Ref { .kind = RefKind::Null, .heap = FuncRef.heap, .address = 0 });
    auto const memory = store.addMemory(MemoryType { .limits = one });
    auto const global = store.addGlobal(
        GlobalType { .type = isoline::binary::I32, .mutability = Mutability::Const }, I32 { .bits = 7 });
    REQUIRE(table.has_value());
    REQUIRE(memory.has_value());

    // A const store answers through the const overloads, with the same instances.
    auto const& view = static_cast<Store const&>(store);
    CHECK(view.function(function).isHost());
    CHECK(view.table(*table).elements.size() == 1);
    CHECK(view.memory(*memory).bytes.size() == PageSize);
    CHECK(view.global(global).value == Value { I32 { .bits = 7 } });
    CHECK(&view.global(global) == &store.global(global));
}
