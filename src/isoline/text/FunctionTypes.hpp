// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include <isoline/binary/Types.hpp>

namespace isoline::text
{

/// A module's function types as the text format defines them: explicit ones appended as written,
/// and implicit ones (spec §6.6.3) found as the first identical type or appended, in O(log n) type
/// comparisons per use.
///
/// The index is ordered, not hashed: a source chooses its types, so a hash of them could be collided on
/// purpose into a scan per use (as binary/TypeCanon.cpp orders its types for the same reason).
///
/// It refers to the module's type list, which it appends to; that list must outlive it and change
/// through it only.
class FunctionTypes
{
  public:
    /// Builds the index of @p types, which starts empty.
    /// @param types The module's types.
    explicit FunctionTypes(std::vector<binary::FuncType>& types);

    FunctionTypes(FunctionTypes const&) = delete;
    FunctionTypes(FunctionTypes&&) = delete;
    FunctionTypes& operator=(FunctionTypes const&) = delete;
    FunctionTypes& operator=(FunctionTypes&&) = delete;
    ~FunctionTypes() = default;

    /// Appends an explicit type, which is a type of its own even when it repeats an earlier one.
    /// @param type The type.
    /// @return Its index.
    uint32_t define(binary::FuncType type);

    /// The index of an implicit type: the first type identical to @p type, appended when there is none.
    /// @param type The type.
    /// @return Its index.
    uint32_t use(binary::FuncType type);

    /// How many times two types were compared: what a lookup costs, so that a test can tell
    /// logarithmic lookups from a scan without timing them.
    [[nodiscard]] std::size_t comparisons() const noexcept { return _comparisons; }

  private:
    /// Orders two type indices by the types they index, counting the comparisons.
    struct Less
    {
        std::vector<binary::FuncType> const* types; ///< The module's types.
        std::size_t* comparisons;                   ///< Where the comparisons are counted.
        /// @return Whether the type at @p a orders before the type at @p b.
        bool operator()(uint32_t a, uint32_t b) const noexcept
        {
            ++*comparisons;
            return (*types)[a] < (*types)[b];
        }
    };

    std::vector<binary::FuncType>* _types; ///< The module's types.
    std::size_t _comparisons = 0;          ///< How many times two types were compared.

    /// Every type of _types once, by the index of its first occurrence.
    std::set<uint32_t, Less> _firstIndices;
};

} // namespace isoline::text
