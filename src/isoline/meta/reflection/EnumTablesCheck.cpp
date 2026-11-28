// SPDX-License-Identifier: Apache-2.0
//
// The exhaustive checks of Isoline's enum tables, where reflection takes the native path: every row of a
// registry holds what it claims, every enum-first table discovery finds is a row there, and every namespace
// under the discovery root is one discovery walks. Compile-only: the translation unit defines nothing, and
// nothing links it. It is built once per scope (ISOLINE_ENUM_TABLES_SCOPE, a header naming the root, its
// namespaces and the registry: src/isoline/CMakeLists.txt's isolineEnumTablesChecks), each against an
// umbrella CMake generates of that scope's libraries' header sets and private headers
// (isoline_add_reflection_check), so that no one translation unit reflects over every layer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <isoline/meta/EnumRows.hpp>
#include <isoline/meta/reflection/EnumReflection.hpp>
#include <isoline/meta/reflection/EnumTablesUmbrella.hpp>
#include ISOLINE_ENUM_TABLES_SCOPE

namespace isoline::meta::reflection
{

namespace
{
    /// What a registry row claims, as discovery compares it.
    enum class RowKind : uint8_t
    {
        Indexed,      ///< ISOLINE_ENUM_TABLE.
        Sized,        ///< ISOLINE_SIZED_TABLE.
        Prefix,       ///< ISOLINE_PREFIX_TABLE.
        Keyed,        ///< ISOLINE_KEYED_TABLE.
        LastConstant, ///< ISOLINE_LAST_ENUMERATOR.
    };

    /// One registry row, as discovery compares it; a reflection a row kind has none of is null.
    struct RegistryRow
    {
        RowKind kind;          ///< What the row claims.
        std::meta::info table; ///< The table.
        std::meta::info key;   ///< The member an indexed table is keyed by.
        std::meta::info last;  ///< The Last the row checks.
    };

#define ISOLINE_ENUM_TABLE(Table, Last, member) { RowKind::Indexed, ^^Table, ^^member, ^^Last },
#define ISOLINE_SIZED_TABLE(Table, Last) \
    { RowKind::Sized,                    \
      ^^Table,                           \
      {                                  \
      },                                 \
      ^^Last },
#define ISOLINE_PREFIX_TABLE(Table, Last, reason) \
    { RowKind::Prefix,                            \
      ^^Table,                                    \
      {                                           \
      },                                          \
      ^^Last },
#define ISOLINE_KEYED_TABLE(Table, reason) \
    { RowKind::Keyed,                      \
      ^^Table,                             \
      {                                    \
      },                                   \
      {} },
#define ISOLINE_LAST_ENUMERATOR(Last, reason) { RowKind::LastConstant, {}, {}, ^^Last },
#define ISOLINE_SOURCE_LOCAL_TABLE(file, Table, reason)
    /// The registry's rows, but those of the tables reflection cannot reach.
    constexpr auto RegistryRows = std::to_array<RegistryRow>({
#include ISOLINE_ENUM_TABLES_REGISTRY
    });
#undef ISOLINE_ENUM_TABLE
#undef ISOLINE_SIZED_TABLE
#undef ISOLINE_PREFIX_TABLE
#undef ISOLINE_KEYED_TABLE
#undef ISOLINE_LAST_ENUMERATOR
#undef ISOLINE_SOURCE_LOCAL_TABLE

    /// The reflections @p field holds in the rows @p select accepts.
    consteval std::vector<std::meta::info> fieldOf(std::meta::info RegistryRow::* field,
                                                   bool (*select)(RegistryRow const&))
    {
        auto values = std::vector<std::meta::info> {};
        for (auto const& row: RegistryRows)
            if (select(row))
                values.push_back(row.*field);
        return values;
    }

    /// Whether @p row registers a table.
    consteval bool registersTable(RegistryRow const& row)
    {
        return row.kind != RowKind::LastConstant;
    }

    /// Whether discovery must find @p row's table: a keyed one, or an indexed one keyed by its row's
    /// first member (an index in a later member is not what discovery looks for). A sized or prefix
    /// table has no row struct to discover.
    consteval bool isDiscoverable(RegistryRow const& row)
    {
        return row.kind == RowKind::Keyed || (row.kind == RowKind::Indexed && isFirstMember(row.key));
    }

    /// Whether @p row checks a Last.
    consteval bool checksLast(RegistryRow const& row)
    {
        return row.kind != RowKind::Keyed;
    }

    /// The enum-first tables under the root, found once.
    constexpr auto Discovered = std::define_static_array(discoveredTables(DiscoveryRoot));

    /// The Last constants under the root, found once.
    constexpr auto DiscoveredLasts = std::define_static_array(discoveredLastConstants(DiscoveryRoot));

    /// A registry row's problem, naming the row's table, or empty.
    /// @param table The table's name, as EnumTables.def spells it.
    /// @param problem What is wrong with it, or empty.
    /// @return The message.
    consteval std::string rowProblem(std::string_view table, std::string const& problem)
    {
        if (problem.empty())
            return {};
        return "EnumTables.def: " + std::string { table } + ": " + problem;
    }

    /// The problem of a table with one entry per enumerator 0 through @p Last.
    /// @param last The reflection of Last.
    /// @param position Whether Last must be the final enumerator.
    /// @param size The table's size.
    /// @return Why it is not so, or empty.
    template <auto Last>
    consteval std::string sizedTableProblem(std::meta::info last, LastEnumerator position, std::size_t size)
    {
        using Enum = std::remove_cvref_t<decltype(Last)>;
        if (auto problem = enumeratorsProblem<Enum>(last, position); !problem.empty())
            return problem;
        if (!sizedByEnum<Last>(size))
            return "it holds " + detail::decimal(size) + " entries, not one per enumerator 0 through "
                   + detail::enumeratorNamed<Enum>(EnumeratorCount<Last> - 1);
        return {};
    }

    /// The problem of a table indexed by @p Last's enum.
    template <auto const& Table, auto Last, auto Key>
    consteval std::string indexedRowProblem(std::meta::info last)
    {
        if (auto problem =
                enumeratorsProblem<std::remove_cvref_t<decltype(Last)>>(last, LastEnumerator::Final);
            !problem.empty())
            return problem;
        return indexedTableProblem<Table, Last, Key>();
    }

// One static_assert per row: what the row claims of its table.
#define ISOLINE_ENUM_TABLE(Table, Last, member)                                                \
    static_assert(rowProblem(#Table, indexedRowProblem<Table, Last, &member>(^^Last)).empty(), \
                  rowProblem(#Table, indexedRowProblem<Table, Last, &member>(^^Last)));
#define ISOLINE_SIZED_TABLE(Table, Last)                                                                  \
    static_assert(                                                                                        \
        rowProblem(#Table, sizedTableProblem<Last>(^^Last, LastEnumerator::Final, Table.size())).empty(), \
        rowProblem(#Table, sizedTableProblem<Last>(^^Last, LastEnumerator::Final, Table.size())));
#define ISOLINE_PREFIX_TABLE(Table, Last, reason)                                                          \
    static_assert(                                                                                         \
        rowProblem(#Table, sizedTableProblem<Last>(^^Last, LastEnumerator::Prefix, Table.size())).empty(), \
        rowProblem(#Table, sizedTableProblem<Last>(^^Last, LastEnumerator::Prefix, Table.size())));
#define ISOLINE_KEYED_TABLE(Table, reason)
#define ISOLINE_LAST_ENUMERATOR(Last, reason)                                                              \
    static_assert(                                                                                         \
        rowProblem(#Last,                                                                                  \
                   enumeratorsProblem<std::remove_cvref_t<decltype(Last)>>(^^Last, LastEnumerator::Final)) \
            .empty(),                                                                                      \
        rowProblem(#Last,                                                                                  \
                   enumeratorsProblem<std::remove_cvref_t<decltype(Last)>>(^^Last, LastEnumerator::Final)));
#define ISOLINE_SOURCE_LOCAL_TABLE(file, Table, reason)
#include ISOLINE_ENUM_TABLES_REGISTRY
#undef ISOLINE_ENUM_TABLE
#undef ISOLINE_SIZED_TABLE
#undef ISOLINE_PREFIX_TABLE
#undef ISOLINE_KEYED_TABLE
#undef ISOLINE_LAST_ENUMERATOR
#undef ISOLINE_SOURCE_LOCAL_TABLE

    static_assert(unlistedNames(Discovered, fieldOf(&RegistryRow::table, registersTable)).empty(),
                  "enum-first tables EnumTables.def does not register (add a row): "
                      + unlistedNames(Discovered, fieldOf(&RegistryRow::table, registersTable)));

    static_assert(
        unlistedNames(fieldOf(&RegistryRow::table, isDiscoverable), Discovered).empty(),
        "EnumTables.def rows discovery does not find (a stale row, or a table that no longer starts "
        "with a scoped enum): "
            + unlistedNames(fieldOf(&RegistryRow::table, isDiscoverable), Discovered));

    static_assert(unlistedNames(DiscoveredLasts, fieldOf(&RegistryRow::last, checksLast)).empty(),
                  "Last constants no row of EnumTables.def checks (add the row of its table, or an "
                  "ISOLINE_LAST_ENUMERATOR row where reflection reaches none): "
                      + unlistedNames(DiscoveredLasts, fieldOf(&RegistryRow::last, checksLast)));

    static_assert(unclassifiedNamespaces(DiscoveryRoot, DiscoveredNamespaces).empty(),
                  "namespaces under the root that DiscoveredNamespaces does not list (add a row): "
                      + unlistedNames(unclassifiedNamespaces(DiscoveryRoot, DiscoveredNamespaces), {}));
} // namespace

} // namespace isoline::meta::reflection
