// SPDX-License-Identifier: Apache-2.0
#pragma once

// The reflected surface of a module: std::meta::members_of over each of its public namespaces, named as the
// export lint (scripts/check-module-exports.py --list) names what it lists, so that the two can be compared
// (scripts/compare-module-surface.py). Only a compiler with reflection native includes this header.
//
// A name is what the lint makes of a namespace's member: a nested namespace is none (each public one is a row
// of its own, and the lint classifies the others); an unnamed member is none, but an unscoped enumeration's
// enumerators are names of its namespace, named or not; an operator function is `operator<symbol>`, as Clang
// names it. A using-declaration is no member at all (ISOLINE_MODULE_SURFACE_UNREFLECTED_KINDS,
// cmake/IsolineModules.cmake).

#include <algorithm>
#include <meta>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace isoline::meta::reflection
{

/// A public namespace of a module, and how the lint spells it.
struct SurfaceNamespace
{
    std::meta::info space; ///< The namespace's reflection.
    std::string_view name; ///< Its qualified name.
};

namespace detail
{
    /// The name the lint gives @p member, a member of a namespace, or an empty string for a member that names
    /// nothing of its own (a nested namespace, an unnamed entity).
    /// @param member The member's reflection.
    /// @return Its unqualified name, or "".
    [[nodiscard]] consteval std::string surfaceLeafOf(std::meta::info member)
    {
        if (std::meta::is_namespace(member))
            return {};
        if (std::meta::is_operator_function(member) || std::meta::is_operator_function_template(member))
            return std::string("operator")
                   + std::string(std::meta::symbol_of(std::meta::operator_of(member)));
        if (std::meta::has_identifier(member))
            return std::string(std::meta::identifier_of(member));
        return {};
    }

    /// Whether @p member is an unscoped enumeration, whose enumerators are names of its namespace. An alias
    /// of one is not: its enumerators are the enumeration's own namespace's.
    /// @param member A namespace's member.
    /// @return Whether its enumerators are the namespace's names.
    [[nodiscard]] consteval bool isUnscopedEnumeration(std::meta::info member)
    {
        return std::meta::is_type(member) && !std::meta::is_type_alias(member)
               && std::meta::is_enum_type(member) && !std::meta::is_scoped_enum_type(member);
    }
} // namespace detail

/// The qualified names of the members of every namespace of @p namespaces, sorted, each once.
/// @param namespaces The public namespaces.
/// @return The names.
[[nodiscard]] consteval std::vector<std::string> surfaceNamesOf(std::span<SurfaceNamespace const> namespaces)
{
    std::vector<std::string> names;
    for (auto const& row: namespaces)
    {
        auto const prefix = std::string(row.name) + "::";
        for (auto const member: std::meta::members_of(row.space, std::meta::access_context::unchecked()))
        {
            if (auto const leaf = detail::surfaceLeafOf(member); !leaf.empty())
                names.push_back(prefix + leaf);
            if (detail::isUnscopedEnumeration(member))
                for (auto const enumerator: std::meta::enumerators_of(member))
                    names.push_back(prefix + std::string(std::meta::identifier_of(enumerator)));
        }
    }
    std::ranges::sort(names);
    auto const [last, end] = std::ranges::unique(names);
    names.erase(last, end);
    return names;
}

/// surfaceNamesOf(@p namespaces), as static strings a program can print.
/// @param namespaces The public namespaces.
/// @return The names.
[[nodiscard]] consteval std::span<char const* const> staticSurfaceNamesOf(
    std::span<SurfaceNamespace const> namespaces)
{
    std::vector<char const*> spelled;
    for (auto const& name: surfaceNamesOf(namespaces))
        spelled.push_back(std::define_static_string(name));
    return std::define_static_array(spelled);
}

} // namespace isoline::meta::reflection
