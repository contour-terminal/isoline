// SPDX-License-Identifier: Apache-2.0
#pragma once

// The scope of the core libraries' enum-table check (EnumTablesCheck.cpp, built as
// isoline_enum_tables_check): isoline_core's, isoline_text's and isoline_cli's headers, discovered from
// isoline down, against meta/EnumTables.def. The component model's are another translation unit's
// (ComponentEnumTablesScope.hpp).

#include <array>
#include <meta>

/// The registry the check compares discovery with.
#define ISOLINE_ENUM_TABLES_REGISTRY <isoline/meta/EnumTables.def>

namespace isoline::meta::reflection
{

/// Where discovery starts.
inline constexpr auto DiscoveryRoot = ^^isoline;

/// Every namespace under isoline that discovery walks, at any depth, by name, but those
/// ImplicitNamespaces names (anonymous and detail ones). A namespace added under isoline is a row here, or
/// the unclassified check names it.
inline constexpr auto DiscoveredNamespaces = std::to_array<std::meta::info>({
    ^^isoline,
    ^^isoline::api,
    ^^isoline::binary,
    ^^isoline::binary::detail::proposal,
    ^^isoline::c,
    ^^isoline::cli,
    ^^isoline::config,
    ^^isoline::interpreter,
    ^^isoline::runtime,
    ^^isoline::text,
    ^^isoline::validate,
});

} // namespace isoline::meta::reflection
