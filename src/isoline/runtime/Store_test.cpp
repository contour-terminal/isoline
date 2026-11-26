// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <vector>

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
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Store;
using isoline::runtime::Trap;
using isoline::runtime::UnlimitedAllocator;
using isoline::runtime::Value;

namespace
{

/// The default page size, 64 KiB.
constexpr auto PageSize = isoline::binary::pageSize(MemoryType {});

} // namespace

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

TEST_CASE("Store.addressTypesBoundGrowth")
{
    // A guest can name sizes no host can hold; the store refuses them without allocating.
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const at64 = Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I64 };
    auto const memory = store.addMemory(MemoryType { .limits = at64 });
    auto const table = store.addTable(TableType { .element = FuncRef, .limits = at64 },
                                      Ref { .kind = RefKind::Null, .heap = FuncRef.heap, .address = 0 });
    REQUIRE(memory.has_value());
    REQUIRE(table.has_value());
    // Past 2^48 pages, the memory64 reach; past what the allocator grants; past 2^64 - 1 slots.
    CHECK(store.growMemory(*memory, uint64_t { 1 } << 48) == -1);
    CHECK(store.growMemory(*memory, uint64_t { 1 } << 20) == -1);
    CHECK(store.growTable(*table, ~uint64_t { 0 }, Ref {}) == -1);
    CHECK(store.growTable(*table, uint64_t { 1 } << 62, Ref {}) == -1);
    CHECK(store.memory(*memory).bytes.size() == PageSize);
    CHECK(store.table(*table).elements.size() == 1);
    // What the allocator grants, it grows by.
    CHECK(store.growMemory(*memory, 1) == 1);
    CHECK(store.growTable(*table, 1, Ref {}) == 1);
}

TEST_CASE("Store.byteGranularMemory")
{
    // A memory of 1-byte pages holds as many bytes as pages, and grows by bytes.
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const memory =
        store.addMemory(MemoryType { .limits = Limits { .min = 3, .max = std::nullopt }, .pageSizeLog2 = 0 });
    REQUIRE(memory.has_value());
    CHECK(store.memory(*memory).bytes.size() == 3);
    CHECK(store.growMemory(*memory, 1) == 3);
    CHECK(store.memory(*memory).bytes.size() == 4);
    // An i32 memory of 1-byte pages reaches 2^32 - 1 bytes, not 2^32.
    CHECK(store.growMemory(*memory, 0xFFFF'FFFC) == -1);
    // With i64 addresses it reaches 2^64 - 1 pages, which no product overflows to grant: the
    // allocator still refuses what is past 4 GiB.
    auto const wide = store.addMemory(
        MemoryType { .limits = Limits { .min = 0, .max = std::nullopt, .addressType = AddressType::I64 },
                     .pageSizeLog2 = 0 });
    REQUIRE(wide.has_value());
    CHECK(store.growMemory(*wide, ~uint64_t { 0 }) == -1);
    CHECK(store.growMemory(*wide, (uint64_t { 1 } << 32) + 1) == -1);
    CHECK(store.growMemory(*wide, 16) == 0);
    CHECK(store.memory(*wide).bytes.size() == 16);
}

TEST_CASE("Store.internTypeHashConses")
{
    using isoline::binary::concreteRef;
    using isoline::binary::Nullable;
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const takesI32 = store.internType(FuncType { .params = { isoline::binary::I32 }, .results = {} });
    auto const returnsI32 = store.internType(FuncType { .params = {}, .results = { isoline::binary::I32 } });
    CHECK(store.internType(FuncType { .params = { isoline::binary::I32 }, .results = {} }) == takesI32);
    CHECK(returnsI32 != takesI32);
    // A type naming another names it by its TypeId, so two such types are equal when the ids are.
    auto const takesRef =
        store.internType(FuncType { .params = { concreteRef(takesI32, Nullable::No) }, .results = {} });
    CHECK(store.internType(FuncType { .params = { concreteRef(takesI32, Nullable::No) }, .results = {} })
          == takesRef);
    CHECK(store.internType(FuncType { .params = { concreteRef(returnsI32, Nullable::No) }, .results = {} })
          != takesRef);
    CHECK(store.internType(FuncType { .params = { concreteRef(takesI32, Nullable::Yes) }, .results = {} })
          != takesRef);
    // Functions intern their types.
    auto const host = store.addHostFunction(
        FuncType { .params = { isoline::binary::I32 }, .results = {} },
        [](std::span<Value const>, std::span<Value>) -> std::expected<void, Trap> { return {}; });
    CHECK(store.function(host).typeId == takesI32);
    auto const defined = store.addModuleFunction(returnsI32, 0, 0);
    CHECK(store.function(defined).typeId == returnsI32);
    CHECK(store.functionType(host) == FuncType { .params = { isoline::binary::I32 }, .results = {} });
}

TEST_CASE("Store.findTypeDoesNotIntern")
{
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const takesI32 = FuncType { .params = { isoline::binary::I32 }, .results = {} };
    CHECK(store.typeCount() == 0);
    CHECK(store.findType(takesI32) == std::nullopt);
    CHECK(store.typeCount() == 0);
    // A new type gets the next TypeId, typeCount() before it, and is found from then on.
    auto const id = store.internType(takesI32);
    CHECK(id == 0);
    CHECK(store.typeCount() == 1);
    CHECK(store.findType(takesI32) == id);
    CHECK(store.internType(FuncType {}) == 1);
    CHECK(store.internType(takesI32) == id);
    CHECK(store.typeCount() == 2);
}

TEST_CASE("Store.typeRoundTrip")
{
    using isoline::binary::concreteRef;
    using isoline::binary::Nullable;
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const empty = FuncType {};
    auto const wide =
        FuncType { .params = { isoline::binary::I64, FuncRef }, .results = { isoline::binary::F32 } };
    auto const emptyId = store.internType(empty);
    auto const wideId = store.internType(wide);
    auto const typed = FuncType { .params = { concreteRef(wideId, Nullable::Yes) }, .results = {} };
    auto const typedId = store.internType(typed);
    CHECK(store.type(emptyId) == empty);
    CHECK(store.type(wideId) == wide);
    CHECK(store.type(typedId) == typed);
    // A reference stays valid as the store interns more.
    auto const& kept = store.type(wideId);
    for (auto const count: std::views::iota(uint32_t { 0 }, uint32_t { 100 }))
        std::ignore =
            store.internType(FuncType { .params = std::vector(count, isoline::binary::I32), .results = {} });
    CHECK(kept == wide);
}

TEST_CASE("Store.tagsAreDistinct")
{
    // Every tag added is a tag of its own, whatever its type; equal types share their TypeId.
    auto const allocator = UnlimitedAllocator {};
    auto store = Store { allocator };
    auto const type = FuncType { .params = { isoline::binary::I32 }, .results = {} };
    auto const first = store.addTag(type);
    auto const second = store.addTag(type);
    CHECK(first != second);
    CHECK(store.tag(first).typeId == store.tag(second).typeId);
    CHECK(store.tag(first).type == type);
    CHECK(store.findType(type) == store.tag(first).typeId);
}

TEST_CASE("Store.exceptionLimits")
{
    using isoline::runtime::StoreLimits;
    using isoline::runtime::TrapCode;
    auto const allocator = UnlimitedAllocator {};
    auto const payload = [](std::size_t count) {
        return std::vector<Value>(count, I32 { .bits = 7 });
    };

    SECTION("the number of exceptions")
    {
        auto store = Store { allocator, StoreLimits { .maxExceptions = 2 } };
        auto const tag = store.addTag(FuncType {});
        auto const first = store.addException(tag, {});
        REQUIRE(first.has_value());
        REQUIRE(store.addException(tag, {}).has_value());
        auto const third = store.addException(tag, {});
        REQUIRE_FALSE(third.has_value());
        CHECK(third.error() == Trap { TrapCode::ExceptionLimitExceeded });
        CHECK(isoline::runtime::specMessage(third.error()) == "exception limit exceeded");
        CHECK(store.exception(*first).tag == tag);
    }

    SECTION("the values of their payloads")
    {
        auto store = Store { allocator, StoreLimits { .maxExceptionValues = 3 } };
        using isoline::binary::I32;
        auto const none = store.addTag(FuncType {});
        auto const one = store.addTag(FuncType { .params = { I32 }, .results = {} });
        auto const two = store.addTag(FuncType { .params = { I32, I32 }, .results = {} });
        auto const first = store.addException(two, payload(2));
        REQUIRE(first.has_value());
        CHECK(store.exception(*first).payload == payload(2));
        CHECK(store.addException(two, payload(2)).error() == Trap { TrapCode::ExceptionLimitExceeded });
        CHECK(store.addException(one, payload(1)).has_value());
        CHECK(store.addException(none, {}).has_value());
        CHECK_FALSE(store.addException(one, payload(1)).has_value());
    }

    SECTION("the defaults")
    {
        auto const limits = StoreLimits {};
        CHECK(limits.maxCallDepth == 10'000);
        CHECK(limits.maxExceptions == 65'536);
        CHECK(limits.maxExceptionValues == uint64_t { 1 } << 20);
    }
}
