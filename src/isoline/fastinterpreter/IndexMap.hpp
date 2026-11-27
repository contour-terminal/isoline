// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <tuple>
#include <utility>

#include <isoline/fastinterpreter/LoweringAllocator.hpp>

namespace isoline::fastinterpreter
{

/// A hash map from a 32-bit index to a 32-bit value: lowering's map from a forwarded local to its newest
/// forwarder, which costs nothing for a local nobody forwards.
///
/// Open addressing with linear probing, in one LoweringVector, so that its only allocations are a vector's
/// storage, charged to the lowering arena at its throwing operations, and a standard library's bookkeeping
/// goes around the budget as every vector's does (LoweringAllocator); a node-based std::unordered_map
/// allocates bookkeeping of its own under types no allocator can tell from its storage. It grows by doubling
/// before it is half full, so a look-up probes O(1) buckets expected and an insertion is amortized O(1). An
/// erasure shifts the probe run back instead of leaving a tombstone, so look-ups never slow with use.
class IndexMap
{
  public:
    /// The one key no index is: what marks a bucket empty. Every index lowering maps is a local's, below
    /// the function's local count, which a 32-bit count bounds.
    static constexpr auto NoKey = std::numeric_limits<uint32_t>::max();

    /// An empty map, which allocates nothing until its first insertion.
    /// @param arena The lowering arena its storage comes from.
    explicit IndexMap(std::pmr::memory_resource* arena): _buckets(arena) {}

    /// The value of @p key.
    /// @param key The key; not NoKey (checked).
    /// @return The value, or null when @p key is not in the map.
    [[nodiscard]] uint32_t* find(uint32_t key)
    {
        auto const index = locate(key);
        return index == _buckets.size() ? nullptr : &_buckets[index].value;
    }

    /// The value of @p key, inserted as @p value where @p key is not in the map yet.
    /// @param key The key; not NoKey (checked).
    /// @param value The value an insertion gives it.
    /// @return The value of @p key, valid until the next insertion.
    [[nodiscard]] uint32_t& findOrInsert(uint32_t key, uint32_t value)
    {
        if (auto const index = locate(key); index != _buckets.size())
            return _buckets[index].value;
        if (2 * (_size + 1) > _buckets.size())
            grow();
        ++_size;
        return place(key, value);
    }

    /// Removes @p key, which is in the map (checked), and shifts back the run of buckets after it, so that
    /// every key stays reachable from its home bucket.
    /// @param key The key.
    void erase(uint32_t key)
    {
        auto hole = locate(key);
        Require(hole != _buckets.size());
        _buckets[hole].key = NoKey;
        --_size;
        for (auto index = next(hole); _buckets[index].key != NoKey; index = next(index))
        {
            auto const homeIndex = home(_buckets[index].key);
            // A bucket whose home lies cyclically within (hole, index] is reachable where it is.
            auto const reachable = hole < index ? (hole < homeIndex && homeIndex <= index)
                                                : (hole < homeIndex || homeIndex <= index);
            if (reachable)
                continue;
            _buckets[hole] = _buckets[index];
            _buckets[index].key = NoKey;
            hole = index;
        }
    }

    /// The keys in the map.
    /// @return Their number.
    [[nodiscard]] std::size_t size() const noexcept { return _size; }

  private:
    /// One bucket: a key and its value, or NoKey.
    struct Bucket
    {
        uint32_t key = NoKey; ///< Its key, or NoKey when it is empty.
        uint32_t value = 0;   ///< Its key's value.
    };

    /// The bucket of the first table, when the map first allocates.
    static constexpr auto InitialBuckets = std::size_t { 8 };

    /// The bucket holding @p key, or the table's size when @p key is not in the map.
    /// @param key The key; not NoKey (checked).
    [[nodiscard]] std::size_t locate(uint32_t key) const
    {
        Require(key != NoKey);
        if (_buckets.empty())
            return 0;
        for (auto index = home(key);; index = next(index))
        {
            if (_buckets[index].key == key)
                return index;
            if (_buckets[index].key == NoKey)
                return _buckets.size();
        }
    }

    /// The bucket a probe for @p key starts at: Fibonacci hashing of the key into the table's size, a power
    /// of two.
    [[nodiscard]] std::size_t home(uint32_t key) const noexcept
    {
        constexpr auto Golden = uint64_t { 0x9E37'79B9'7F4A'7C15 };
        auto const shift = 64 - std::countr_zero(_buckets.size());
        return static_cast<std::size_t>((uint64_t { key } * Golden) >> shift);
    }

    /// The bucket after @p index, wrapping around.
    [[nodiscard]] std::size_t next(std::size_t index) const noexcept
    {
        return (index + 1) & (_buckets.size() - 1);
    }

    /// Puts @p key with @p value into its first empty bucket; @p key is not in the table, and a bucket is
    /// free.
    [[nodiscard]] uint32_t& place(uint32_t key, uint32_t value)
    {
        auto index = home(key);
        while (_buckets[index].key != NoKey)
            index = next(index);
        _buckets[index] = Bucket { .key = key, .value = value };
        return _buckets[index].value;
    }

    /// Doubles the table and moves every key into it.
    void grow()
    {
        auto old = LoweringVector<Bucket>(
            _buckets.empty() ? InitialBuckets : 2 * _buckets.size(), Bucket {}, _buckets.get_allocator());
        std::swap(old, _buckets);
        for (auto const& bucket: old)
        {
            if (bucket.key != NoKey)
                std::ignore = place(bucket.key, bucket.value);
        }
    }

    LoweringVector<Bucket> _buckets;
    std::size_t _size = 0;
};

} // namespace isoline::fastinterpreter
