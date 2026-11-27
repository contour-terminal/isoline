// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/validate/Subtyping.hpp>

namespace isoline::validate
{

/// What a core import requires, or what an export provides: a function, table, memory, global or tag
/// type, its concrete type indices naming types of one module's type section. It has binary::Import's
/// shape, so that a declared core module type (which is no module) states its imports and exports the
/// same way.
struct CoreExternType
{
    binary::ExternKind kind = binary::ExternKind::Func; ///< Selects the field below that applies.
    uint32_t typeIndex = 0;                             ///< Func and Tag: the function type's index.
    binary::TableType table {};                         ///< Table: the table's type.
    binary::MemoryType memory {};                       ///< Memory: the memory's type.
    binary::GlobalType global {};                       ///< Global: the global's type.

    /// Two extern types are equal when every field is.
    bool operator==(CoreExternType const&) const = default;
};

/// The extern type an import requires.
/// @param import The import.
/// @return Its kind and the type of that kind it names.
[[nodiscard]] CoreExternType externTypeOf(binary::Import const& import);

/// Whether limits @p sub match limits @p super: the same address type, at least @p super's minimum,
/// and, when @p super has a maximum, one no larger (the core spec's limits matching). A table or
/// memory of a store matches by its current size, which it passes as @p sub's minimum.
/// @param sub The limits provided.
/// @param super The limits required.
/// @return Whether @p sub may stand where @p super is required.
[[nodiscard]] bool limitsMatch(binary::Limits const& sub, binary::Limits const& super) noexcept;

/// Whether memory type @p sub matches @p super: the same page size, and matching limits.
/// @param sub The memory type provided.
/// @param super The memory type required.
/// @return Whether @p sub may stand where @p super is required.
[[nodiscard]] bool memoryTypeMatches(binary::MemoryType const& sub, binary::MemoryType const& super) noexcept;

/// Whether table type @p sub matches @p super: equivalent element types, as a table is read and
/// written through either side, and matching limits.
/// @param sub The table type provided.
/// @param super The table type required.
/// @param equivalent Whether a concrete type of @p sub's indices and one of @p super's are equivalent.
/// @return Whether @p sub may stand where @p super is required.
template <typename Equivalent>
[[nodiscard]] bool tableTypeMatches(binary::TableType const& sub,
                                    binary::TableType const& super,
                                    Equivalent const& equivalent)
{
    return isEquivalentWith(sub.element, super.element, equivalent) && limitsMatch(sub.limits, super.limits);
}

/// Whether global type @p sub matches @p super: the same mutability, and the value type by subtyping
/// when immutable, by equivalence when mutable, as it is written through either side then.
/// @param sub The global type provided.
/// @param super The global type required.
/// @param equivalent Whether a concrete type of @p sub's indices and one of @p super's are equivalent.
/// @return Whether @p sub may stand where @p super is required.
template <typename Equivalent>
[[nodiscard]] bool globalTypeMatches(binary::GlobalType const& sub,
                                     binary::GlobalType const& super,
                                     Equivalent const& equivalent)
{
    if (sub.mutability != super.mutability)
        return false;
    if (super.mutability == binary::Mutability::Var)
        return isEquivalentWith(sub.type, super.type, equivalent);
    return isSubtypeWith(sub.type, super.type, equivalent);
}

/// Matches the types of one module (the sub side, which provides) against another's (the super side,
/// which requires), as core import matching does. Without GC there are no recursion groups and no
/// declared supertypes, so two concrete types match exactly when they are equivalent, the core spec's
/// type equivalence across modules, and a function or tag matches only an equivalent type.
///
/// It holds the joint space of the two type sections: binary::jointCanonicalTypeIds() over the sub side's
/// types followed by the super side's, each section's concrete indices counted from its own start, so
/// that two types are equivalent exactly when their joint ids are equal. Building it costs
/// O((n + m) log (n + m)) comparisons and memory linear in the two sections, and nothing is
/// recursive; every query after is O(1) for a type and allocates nothing, however many there are. It
/// keeps no reference to either section. A caller matching many imports between the same two
/// modules builds one matcher for them.
class TypeMatcher
{
  public:
    /// The joint space of @p sub's and @p super's types.
    ///
    /// Precondition (checked): every concrete type index of type i of either section is below i, as
    /// validation checks.
    /// @param sub The providing module's type section.
    /// @param super The requiring module's type section.
    TypeMatcher(std::span<binary::FuncType const> sub, std::span<binary::FuncType const> super);

    /// Whether type @p sub of the providing module matches type @p super of the requiring one.
    /// @param sub A type index of the providing module (checked).
    /// @param super A type index of the requiring module (checked).
    /// @return Whether they are equivalent: their joint ids are equal.
    [[nodiscard]] bool matches(uint32_t sub, uint32_t super) const;

    /// Whether extern type @p sub of the providing module matches @p super of the requiring one: the
    /// same kind, and a function or tag of an equivalent type, a table, memory or global as
    /// tableTypeMatches(), memoryTypeMatches() and globalTypeMatches() decide.
    /// @param sub What is provided, in the providing module's indices (checked: in its section).
    /// @param super What is required, in the requiring module's indices (checked: in its section).
    /// @return Whether an import of @p super may be satisfied by an export of @p sub.
    [[nodiscard]] bool matchesExtern(CoreExternType const& sub, CoreExternType const& super) const;

  private:
    std::size_t _subCount;      ///< The providing section's type count: where the requiring one's ids begin.
    std::vector<uint32_t> _ids; ///< The joint ids, the providing section's types, then the requiring one's.
};

/// Whether type @p sub of the type section @p a matches type @p super of @p b (core import matching),
/// types compared across the two sections by canonical structure: the check runtime instantiation
/// makes, available without a store. One-shot: it builds the two sections' joint space for this one
/// query, so a caller matching many pairs of the same two modules builds one TypeMatcher instead.
/// @param a The providing module's type section.
/// @param sub A type index of @p a (checked).
/// @param b The requiring module's type section.
/// @param super A type index of @p b (checked).
/// @return true when an import of a function of type @p super may be satisfied by one of type @p sub.
[[nodiscard]] bool matches(std::span<binary::FuncType const> a,
                           uint32_t sub,
                           std::span<binary::FuncType const> b,
                           uint32_t super);

/// The same for whole extern types (func, table, memory, global, tag), limits included.
/// @param a The providing module's type section.
/// @param sub What is provided, in @p a's indices.
/// @param b The requiring module's type section.
/// @param super What is required, in @p b's indices.
/// @return true when an import of @p super may be satisfied by an export of @p sub.
[[nodiscard]] bool matchesExtern(std::span<binary::FuncType const> a,
                                 CoreExternType const& sub,
                                 std::span<binary::FuncType const> b,
                                 CoreExternType const& super);

} // namespace isoline::validate
