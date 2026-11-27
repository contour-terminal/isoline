// SPDX-License-Identifier: Apache-2.0
#include <isoline/validate/TypeMatching.hpp>

#include <core/Assert.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/TypeCanon.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/validate/Subtyping.hpp>

namespace isoline::validate
{

namespace
{
    /// The partner of a canonical type that has none yet.
    constexpr auto NoPartner = std::numeric_limits<uint32_t>::max();

    /// The value type at @p position of @p type: its params, then its results.
    [[nodiscard]] binary::ValueType const& valueTypeAt(binary::FuncType const& type, uint32_t position)
    {
        if (position < type.params.size())
            return type.params[position];
        return type.results[position - type.params.size()];
    }

    /// The number of value types of @p type, params and results.
    [[nodiscard]] uint32_t valueTypeCount(binary::FuncType const& type) noexcept
    {
        return static_cast<uint32_t>(type.params.size() + type.results.size());
    }

    /// Whether @p first and @p second agree in everything but the types their concrete references
    /// name: as many params and results, each of one kind, nullability and heap kind.
    [[nodiscard]] bool shallowlyEqual(binary::FuncType const& first, binary::FuncType const& second)
    {
        if (first.params.size() != second.params.size() || first.results.size() != second.results.size())
            return false;
        auto const anyConcrete = [](uint32_t /*firstIndex*/, uint32_t /*secondIndex*/) {
            return true;
        };
        for (auto const position: std::views::iota(uint32_t { 0 }, valueTypeCount(first)))
            if (!isEquivalentWith(valueTypeAt(first, position), valueTypeAt(second, position), anyConcrete))
                return false;
        return true;
    }
} // namespace

CoreExternType externTypeOf(binary::Import const& import)
{
    return CoreExternType { .kind = import.kind,
                            .typeIndex = import.typeIndex,
                            .table = import.table,
                            .memory = import.memory,
                            .global = import.global };
}

bool limitsMatch(binary::Limits const& sub, binary::Limits const& super) noexcept
{
    if (sub.addressType != super.addressType || sub.min < super.min)
        return false;
    return !super.max || (sub.max && *sub.max <= *super.max);
}

bool memoryTypeMatches(binary::MemoryType const& sub, binary::MemoryType const& super) noexcept
{
    return sub.pageSizeLog2 == super.pageSizeLog2 && limitsMatch(sub.limits, super.limits);
}

CanonicalTypes::CanonicalTypes(std::span<binary::FuncType const> types):
    _types { types }, _ids { binary::canonicalTypeIds(types) }
{
}

uint32_t CanonicalTypes::canonicalId(uint32_t index) const
{
    Require(index < _ids.size());
    return _ids[index];
}

TypeMatcher::TypeMatcher(CanonicalTypes const& sub, CanonicalTypes const& super):
    _sub { sub },
    _super { super },
    _partnerOfSub(sub.types().size(), NoPartner),
    _partnerOfSuper(super.types().size(), NoPartner)
{
}

TypeMatcher::Verdict TypeMatcher::known(uint32_t sub, uint32_t super) const
{
    if (_partnerOfSub[sub] != NoPartner)
        return _partnerOfSub[sub] == super ? Verdict::Equivalent : Verdict::Different;
    // A super type equivalent to another sub type is equivalent to no other (they are inequivalent).
    if (_partnerOfSuper[super] != NoPartner || _different.contains({ sub, super }))
        return Verdict::Different;
    return Verdict::Unknown;
}

bool TypeMatcher::push(Stack& stack, uint32_t sub, uint32_t super)
{
    ++_comparisons;
    stack.push_back(Frame { .sub = sub, .super = super, .position = 0 });
    return shallowlyEqual(_sub.types()[sub], _super.types()[super]);
}

void TypeMatcher::fail(Stack const& stack)
{
    // Each pair on the stack is an ancestor of the one above it, so each needed the pair that failed.
    for (auto const& frame: stack)
        _different.emplace(frame.sub, frame.super);
}

bool TypeMatcher::matches(uint32_t sub, uint32_t super)
{
    auto const root = std::pair { _sub.canonicalId(sub), _super.canonicalId(super) };
    if (auto const verdict = known(root.first, root.second); verdict != Verdict::Unknown)
        return verdict == Verdict::Equivalent;
    auto stack = Stack {};
    if (!push(stack, root.first, root.second))
    {
        fail(stack);
        return false;
    }
    while (!stack.empty())
    {
        auto& frame = stack.back();
        auto const& subType = _sub.types()[frame.sub];
        auto const& superType = _super.types()[frame.super];
        auto child = std::optional<std::pair<uint32_t, uint32_t>> {};
        while (!child && frame.position < valueTypeCount(subType))
        {
            auto const& subValue = valueTypeAt(subType, frame.position);
            auto const& superValue = valueTypeAt(superType, frame.position);
            ++frame.position;
            if (!binary::isConcreteRef(subValue))
                continue; // shallowlyEqual() compared it whole
            auto const pair = std::pair { _sub.canonicalId(subValue.heap.typeIndex),
                                          _super.canonicalId(superValue.heap.typeIndex) };
            switch (known(pair.first, pair.second))
            {
                case Verdict::Equivalent: break;
                case Verdict::Different: fail(stack); return false;
                case Verdict::Unknown: child = pair; break;
            }
        }
        if (child)
        {
            if (!push(stack, child->first, child->second))
            {
                fail(stack);
                return false;
            }
            continue;
        }
        _partnerOfSub[frame.sub] = frame.super;
        _partnerOfSuper[frame.super] = frame.sub;
        stack.pop_back();
    }
    return true;
}

bool TypeMatcher::matchesExtern(CoreExternType const& sub, CoreExternType const& super)
{
    if (sub.kind != super.kind)
        return false;
    auto const equivalent = [this](uint32_t subIndex, uint32_t superIndex) {
        return matches(subIndex, superIndex);
    };
    switch (sub.kind)
    {
        case binary::ExternKind::Func: return matches(sub.typeIndex, super.typeIndex);
        case binary::ExternKind::Table: return tableTypeMatches(sub.table, super.table, equivalent);
        case binary::ExternKind::Memory: return memoryTypeMatches(sub.memory, super.memory);
        case binary::ExternKind::Global: return globalTypeMatches(sub.global, super.global, equivalent);
        case binary::ExternKind::Tag: break; // below the switch, so that every kind is a case
    }
    // A tag's type matches only an equivalent one: an exception's payload is read by the tag's type.
    return matches(sub.typeIndex, super.typeIndex);
}

bool matches(std::span<binary::FuncType const> a,
             uint32_t sub,
             std::span<binary::FuncType const> b,
             uint32_t super)
{
    auto const subTypes = CanonicalTypes { a };
    auto const superTypes = CanonicalTypes { b };
    return TypeMatcher { subTypes, superTypes }.matches(sub, super);
}

bool matchesExtern(std::span<binary::FuncType const> a,
                   CoreExternType const& sub,
                   std::span<binary::FuncType const> b,
                   CoreExternType const& super)
{
    auto const subTypes = CanonicalTypes { a };
    auto const superTypes = CanonicalTypes { b };
    return TypeMatcher { subTypes, superTypes }.matchesExtern(sub, super);
}

} // namespace isoline::validate
