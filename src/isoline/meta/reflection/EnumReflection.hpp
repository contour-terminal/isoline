// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <meta>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <isoline/meta/EnumRows.hpp>

// C++26 reflection checks of the tables indexed by an enum, which add what C++23 cannot see: that a
// table's Last is its enum's final enumerator, that the enumerators are dense from 0, and that every
// enum-first table is registered in EnumTables.def. Only compiled where reflection takes the native
// path (cmake/IsolineLanguageFeatures.cmake), and never installed. Reflection only verifies: the
// tables and EnumTables.def stay the source, and a misplaced row is the one firstMisplacedRow() finds.

namespace isoline::meta::reflection
{

/// Where a table's Last stands among its enum's enumerators.
enum class LastEnumerator : uint8_t
{
    Prefix, ///< Anywhere: the table covers 0 through Last, and the enumerators after it are spelled apart.
    Final,  ///< The enum's final enumerator: the table covers every one.
};

/// What discovery does with a namespace an ImplicitNamespaces row names.
enum class ImplicitNamespace : uint8_t
{
    Skipped, ///< Neither walked for tables nor classified: test data, or the checking machinery itself.
    Walked,  ///< Walked for tables as part of its parent, and classified without a list naming it.
};

/// Where an ImplicitNamespaces row applies.
enum class ImplicitDepth : uint8_t
{
    Anywhere,  ///< At every depth under the root.
    UnderRoot, ///< Only directly under the root.
};

/// One namespace name the checks know without a list naming it.
struct ImplicitNamespaceRow
{
    std::string_view name;  ///< The namespace's identifier; empty for an anonymous namespace.
    ImplicitDepth depth;    ///< Where the name is implicit.
    ImplicitNamespace kind; ///< Whether discovery walks it.
};

/// The namespace names every list implies: an anonymous or detail namespace belongs to its parent, a
/// testing namespace holds test data, and the root's meta holds these checks. Any other namespace under
/// the root, at any depth, is walked, and is unclassified until a list names it.
inline constexpr auto ImplicitNamespaces = std::to_array<ImplicitNamespaceRow>({
    { .name = "", .depth = ImplicitDepth::Anywhere, .kind = ImplicitNamespace::Walked },
    { .name = "detail", .depth = ImplicitDepth::Anywhere, .kind = ImplicitNamespace::Walked },
    { .name = "testing", .depth = ImplicitDepth::Anywhere, .kind = ImplicitNamespace::Skipped },
    { .name = "meta", .depth = ImplicitDepth::UnderRoot, .kind = ImplicitNamespace::Skipped },
});

namespace detail
{
    /// @p value in decimal.
    /// @param value The number.
    /// @return Its digits.
    [[nodiscard]] consteval std::string decimal(std::size_t value)
    {
        auto digits = std::array<char, std::numeric_limits<std::size_t>::digits10 + 1> {};
        auto const written = std::to_chars(digits.data(), digits.data() + digits.size(), value);
        return std::string { digits.data(), written.ptr };
    }

    /// The enumerator of @p Enum whose value is @p value, spelled `Enum::Name`, or
    /// `static_cast<Enum>(value)` when none is.
    /// @param value The underlying value.
    /// @return Its spelling.
    template <typename Enum>
    [[nodiscard]] consteval std::string enumeratorNamed(std::size_t value)
    {
        auto const enumName = std::string { std::meta::identifier_of(^^Enum) };
        for (auto const enumerator: std::meta::enumerators_of(^^Enum))
            if (static_cast<std::size_t>(std::to_underlying(std::meta::extract<Enum>(enumerator))) == value)
                return enumName + "::" + std::string { std::meta::identifier_of(enumerator) };
        return "static_cast<" + enumName + ">(" + decimal(value) + ")";
    }

    /// The ImplicitNamespaces row that names @p space, a namespace under @p root, if any.
    /// @param space A namespace's reflection.
    /// @param root The root it is under.
    /// @return Its row, or nullptr for a namespace no row names where it is.
    [[nodiscard]] consteval ImplicitNamespaceRow const* implicitRowOf(std::meta::info space,
                                                                      std::meta::info root)
    {
        auto const name =
            std::meta::has_identifier(space) ? std::meta::identifier_of(space) : std::string_view {};
        auto const underRoot = std::meta::parent_of(space) == root;
        for (auto const& row: ImplicitNamespaces)
            if (row.name == name && (row.depth == ImplicitDepth::Anywhere || underRoot))
                return &row;
        return nullptr;
    }

    /// Whether @p entity is one of @p list.
    [[nodiscard]] consteval bool isListed(std::meta::info entity, std::span<std::meta::info const> list)
    {
        return std::ranges::find(list, entity) != list.end();
    }

    /// Whether @p member is a namespace proper, not an alias of one.
    [[nodiscard]] consteval bool isNamespace(std::meta::info member)
    {
        return std::meta::is_namespace(member) && !std::meta::is_namespace_alias(member);
    }

    /// Whether @p variable is a std::array of rows whose first member is a scoped enum.
    [[nodiscard]] consteval bool isEnumFirstTable(std::meta::info variable)
    {
        auto const type = std::meta::remove_cvref(std::meta::type_of(variable));
        if (!std::meta::has_template_arguments(type) || std::meta::template_of(type) != ^^std::array)
            return false;
        auto const row = std::meta::template_arguments_of(type).front();
        if (!std::meta::is_class_type(row))
            return false;
        auto const members =
            std::meta::nonstatic_data_members_of(row, std::meta::access_context::unchecked());
        return !members.empty() && std::meta::is_scoped_enum_type(std::meta::type_of(members.front()));
    }

    /// Appends the namespaces under @p space that discovery walks, each before its own, to @p walked:
    /// every one but those an ImplicitNamespaces row skips, and what is under them.
    consteval void collectWalked(std::meta::info space,
                                 std::meta::info root,
                                 std::vector<std::meta::info>& walked)
    {
        for (auto const member: std::meta::members_of(space, std::meta::access_context::unchecked()))
        {
            if (!isNamespace(member))
                continue;
            auto const* const implicit = implicitRowOf(member, root);
            if (implicit != nullptr && implicit->kind == ImplicitNamespace::Skipped)
                continue;
            walked.push_back(member);
            collectWalked(member, root, walked);
        }
    }

    /// @p root and every namespace under it that discovery walks, in declaration order, each parent
    /// before its children.
    [[nodiscard]] consteval std::vector<std::meta::info> walkedNamespaces(std::meta::info root)
    {
        auto walked = std::vector<std::meta::info> { root };
        collectWalked(root, root, walked);
        return walked;
    }

    /// Whether @p variable is a Last constant: a variable named Last… of a scoped enum type.
    [[nodiscard]] consteval bool isLastConstant(std::meta::info variable)
    {
        return std::meta::has_identifier(variable) && std::meta::identifier_of(variable).starts_with("Last")
               && std::meta::is_scoped_enum_type(std::meta::remove_cv(std::meta::type_of(variable)));
    }

    /// The namespace-scope variables of the namespaces discovery walks under @p root that @p select
    /// accepts.
    [[nodiscard]] consteval std::vector<std::meta::info> discoveredVariables(std::meta::info root,
                                                                             bool (*select)(std::meta::info))
    {
        auto variables = std::vector<std::meta::info> {};
        for (auto const space: walkedNamespaces(root))
            for (auto const member: std::meta::members_of(space, std::meta::access_context::unchecked()))
                if (std::meta::is_variable(member) && select(member))
                    variables.push_back(member);
        return variables;
    }
} // namespace detail

/// Why @p Enum's enumerators are not 0, 1, … in declaration order with @p last final (or, for a
/// prefix table, merely dense through it).
/// @param last The reflection of the table's Last: a variable holding an enumerator, or the enumerator.
/// @param position Whether @p last must be the final enumerator.
/// @return The problem, naming the enumerator at fault; empty when there is none.
template <typename Enum>
    requires std::is_scoped_enum_v<Enum>
[[nodiscard]] consteval std::string enumeratorsProblem(std::meta::info last, LastEnumerator position)
{
    auto const enumName = std::string { std::meta::identifier_of(^^Enum) };
    auto const lastValue =
        static_cast<std::size_t>(std::to_underlying(std::meta::extract<Enum>(std::meta::constant_of(last))));
    auto const lastName = std::meta::is_enumerator(last)
                              ? detail::enumeratorNamed<Enum>(lastValue)
                              : std::string { std::meta::identifier_of(last) } + " ("
                                    + detail::enumeratorNamed<Enum>(lastValue) + ")";
    auto lastSeen = false;
    for (auto const& [index, enumerator]: core::views::enumerate(std::meta::enumerators_of(^^Enum)))
    {
        auto const name = enumName + "::" + std::string { std::meta::identifier_of(enumerator) };
        if (lastSeen)
            return name + " follows " + lastName + ", which must be " + enumName + "'s final enumerator";
        auto const value = static_cast<std::size_t>(std::to_underlying(std::meta::extract<Enum>(enumerator)));
        if (value != index)
            return name + " is " + detail::decimal(value) + ", not " + detail::decimal(index) + ": "
                   + enumName + "'s enumerators must be 0, 1, 2 and on, in declaration order";
        if (value == lastValue)
        {
            if (position == LastEnumerator::Prefix)
                return {};
            lastSeen = true;
        }
    }
    if (!lastSeen)
        return std::string { std::meta::identifier_of(last) } + " is " + detail::decimal(lastValue)
               + ", which names no enumerator of " + enumName;
    return {};
}

/// Why @p Table is not one row per enumerator 0 through @p Last at its index, naming the row and the
/// enumerator. The row is the one firstMisplacedRow() finds; this only names it.
/// @return The problem; empty when every row is in place.
template <auto const& Table, auto Last, auto Key>
    requires std::is_enum_v<decltype(Last)>
[[nodiscard]] consteval std::string indexedTableProblem()
{
    using Enum = decltype(Last);
    constexpr auto index = firstMisplacedRow<Last>(Table, Key);
    if constexpr (index == NoMisplacedRow)
        return {};
    else if constexpr (index >= Table.size())
        return "it has no row for " + detail::enumeratorNamed<Enum>(index) + ", its row "
               + detail::decimal(index);
    else
    {
        auto const named =
            detail::enumeratorNamed<Enum>(static_cast<std::size_t>(std::to_underlying(Table[index].*Key)));
        if constexpr (index >= EnumeratorCount<Last>)
            return "row " + detail::decimal(index) + ", which names " + named + ", is past "
                   + detail::enumeratorNamed<Enum>(index - 1) + ", the last row's enumerator";
        else
            return "row " + detail::decimal(index) + " names " + named + ", where "
                   + detail::enumeratorNamed<Enum>(index) + " belongs";
    }
}

/// Whether @p member is the first non-static data member of its class: a key discovery sees.
/// @param member The reflection of a non-static data member.
/// @return Whether it is declared before every other.
[[nodiscard]] consteval bool isFirstMember(std::meta::info member)
{
    auto const members = std::meta::nonstatic_data_members_of(std::meta::parent_of(member),
                                                              std::meta::access_context::unchecked());
    return !members.empty() && members.front() == member;
}

/// Every namespace-scope std::array<Row, N> variable whose Row's first member is a scoped enum, in
/// @p root and every namespace under it but those ImplicitNamespaces skips (a testing namespace at any
/// depth, the root's meta).
/// @param root The namespace to walk.
/// @return The tables' reflections, in declaration order, each namespace before its children.
[[nodiscard]] consteval std::vector<std::meta::info> discoveredTables(std::meta::info root)
{
    return detail::discoveredVariables(root, detail::isEnumFirstTable);
}

/// Every namespace-scope Last constant (a variable named Last… of a scoped enum type) in the
/// namespaces discoveredTables() walks.
/// @param root The namespace to walk.
/// @return The constants' reflections, in the same order.
[[nodiscard]] consteval std::vector<std::meta::info> discoveredLastConstants(std::meta::info root)
{
    return detail::discoveredVariables(root, detail::isLastConstant);
}

/// The namespaces under @p root, at any depth, that discovery walks but neither @p namespaces nor
/// ImplicitNamespaces names (an anonymous or detail namespace is its parent's).
/// @param root The namespace whose descendants are classified.
/// @param namespaces The namespaces a list names.
/// @return The unclassified ones, each parent before its children.
[[nodiscard]] consteval std::vector<std::meta::info> unclassifiedNamespaces(
    std::meta::info root, std::span<std::meta::info const> namespaces)
{
    auto unclassified = std::vector<std::meta::info> {};
    for (auto const space: detail::walkedNamespaces(root))
        if (space != root && !detail::isListed(space, namespaces)
            && detail::implicitRowOf(space, root) == nullptr)
            unclassified.push_back(space);
    return unclassified;
}

/// The entities of @p entities that @p list does not hold, by name: an unregistered table, or a
/// registry row discovery does not find.
/// @param entities The entities to look up.
/// @param list Where to look them up.
/// @return Their qualified names, separated by ", "; empty when every one is listed.
[[nodiscard]] consteval std::string unlistedNames(std::span<std::meta::info const> entities,
                                                  std::span<std::meta::info const> list)
{
    auto names = std::string {};
    for (auto const entity: entities)
    {
        if (detail::isListed(entity, list))
            continue;
        if (!names.empty())
            names += ", ";
        names += std::meta::display_string_of(entity);
    }
    return names;
}

} // namespace isoline::meta::reflection
