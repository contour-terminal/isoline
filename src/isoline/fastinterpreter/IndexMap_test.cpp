// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory_resource>
#include <ranges>

#include <isoline/fastinterpreter/IndexMap.hpp>

using isoline::fastinterpreter::IndexMap;

TEST_CASE("IndexMap.agreesWithAnOrderedMap")
{
    // Inserts, updates, look-ups and erasures of keys drawn from a small range, so that probe runs collide
    // and every erasure shifts some of them back; over the smallest range the table stays small, and runs wrap
    // around its end. The map must agree with std::map throughout.
    auto const range = GENERATE(uint32_t { 301 }, uint32_t { 13 }, uint32_t { 5 });
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto map = IndexMap { &memory };
    auto oracle = std::map<uint32_t, uint32_t> {};
    // A fixed sequence (splitmix64), not a random one: the test is the same in every run.
    auto state = uint64_t { 2026 };
    auto const nextKey = [&state, range] {
        state += 0x9E37'79B9'7F4A'7C15;
        auto mixed = state;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EB;
        return static_cast<uint32_t>((mixed ^ (mixed >> 31U)) % range);
    };
    for (auto const step: std::views::iota(uint32_t { 0 }, uint32_t { 20'000 }))
    {
        auto const key = nextKey();
        switch (step % 3)
        {
            case 0: {
                auto& value = map.findOrInsert(key, step);
                auto const inserted = oracle.try_emplace(key, step).first;
                CHECK(value == inserted->second);
                value = step + 1;
                inserted->second = step + 1;
                break;
            }
            case 1: {
                auto const* const found = map.find(key);
                auto const expected = oracle.find(key);
                REQUIRE((found != nullptr) == (expected != oracle.end()));
                if (found != nullptr)
                    CHECK(*found == expected->second);
                break;
            }
            default:
                if (oracle.erase(key) == 1)
                    map.erase(key);
                break;
        }
        REQUIRE(map.size() == oracle.size());
    }
    for (auto const& [key, value]: oracle)
    {
        auto const* const found = map.find(key);
        REQUIRE(found != nullptr);
        CHECK(*found == value);
    }
}

namespace
{

/// Counts the allocations it passes on to the global heap.
class CountingResource final: public std::pmr::memory_resource
{
  public:
    [[nodiscard]] uint64_t allocations() const noexcept { return _allocations; }

  private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        ++_allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override
    {
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override
    {
        return this == &other;
    }

    uint64_t _allocations = 0;
};

} // namespace

TEST_CASE("IndexMap.growsAndAllocatesNothingEmpty")
{
    // A function forwards few of its locals, often none: an empty map costs nothing, and a full one grows by
    // doubling, a logarithmic number of tables for its keys.
    auto memory = CountingResource {};
    auto map = IndexMap { &memory };
    CHECK(map.find(7) == nullptr);
    CHECK(memory.allocations() == 0);
    for (auto const key: std::views::iota(uint32_t { 0 }, uint32_t { 1'000 }))
        map.findOrInsert(key * 7'919, key) = key + 1;
    CHECK(map.size() == 1'000);
    // 8, 16, ... 2'048 buckets: nine tables, each the vector's one allocation of its storage.
    CHECK(memory.allocations() == 9);
    for (auto const key: std::views::iota(uint32_t { 0 }, uint32_t { 1'000 }))
    {
        auto const* const found = map.find(key * 7'919);
        REQUIRE(found != nullptr);
        CHECK(*found == key + 1);
    }
}
