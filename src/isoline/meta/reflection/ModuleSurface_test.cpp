// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/meta/reflection/ModuleSurface.hpp>

// The reflected surface's names, on a fixture namespace holding a member of each kind the export lint lists
// (scripts/testdata/module-exports/list-names/ is the lint's side of the same fixture), and the members that
// name nothing. Every case is a STATIC_REQUIRE: the program meta-module-surface.reflection runs computes its
// names at compile time too.

namespace module_surface_fixture
{

struct Store
{
    friend bool operator==(Store const&, Store const&) = default; // a hidden friend: no name of the namespace
};

template <typename T>
struct Box
{
};

template <>
struct Box<int> // a specialization: the class template is the name
{
};

template <typename T>
concept Boxed = requires { typename T::Boxed; };

using Pages = unsigned;

template <typename T>
using BoxOf = Box<T>;

enum Kind // unscoped: its enumerators are the namespace's names
{
    KindFunction,
};

enum // unnamed and unscoped: only its enumerators are names
{
    AnonymousConstant = 1,
};

enum class Scoped // scoped: its enumerators are not
{
    Hidden,
};

inline constexpr unsigned storeLimit = 1;

Store open(int handle);

template <typename T>
Store open(T handle); // an overload set is one name

bool operator<(Store const&, Store const&);

namespace nested // a namespace is no name; a public one is a row of its own
{
    struct Inner
    {
    };

    enum NestedKind
    {
        NestedValue,
    };

    void someFunction();
} // namespace nested

// An alias of an unscoped enumeration of another namespace: its enumerators are that namespace's, not this
// one's.
using KindAlias = nested::NestedKind;

using nested::Inner;        // a using-declaration is no member at all (UsingDecl, the measured row)
using nested::someFunction; // nor is one of a function

} // namespace module_surface_fixture

namespace
{

using isoline::meta::reflection::surfaceNamesOf;
using isoline::meta::reflection::SurfaceNamespace;

constexpr auto FixtureNamespaces = std::to_array<SurfaceNamespace>({
    { ^^module_surface_fixture, "fixture" },
    { ^^module_surface_fixture::nested, "fixture::nested" },
});

consteval bool namesAre(std::span<SurfaceNamespace const> namespaces,
                        std::vector<std::string_view> const& expected)
{
    auto const names = surfaceNamesOf(namespaces);
    return std::ranges::equal(names, expected);
}

} // namespace

TEST_CASE("ModuleSurface: a namespace's names are those the export lint lists, but its using-declarations",
          "[meta][reflection]")
{
    STATIC_REQUIRE(namesAre(FixtureNamespaces,
                            {
                                "fixture::AnonymousConstant",
                                "fixture::Box",
                                "fixture::BoxOf",
                                "fixture::Boxed",
                                "fixture::Kind",
                                "fixture::KindAlias",
                                "fixture::KindFunction",
                                "fixture::Pages",
                                "fixture::Scoped",
                                "fixture::Store",
                                "fixture::nested::Inner",
                                "fixture::nested::NestedKind",
                                "fixture::nested::NestedValue",
                                "fixture::nested::someFunction",
                                "fixture::open",
                                "fixture::operator<",
                                "fixture::storeLimit",
                            }));
}

TEST_CASE("ModuleSurface: no namespace, no names", "[meta][reflection]")
{
    STATIC_REQUIRE(namesAre({}, {}));
}
