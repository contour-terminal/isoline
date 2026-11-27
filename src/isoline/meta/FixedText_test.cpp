// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <type_traits>

#include <isoline/meta/FixedText.hpp>

using isoline::meta::FixedText;

namespace
{

/// A class template whose argument is a text: what FixedText being structural allows, and what an
/// annotation's value needs too.
template <FixedText Value>
struct Tag
{
    static constexpr auto Text = Value; ///< The argument, read back.
};

} // namespace

TEST_CASE("FixedText.viewExcludesTheTerminator")
{
    STATIC_REQUIRE(FixedText { "bad token" }.view() == "bad token");
    STATIC_REQUIRE(FixedText { "bad token" }.view().size() == 9);
    STATIC_REQUIRE(FixedText { "" }.view().empty());
}

TEST_CASE("FixedText.viewsItsTextAtRunTime")
{
    // Not a constant: the copy and the view run as a run-time caller's do.
    auto const text = FixedText { "bad token" };
    CHECK(text.view() == "bad token");
}

TEST_CASE("FixedText.isATemplateArgument")
{
    STATIC_REQUIRE(Tag<"x">::Text.view() == "x");
    // Two arguments are the same when their texts are: the class is one specialization per text.
    STATIC_REQUIRE(std::is_same_v<Tag<"x">, Tag<"x">>);
    STATIC_REQUIRE_FALSE(std::is_same_v<Tag<"x">, Tag<"y">>);
}
