// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <isoline/meta/FixedText.hpp>
#include <isoline/meta/reflection/AnnotatedRows.hpp>

// The rehearsal of annotations as a table's source (P19 Task 7): fixture enums whose enumerators carry
// their rows, derived by reflection and checked against hand tables, as Isoline's tables will be once
// every compiler has annotations (Task 15). Every case is a STATIC_REQUIRE; the fixtures are this
// file's. tests/compile-fail/ holds the enums the derivation refuses to compile.

namespace
{

using isoline::meta::FixedText;
using isoline::meta::reflection::annotatedRows;
using isoline::meta::reflection::annotatedTexts;
using isoline::meta::reflection::annotationProblem;
using isoline::meta::reflection::derivedTableProblem;

/// The rehearsal's error codes, each carrying its message.
enum class RehearsalCode : uint8_t
{
    BadToken[[= FixedText { "bad token" }]],
    Unterminated[[= FixedText { "unterminated string" }]],
};

/// RehearsalCode's messages, by hand, in enumerator order: what the annotations must derive.
constexpr auto RehearsalTexts = std::to_array<std::string_view>({
    "bad token",
    "unterminated string",
});

static_assert(derivedTableProblem<RehearsalCode>(RehearsalTexts).empty(),
              derivedTableProblem<RehearsalCode>(RehearsalTexts));

/// RehearsalCode's messages with the second one wrong.
constexpr auto WrongTexts = std::to_array<std::string_view>({
    "bad token",
    "unterminated",
});

/// RehearsalCode's messages with one too many.
constexpr auto LongTexts = std::to_array<std::string_view>({
    "bad token",
    "unterminated string",
    "extra",
});

// The shape of a real table's row (Task 15's TrapText): a text beside other columns, as a class template
// over the text's length, so that every enumerator's annotation has a type of its own.

/// A column beside a row's text.
enum class Detail : uint8_t
{
    None,
    Fatal,
};

/// An enumerator's row, as an annotation: structural, the text held by value.
template <std::size_t N>
struct TextRow
{
    FixedText<N> text; ///< The row's text.
    Detail detail;     ///< The row's other column.
};

/// The rehearsal's traps, each carrying its row.
enum class RehearsalTrap : uint8_t
{
    OutOfBounds[[= TextRow { FixedText { "out of bounds memory access" }, Detail::Fatal }]],
    Unreachable[[= TextRow { FixedText { "unreachable" }, Detail::None }]],
};

/// RehearsalTrap's row, by hand: the shape of Isoline's enum tables.
struct TrapRow
{
    RehearsalTrap code;    ///< The enumerator the row is for.
    std::string_view text; ///< The row's text.
    Detail detail;         ///< The row's other column.
};

/// Projects a TextRow annotation onto the hand table's row.
constexpr auto ToTrapRow = [](RehearsalTrap code, auto const& row) {
    return TrapRow { .code = code, .text = row.text.view(), .detail = row.detail };
};

/// RehearsalTrap's rows, by hand.
constexpr auto TrapRows = std::to_array<TrapRow>({
    { .code = RehearsalTrap::OutOfBounds, .text = "out of bounds memory access", .detail = Detail::Fatal },
    { .code = RehearsalTrap::Unreachable, .text = "unreachable", .detail = Detail::None },
});

static_assert(derivedTableProblem<RehearsalTrap, ^^TextRow, ToTrapRow>(TrapRows).empty(),
              derivedTableProblem<RehearsalTrap, ^^TextRow, ToTrapRow>(TrapRows));

/// RehearsalTrap's rows, a text column wrong.
constexpr auto WrongTextColumn = std::to_array<TrapRow>({
    { .code = RehearsalTrap::OutOfBounds, .text = "out of bounds memory access", .detail = Detail::Fatal },
    { .code = RehearsalTrap::Unreachable, .text = "unreachable code", .detail = Detail::None },
});

/// RehearsalTrap's rows, a Detail column wrong.
constexpr auto WrongDetailColumn = std::to_array<TrapRow>({
    { .code = RehearsalTrap::OutOfBounds, .text = "out of bounds memory access", .detail = Detail::None },
    { .code = RehearsalTrap::Unreachable, .text = "unreachable", .detail = Detail::None },
});

/// RehearsalTrap's rows, one missing.
constexpr auto ShortTrapRows = std::to_array<TrapRow>({
    { .code = RehearsalTrap::OutOfBounds, .text = "out of bounds memory access", .detail = Detail::Fatal },
});

/// An enum whose second enumerator lacks its text.
enum class Unannotated : uint8_t
{
    Annotated[[= FixedText { "annotated" }]],
    Bare,
};

/// An enum whose second enumerator has two texts.
enum class Doubled : uint8_t
{
    Single[[= FixedText { "single" }]],
    Twice[[= FixedText { "first" }]][[= FixedText { "second" }]],
};

/// A scalar annotation: a row that is not a text.
struct Weight
{
    uint8_t value; ///< The weight.

    /// Rows compare member by member.
    [[nodiscard]] constexpr bool operator==(Weight const&) const noexcept = default;
};

/// An enum carrying a Weight on each enumerator, and a text on one: annotations of other types are not
/// counted.
enum class Weighted : uint8_t
{
    Light[[= Weight { 1 }]][[= FixedText { "light" }]],
    Heavy[[= Weight { 9 }]],
};

/// Weighted's weights, by hand.
constexpr auto Weights = std::to_array<Weight>({ { .value = 1 }, { .value = 9 } });

/// Weighted's weights, the second one wrong.
constexpr auto WrongWeights = std::to_array<Weight>({ { .value = 1 }, { .value = 8 } });

/// An enum whose second enumerator carries an int where a Weight is expected.
enum class Mistyped : uint8_t
{
    Weighed[[= Weight { 1 }]],
    Counted[[= 1]],
};

/// An enum whose enumerators carry ints, a key that is not a class.
enum class Counted : uint8_t
{
    One[[= 1]],
    None,
};

} // namespace

TEST_CASE("AnnotatedRows.textsDeriveTheHandTable")
{
    STATIC_REQUIRE(annotationProblem<RehearsalCode>().empty());
    STATIC_REQUIRE(std::ranges::equal(annotatedTexts<RehearsalCode>, RehearsalTexts));
    STATIC_REQUIRE(derivedTableProblem<RehearsalCode>(WrongTexts)
                   == "row 1 (RehearsalCode::Unterminated): the table has \"unterminated\", its annotation "
                      "\"unterminated string\"");
    STATIC_REQUIRE(derivedTableProblem<RehearsalCode>(LongTexts)
                   == "the table has 3 rows, where RehearsalCode has 2 enumerators");
}

TEST_CASE("AnnotatedRows.rowTemplatesDeriveTheHandTable")
{
    STATIC_REQUIRE(annotationProblem<RehearsalTrap, ^^TextRow>().empty());
    STATIC_REQUIRE(annotatedRows<RehearsalTrap, TrapRow, ^^TextRow, ToTrapRow>[1].text == "unreachable");
    STATIC_REQUIRE(
        derivedTableProblem<RehearsalTrap, ^^TextRow, ToTrapRow>(WrongTextColumn)
        == "row 1 (RehearsalTrap::Unreachable), column text: the table has \"unreachable code\", its "
           "annotation \"unreachable\"");
    STATIC_REQUIRE(derivedTableProblem<RehearsalTrap, ^^TextRow, ToTrapRow>(WrongDetailColumn)
                   == "row 0 (RehearsalTrap::OutOfBounds), column detail: the table has Detail::None, its "
                      "annotation Detail::Fatal");
    STATIC_REQUIRE(derivedTableProblem<RehearsalTrap, ^^TextRow, ToTrapRow>(ShortTrapRows)
                   == "the table has 1 rows, where RehearsalTrap has 2 enumerators");
}

TEST_CASE("AnnotatedRows.missingAnnotationIsNamed")
{
    STATIC_REQUIRE(annotationProblem<Unannotated>()
                   == "Unannotated::Bare has no FixedText annotation, where each enumerator needs one");
    STATIC_REQUIRE(annotationProblem<Weighted>()
                   == "Weighted::Heavy has no FixedText annotation, where each enumerator needs one");
    STATIC_REQUIRE(annotationProblem<Mistyped, ^^Weight>()
                   == "Mistyped::Counted has no Weight annotation, where each enumerator needs one");
    STATIC_REQUIRE(annotationProblem<Counted, ^^int>()
                   == "Counted::None has no int annotation, where each enumerator needs one");
    STATIC_REQUIRE(annotationProblem<RehearsalCode, ^^TextRow>()
                   == "RehearsalCode::BadToken has no TextRow annotation, where each enumerator needs one");
}

TEST_CASE("AnnotatedRows.twoAnnotationsAreNamed")
{
    STATIC_REQUIRE(annotationProblem<Doubled>()
                   == "Doubled::Twice has 2 FixedText annotations, where each enumerator needs exactly one");
}

TEST_CASE("AnnotatedRows.scalarAnnotations")
{
    STATIC_REQUIRE(annotationProblem<Weighted, ^^Weight>().empty());
    STATIC_REQUIRE(std::ranges::equal(annotatedRows<Weighted, Weight, ^^Weight>, Weights));
    STATIC_REQUIRE(derivedTableProblem<Weighted, ^^Weight>(Weights).empty());
    STATIC_REQUIRE(derivedTableProblem<Weighted, ^^Weight>(WrongWeights)
                   == "row 1 (Weighted::Heavy), column value: the table has 8, its annotation 9");
}
