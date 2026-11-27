// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <set>
#include <span>
#include <utility>
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

/// A module's type section with the canonical id of each of its types (binary::canonicalTypeIds), the
/// form TypeMatcher compares two modules' types in. Built once per module, it serves every matcher
/// of that module, so the O(n log n) canonicalization is not paid per import.
class CanonicalTypes
{
  public:
    /// Canonicalizes @p types.
    ///
    /// Precondition (checked): every concrete type index of type i is below i, as validation checks.
    /// @param types The type section; it must outlive this object.
    explicit CanonicalTypes(std::span<binary::FuncType const> types);

    /// The type section.
    [[nodiscard]] std::span<binary::FuncType const> types() const noexcept { return _types; }

    /// The canonical id of type @p index: the index of the first type of the section equivalent to it.
    /// @param index A type index (checked: in the section).
    /// @return Its canonical id, at most @p index.
    [[nodiscard]] uint32_t canonicalId(uint32_t index) const;

  private:
    std::span<binary::FuncType const> _types;
    std::vector<uint32_t> _ids;
};

/// Matches the types of one module (the sub side, which provides) against another's (the super side,
/// which requires), as core import matching does: two concrete types match when they are equivalent,
/// structurally over the canonical ids of the types they name. Without GC there are no recursion
/// groups and no declared supertypes, so the core spec's type equivalence across modules is exactly
/// that, and a function or tag matches only an equivalent type.
///
/// Every pair of types is compared on an explicit work stack, never by recursion, and its verdict is
/// remembered for the matcher's lifetime: a pair found equivalent records the partner of each side's
/// canonical type (each has at most one, since a module's canonical types are pairwise
/// inequivalent), a pair found not to is kept in an ordered set. So a type pair is compared once
/// however many imports name it, and the work of every query together is linear in the types
/// reached, times the ordered set's logarithm; no hash keyed by guest data is involved.
class TypeMatcher
{
  public:
    /// A matcher of @p sub's types against @p super's.
    /// @param sub The module that provides; it must outlive this object.
    /// @param super The module that requires; it must outlive this object.
    TypeMatcher(CanonicalTypes const& sub, CanonicalTypes const& super);

    /// Whether type @p sub of the providing module matches type @p super of the requiring one.
    /// @param sub A type index of the providing module (checked).
    /// @param super A type index of the requiring module (checked).
    /// @return Whether they are equivalent.
    [[nodiscard]] bool matches(uint32_t sub, uint32_t super);

    /// Whether extern type @p sub of the providing module matches @p super of the requiring one: the
    /// same kind, and a function or tag of an equivalent type, a table, memory or global as
    /// tableTypeMatches(), memoryTypeMatches() and globalTypeMatches() decide.
    /// @param sub What is provided, in the providing module's indices (checked: in its section).
    /// @param super What is required, in the requiring module's indices (checked: in its section).
    /// @return Whether an import of @p super may be satisfied by an export of @p sub.
    [[nodiscard]] bool matchesExtern(CoreExternType const& sub, CoreExternType const& super);

    /// How many type pairs this matcher has compared structurally so far; each at most once.
    [[nodiscard]] uint64_t comparisons() const noexcept { return _comparisons; }

  private:
    /// A pair of canonical ids under comparison, and the next of its value types to compare.
    struct Frame
    {
        uint32_t sub = 0;      ///< The providing side's canonical id.
        uint32_t super = 0;    ///< The requiring side's canonical id.
        uint32_t position = 0; ///< The next value type (params, then results) whose children to visit.
    };

    /// What is known of a pair of canonical ids.
    enum class Verdict : uint8_t
    {
        Unknown,
        Different,
        Equivalent,
    };

    /// The stack of one matches() call: a local, so that a comparison the host ran out of memory in
    /// leaves nothing behind but the verdicts it had completed.
    using Stack = std::vector<Frame>;

    /// What the verdicts so far say of the pair (@p sub, @p super) of canonical ids.
    [[nodiscard]] Verdict known(uint32_t sub, uint32_t super) const;

    /// Pushes the pair (@p sub, @p super) of canonical ids, counting the comparison.
    /// @return Whether the two agree in all but the types they name (if not, the pair has failed).
    [[nodiscard]] bool push(Stack& stack, uint32_t sub, uint32_t super);

    /// Records every pair of @p stack as different: the top one failed, and each needed the one above.
    void fail(Stack const& stack);

    CanonicalTypes const& _sub;
    CanonicalTypes const& _super;
    std::vector<uint32_t> _partnerOfSub;                ///< By canonical id: its equivalent, or NoPartner.
    std::vector<uint32_t> _partnerOfSuper;              ///< By canonical id: its equivalent, or NoPartner.
    std::set<std::pair<uint32_t, uint32_t>> _different; ///< Pairs found not equivalent.
    uint64_t _comparisons = 0;
};

/// Whether type @p sub of the type section @p a matches type @p super of @p b (core import matching),
/// types compared across the two sections by canonical structure: the check runtime instantiation
/// makes, available without a store. One-shot: a caller matching many pairs of the same two modules
/// builds one TypeMatcher instead.
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
