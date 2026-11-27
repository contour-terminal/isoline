// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <meta>
#include <string_view>
#include <vector>

#include <isoline/meta/reflection/EnumReflection.hpp>

// Every case is a STATIC_REQUIRE: what it checks is what the check translation units assert, at
// compile time, of Isoline's own tables. The fixtures are this file's.

namespace
{

using isoline::meta::reflection::discoveredLastConstants;
using isoline::meta::reflection::discoveredTables;
using isoline::meta::reflection::enumeratorsProblem;
using isoline::meta::reflection::indexedTableProblem;
using isoline::meta::reflection::isFirstMember;
using isoline::meta::reflection::LastEnumerator;
using isoline::meta::reflection::unclassifiedNamespaces;
using isoline::meta::reflection::unlistedNames;

/// An enum whose enumerators are 0, 1, 2, with its last named.
enum class Dense : uint8_t
{
    A,
    B,
    C,
};

/// The last Dense enumerator.
constexpr auto LastDense = Dense::C;

/// An enum with an enumerator past the one its Last names: the C++23 checks cannot see Extra.
enum class Trailing : uint8_t
{
    A,
    B,
    Extra,
};

/// The enumerator Trailing's tables end at, which is not its last.
constexpr auto LastTrailing = Trailing::B;

/// An enum whose values skip one.
enum class Gap : uint8_t
{
    A = 0,
    B = 1,
    C = 3,
};

/// The last Gap enumerator.
constexpr auto LastGap = Gap::C;

/// An enum a prefix table covers through B, with C spelled apart.
enum class Prefix : uint8_t
{
    A,
    B,
    C,
};

/// A value of Dense that names no enumerator.
constexpr auto PastDense = static_cast<Dense>(7);

/// One row of a fixture table.
struct DenseRow
{
    Dense code;  ///< The enumerator; equal to the row's index where the table is in place.
    int payload; ///< What the row says.
};

constexpr auto RowsInOrder = std::to_array<DenseRow>({
    { .code = Dense::A, .payload = 10 },
    { .code = Dense::B, .payload = 11 },
    { .code = Dense::C, .payload = 12 },
});

constexpr auto SwappedRows = std::to_array<DenseRow>({
    { .code = Dense::A, .payload = 10 },
    { .code = Dense::C, .payload = 12 },
    { .code = Dense::B, .payload = 11 },
});

constexpr auto MissingLastRow = std::to_array<DenseRow>({
    { .code = Dense::A, .payload = 10 },
    { .code = Dense::B, .payload = 11 },
});

constexpr auto ExtraRow = std::to_array<DenseRow>({
    { .code = Dense::A, .payload = 10 },
    { .code = Dense::B, .payload = 11 },
    { .code = Dense::C, .payload = 12 },
    { .code = static_cast<Dense>(3), .payload = 13 },
});

/// A row whose first member is not a scoped enum: never discovered.
struct UnkeyedRow
{
    int code;    ///< A number, not an enumerator.
    int payload; ///< What the row says.
};

} // namespace

// The discovery fixtures: a root namespace whose children are listed, implicit or unclassified, with a
// listed child that has an implicit, a plain, an anonymous and a nested meta child of its own.
namespace discovery
{

/// Found: a table of the root itself.
inline constexpr auto Root = std::to_array<DenseRow>({ { .code = Dense::A, .payload = 0 } });

/// Found: a Last constant of the root.
inline constexpr auto LastRoot = Dense::C;

namespace listed
{
    /// Found: an enum-first table in a listed namespace.
    inline constexpr auto Listed = std::to_array<DenseRow>({ { .code = Dense::A, .payload = 1 } });
    /// Not found: its row's first member is an int.
    inline constexpr auto Unkeyed = std::to_array<UnkeyedRow>({ { .code = 0, .payload = 1 } });
    /// Not found: no row struct.
    inline constexpr auto Plain = std::to_array<int>({ 1, 2 });
    /// Not a Last constant: an int.
    inline constexpr auto LastCount = 3;
    /// Not a Last constant: not named Last….
    inline constexpr auto FinalDense = Dense::C;

    namespace detail
    {
        /// Found: a detail namespace is walked as part of its parent.
        inline constexpr auto Hidden = std::to_array<DenseRow>({ { .code = Dense::A, .payload = 2 } });
        /// Found: a Last constant in a detail namespace.
        inline constexpr auto LastHidden = Dense::B;
    } // namespace detail

    namespace testing
    {
        /// Not found: a testing namespace, at any depth, holds test data, not tables of the library.
        inline constexpr auto Fixture = std::to_array<DenseRow>({ { .code = Dense::A, .payload = 3 } });
        /// Not found: the same for a Last constant.
        inline constexpr auto LastFixture = Dense::A;
    } // namespace testing

    namespace inner
    {
        /// Found: a plain nested namespace is walked, and unclassified until a list names it.
        inline constexpr auto Inner = std::to_array<DenseRow>({ { .code = Dense::A, .payload = 4 } });

        namespace deeper
        {
            /// Found: the walk goes to every depth.
            inline constexpr auto Deeper = std::to_array<DenseRow>({ { .code = Dense::A, .payload = 10 } });
        } // namespace deeper
    } // namespace inner

    namespace
    {
        /// Found: an anonymous namespace, as a header may hold, is walked as part of its parent.
        inline constexpr auto Anonymous = std::to_array<DenseRow>({ { .code = Dense::A, .payload = 5 } });
    } // namespace

    namespace meta
    {
        /// Found: only the root's meta is skipped; one deeper is a namespace like any other.
        inline constexpr auto NestedMeta = std::to_array<DenseRow>({ { .code = Dense::A, .payload = 6 } });
    } // namespace meta
} // namespace listed

namespace detail
{
} // namespace detail

namespace testing
{
    /// Not found: a testing child of the root.
    inline constexpr auto RootFixture = std::to_array<DenseRow>({ { .code = Dense::A, .payload = 7 } });
} // namespace testing

namespace meta
{
    /// Not found: the root's meta holds the checks.
    inline constexpr auto Machinery = std::to_array<DenseRow>({ { .code = Dense::A, .payload = 8 } });

    namespace below
    {
        /// Not found: nor what is under it.
        inline constexpr auto BelowMachinery =
            std::to_array<DenseRow>({ { .code = Dense::A, .payload = 9 } });
    } // namespace below
} // namespace meta

namespace extra
{
} // namespace extra

} // namespace discovery

namespace
{

constexpr auto ListedNamespaces = std::to_array<std::meta::info>({ ^^discovery::listed });

} // namespace

TEST_CASE("EnumReflection.denseEnumeratorsPass")
{
    STATIC_REQUIRE(enumeratorsProblem<Dense>(^^LastDense, LastEnumerator::Final).empty());
    STATIC_REQUIRE(enumeratorsProblem<Dense>(^^Dense::C, LastEnumerator::Final).empty());
}

TEST_CASE("EnumReflection.lastThatIsNotLastIsNamed")
{
    STATIC_REQUIRE(
        enumeratorsProblem<Trailing>(^^LastTrailing, LastEnumerator::Final)
        == "Trailing::Extra follows LastTrailing (Trailing::B), which must be Trailing's final enumerator");
    STATIC_REQUIRE(enumeratorsProblem<Dense>(^^PastDense, LastEnumerator::Final)
                   == "PastDense is 7, which names no enumerator of Dense");
}

TEST_CASE("EnumReflection.gapIsNamed")
{
    STATIC_REQUIRE(enumeratorsProblem<Gap>(^^LastGap, LastEnumerator::Final)
                   == "Gap::C is 3, not 2: Gap's enumerators must be 0, 1, 2 and on, in declaration order");
    STATIC_REQUIRE(enumeratorsProblem<Gap>(^^LastGap, LastEnumerator::Prefix)
                   == "Gap::C is 3, not 2: Gap's enumerators must be 0, 1, 2 and on, in declaration order");
}

TEST_CASE("EnumReflection.prefixMayStopEarly")
{
    STATIC_REQUIRE(enumeratorsProblem<Prefix>(^^Prefix::B, LastEnumerator::Prefix).empty());
    STATIC_REQUIRE(enumeratorsProblem<Trailing>(^^LastTrailing, LastEnumerator::Prefix).empty());
    // A gap past the prefix is none of its business; one before its end is.
    STATIC_REQUIRE(enumeratorsProblem<Gap>(^^Gap::B, LastEnumerator::Prefix).empty());
    STATIC_REQUIRE(enumeratorsProblem<Prefix>(^^Prefix::B, LastEnumerator::Final)
                   == "Prefix::C follows Prefix::B, which must be Prefix's final enumerator");
}

TEST_CASE("EnumReflection.misplacedRowIsNamed")
{
    STATIC_REQUIRE(indexedTableProblem<RowsInOrder, LastDense, &DenseRow::code>().empty());
    STATIC_REQUIRE(indexedTableProblem<SwappedRows, LastDense, &DenseRow::code>()
                   == "row 1 names Dense::C, where Dense::B belongs");
    STATIC_REQUIRE(indexedTableProblem<MissingLastRow, LastDense, &DenseRow::code>()
                   == "it has no row for Dense::C, its row 2");
    STATIC_REQUIRE(
        indexedTableProblem<ExtraRow, LastDense, &DenseRow::code>()
        == "row 3, which names static_cast<Dense>(3), is past Dense::C, the last row's enumerator");
}

TEST_CASE("EnumReflection.discoversTablesInNestedNamespaces")
{
    STATIC_REQUIRE(discoveredTables(^^discovery)
                   == std::vector { ^^discovery::Root,
                                    ^^discovery::listed::Listed,
                                    ^^discovery::listed::detail::Hidden,
                                    ^^discovery::listed::inner::Inner,
                                    ^^discovery::listed::inner::deeper::Deeper,
                                    ^^discovery::listed::Anonymous,
                                    ^^discovery::listed::meta::NestedMeta });
}

TEST_CASE("EnumReflection.discoversLastConstants")
{
    STATIC_REQUIRE(discoveredLastConstants(^^discovery)
                   == std::vector { ^^discovery::LastRoot, ^^discovery::listed::detail::LastHidden });
}

TEST_CASE("EnumReflection.unclassifiedNamespaceIsNamed")
{
    // At any depth: listed::inner and listed::meta (meta is implicit only under the root) are walked,
    // so a list must name them; detail and anonymous namespaces are their parents', and what testing
    // or the root's meta holds is not walked at all.
    STATIC_REQUIRE(unclassifiedNamespaces(^^discovery, ListedNamespaces)
                   == std::vector { ^^discovery::listed::inner,
                                    ^^discovery::listed::inner::deeper,
                                    ^^discovery::listed::meta,
                                    ^^discovery::extra });
    constexpr auto all = std::to_array<std::meta::info>({ ^^discovery::listed,
                                                          ^^discovery::listed::inner,
                                                          ^^discovery::listed::inner::deeper,
                                                          ^^discovery::listed::meta,
                                                          ^^discovery::extra });
    STATIC_REQUIRE(unclassifiedNamespaces(^^discovery, all).empty());
}

TEST_CASE("EnumReflection.unlistedTableIsNamed")
{
    constexpr auto registered = std::to_array<std::meta::info>({ ^^discovery::Root,
                                                                 ^^discovery::listed::Listed,
                                                                 ^^discovery::listed::inner::Inner,
                                                                 ^^discovery::listed::inner::deeper::Deeper,
                                                                 ^^discovery::listed::Anonymous,
                                                                 ^^discovery::listed::meta::NestedMeta });
    STATIC_REQUIRE(unlistedNames(discoveredTables(^^discovery), registered)
                   == "discovery::listed::detail::Hidden");
    // A stale row: registered, but not an enum-first table.
    constexpr auto stale =
        std::to_array<std::meta::info>({ ^^discovery::listed::Listed, ^^discovery::listed::Unkeyed });
    STATIC_REQUIRE(unlistedNames(stale, discoveredTables(^^discovery)) == "discovery::listed::Unkeyed");
    STATIC_REQUIRE(unlistedNames(registered, discoveredTables(^^discovery)).empty());
}

TEST_CASE("EnumReflection.firstMemberIsTheKeyDiscoverySees")
{
    STATIC_REQUIRE(isFirstMember(^^DenseRow::code));
    STATIC_REQUIRE_FALSE(isFirstMember(^^DenseRow::payload));
}
