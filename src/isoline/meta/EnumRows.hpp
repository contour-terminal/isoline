// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

// The checks of the tables indexed by an enum: one row per enumerator, each at its enumerator's index.
// They are C++23 and installed, so that every table states its check through them on every compiler,
// and a reflection check that finds a table wrong names the row firstMisplacedRow() finds. Beside them
// is the check of a table keyed by a member, a name or a spelling, which a lookup by that key reads:
// firstRepeatedKey(). Reflection adds nothing to it, so such a table has no row in EnumTables.def.

namespace isoline::meta
{

/// What firstMisplacedRow() returns for a table whose every row is in place.
inline constexpr auto NoMisplacedRow = std::numeric_limits<std::size_t>::max();

/// The number of enumerators 0 through @p Last: the rows of a table indexed by decltype(Last).
template <auto Last>
    requires std::is_enum_v<decltype(Last)>
inline constexpr auto EnumeratorCount = static_cast<std::size_t>(std::to_underlying(Last)) + 1;

/// The first row out of place in @p rows, a table of one row per enumerator of decltype(Last), 0
/// through @p Last, each at its enumerator's index.
///
/// A row is out of place when its @p key names another enumerator than its index. A table that is
/// short is out of place at its first missing row, and one that is long at its first extra row.
/// @param rows The table.
/// @param key The member naming each row's enumerator.
/// @return The index of that row, or NoMisplacedRow when every row is in place and none is missing or
///         extra.
template <auto Last, typename Row, std::size_t Count>
    requires std::is_enum_v<decltype(Last)>
[[nodiscard]] consteval std::size_t firstMisplacedRow(std::array<Row, Count> const& rows,
                                                      decltype(Last) Row::* key) noexcept
{
    auto index = std::size_t { 0 };
    for (auto const& row: rows)
    {
        if (static_cast<std::size_t>(std::to_underlying(row.*key)) != index)
            return index;
        ++index;
    }
    return index == EnumeratorCount<Last> ? NoMisplacedRow : std::min(index, EnumeratorCount<Last>);
}

/// Whether @p rows hold one row per enumerator of decltype(Last), 0 through @p Last, each at its
/// enumerator's index.
/// @param rows The table.
/// @param key The member naming each row's enumerator.
/// @return Whether every row is in place and none is missing or extra.
template <auto Last, typename Row, std::size_t Count>
    requires std::is_enum_v<decltype(Last)>
[[nodiscard]] consteval bool rowsAtEnumeratorIndex(std::array<Row, Count> const& rows,
                                                   decltype(Last) Row::* key) noexcept
{
    return firstMisplacedRow<Last>(rows, key) == NoMisplacedRow;
}

/// Whether a table of @p count entries has one entry per enumerator 0 through @p Last.
/// @param count The table's size: an array's, or a variant's number of alternatives.
/// @return Whether it is decltype(Last)'s number of enumerators.
template <auto Last>
    requires std::is_enum_v<decltype(Last)>
[[nodiscard]] consteval bool sizedByEnum(std::size_t count) noexcept
{
    return count == EnumeratorCount<Last>;
}

/// What firstRepeatedKey() returns for a table no two rows of which share a key.
inline constexpr auto NoRepeatedKey = std::numeric_limits<std::size_t>::max();

namespace detail
{
    /// firstRepeatedKey(), where @p unkeyed is empty when every row has a key. A value, not a pointer:
    /// GCC 14 and 15 refuse to compare the address of a temporary with null in constant evaluation.
    template <typename Row, std::size_t Count, typename Key>
    [[nodiscard]] consteval std::size_t firstRepeatedKeyOf(std::array<Row, Count> const& rows,
                                                           Key Row::* key,
                                                           std::optional<Key> const& unkeyed) noexcept
    {
        auto index = std::size_t { 0 };
        for (auto const& row: rows)
        {
            auto const earlier = std::span(rows).first(index);
            if ((!unkeyed.has_value() || row.*key != *unkeyed)
                && std::ranges::find(earlier, row.*key, key) != earlier.end())
                return index;
            ++index;
        }
        return NoRepeatedKey;
    }
} // namespace detail

/// The first row of @p rows whose @p key an earlier row holds: a lookup by that key finds the earlier
/// row only.
/// @param rows The table.
/// @param key The member keying each row.
/// @return The index of that row, or NoRepeatedKey when no two rows share a key.
template <typename Row, std::size_t Count, typename Key>
[[nodiscard]] consteval std::size_t firstRepeatedKey(std::array<Row, Count> const& rows,
                                                     Key Row::* key) noexcept
{
    return detail::firstRepeatedKeyOf(rows, key, std::optional<Key> {});
}

/// The first row of @p rows whose @p key an earlier row holds, but @p unkeyed, which means the row has
/// no such key and may repeat: a lookup by that key finds the earlier row only.
/// @param rows The table.
/// @param key The member keying each row.
/// @param unkeyed The value of @p key a row without one holds.
/// @return The index of that row, or NoRepeatedKey when no two rows share a key but @p unkeyed.
template <typename Row, std::size_t Count, typename Key>
[[nodiscard]] consteval std::size_t firstRepeatedKey(std::array<Row, Count> const& rows,
                                                     Key Row::* key,
                                                     std::type_identity_t<Key> const& unkeyed) noexcept
{
    return detail::firstRepeatedKeyOf(rows, key, std::optional<Key> { unkeyed });
}

} // namespace isoline::meta
