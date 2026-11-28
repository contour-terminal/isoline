// SPDX-License-Identifier: Apache-2.0
#pragma once

// The scope of the component model's enum-table check (EnumTablesCheck.cpp, built as
// isoline_component_enum_tables_check): isoline_component's and isoline_component_text's headers,
// discovered from isoline::component down, against component/EnumTables.def. A translation unit of its
// own, so that neither check reflects over both layers at once (each fits a CI runner's memory).

#include <array>
#include <meta>

/// The registry the check compares discovery with.
#define ISOLINE_ENUM_TABLES_REGISTRY <isoline/component/EnumTables.def>

namespace isoline::meta::reflection
{

/// Where discovery starts.
inline constexpr auto DiscoveryRoot = ^^isoline::component;

/// Every namespace under isoline::component that discovery walks, at any depth, by name, but those
/// ImplicitNamespaces names (anonymous, detail and testing ones). A namespace added there is a row here,
/// or the unclassified check names it.
inline constexpr auto DiscoveredNamespaces = std::to_array<std::meta::info>({
    ^^isoline::component,
    ^^isoline::component::api,
    ^^isoline::component::api::typed,
    ^^isoline::component::binary,
    ^^isoline::component::canonical,
    ^^isoline::component::runtime,
    ^^isoline::component::text,
    ^^isoline::component::types,
    ^^isoline::component::validate,
});

} // namespace isoline::meta::reflection
