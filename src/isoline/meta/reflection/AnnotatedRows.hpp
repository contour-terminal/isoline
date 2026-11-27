// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Utils.hpp>

#include <array>
#include <cstddef>
#include <meta>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <isoline/meta/FixedText.hpp>
#include <isoline/meta/reflection/EnumReflection.hpp>

// C++26 annotations, rehearsed: the rows of a table derived from its enumerators' annotations, where
// each enumerator carries its own row (`OutOfBounds [[=TrapText { FixedText { "out of bounds" }, … }]]`).
// Only compiled where annotations take the native path (cmake/IsolineLanguageFeatures.cmake), and never
// installed. Until every compiler has annotations (P19 Task 15) the hand tables stay the source: a
// derived table only verifies one, and derivedTableProblem() names the first row and column where they
// differ.
//
// An annotation is looked up by a key: a type's reflection (^^Weight) finds the annotations of exactly
// that type, a class template's (^^TextRow) those of any of its specializations, so that a row holding a
// FixedText<N> is found whatever its N. A projection turns each annotation into the hand table's row
// type, so the two compare column by column.

namespace isoline::meta::reflection
{

/// The projection annotatedRows uses when none is given: a FixedText's text, any other annotation as it
/// is.
struct AnnotationAsRow
{
    /// @param annotation The annotation's value.
    /// @return Its text when it is a FixedText<N>, else @p annotation.
    template <typename Enum, typename Annotation>
    [[nodiscard]] constexpr auto operator()(Enum /*enumerator*/, Annotation const& annotation) const noexcept
    {
        if constexpr (std::meta::has_template_arguments(^^Annotation)
                      && std::meta::template_of(^^Annotation) == ^^isoline::meta::FixedText)
            return annotation.view();
        else
            return annotation;
    }
};

namespace detail
{
    /// Whether @p annotation is one @p key finds: of exactly the type @p key reflects, or of a
    /// specialization of the class template it reflects.
    /// @param annotation An annotation's reflection.
    /// @param key A type's or a class template's reflection.
    [[nodiscard]] consteval bool isKeyedBy(std::meta::info annotation, std::meta::info key)
    {
        auto const type = std::meta::remove_cv(std::meta::type_of(annotation));
        if (std::meta::is_template(key))
            return std::meta::has_template_arguments(type) && std::meta::template_of(type) == key;
        return type == std::meta::dealias(key);
    }

    /// The annotations of @p enumerator that @p key finds, in declaration order.
    [[nodiscard]] consteval std::vector<std::meta::info> annotationsKeyedBy(std::meta::info enumerator,
                                                                            std::meta::info key)
    {
        auto found = std::meta::annotations_of(enumerator);
        std::erase_if(found, [key](std::meta::info annotation) { return !isKeyedBy(annotation, key); });
        return found;
    }

    /// The name a message gives @p entity: its identifier (a class, a class template), else how it is
    /// spelled (`int`, a specialization).
    [[nodiscard]] consteval std::string nameOf(std::meta::info entity)
    {
        return std::string { std::meta::has_identifier(entity) ? std::meta::identifier_of(entity)
                                                               : std::meta::display_string_of(entity) };
    }

    /// The enumerator @p Enum declares at @p index, spelled `Enum::Name`.
    template <typename Enum>
    [[nodiscard]] consteval std::string enumeratorAt(std::size_t index)
    {
        return nameOf(^^Enum) + "::" + nameOf(std::meta::enumerators_of(^^Enum).at(index));
    }

    /// @p value spelled for a message: a text quoted, an enumerator by name, an unsigned number in
    /// decimal; empty for a value of another type.
    template <typename Value>
    [[nodiscard]] consteval std::string spelled(Value const& value)
    {
        if constexpr (std::is_same_v<Value, std::string_view>)
            return "\"" + std::string { value } + "\"";
        else if constexpr (std::is_enum_v<Value>)
            return enumeratorNamed<Value>(static_cast<std::size_t>(std::to_underlying(value)));
        else if constexpr (std::is_unsigned_v<Value>)
            return decimal(value);
        else
            return {};
    }

    /// Why @p table's value differs from @p derived's, the two being equal: empty.
    /// @param where The row (and column) the values are in.
    template <typename Value>
    [[nodiscard]] consteval std::string valueProblem(std::string const& where,
                                                     Value const& table,
                                                     Value const& derived)
    {
        if (table == derived)
            return {};
        auto const tableSpelled = spelled(table);
        if (tableSpelled.empty())
            return where + " differs from its annotation";
        return where + ": the table has " + tableSpelled + ", its annotation " + spelled(derived);
    }
} // namespace detail

/// Why some enumerator of @p Enum lacks exactly one annotation that @p Annotation finds (a type's
/// reflection, or a class template's for any of its specializations); empty when none does.
/// @return The problem, naming the first enumerator at fault and how many it has.
template <typename Enum, std::meta::info Annotation = ^^isoline::meta::FixedText>
    requires std::is_enum_v<Enum>
[[nodiscard]] consteval std::string annotationProblem()
{
    auto const name = detail::nameOf(Annotation);
    for (auto const enumerator: std::meta::enumerators_of(^^Enum))
    {
        auto const count = detail::annotationsKeyedBy(enumerator, Annotation).size();
        if (count == 1)
            continue;
        auto const spelled = detail::nameOf(^^Enum) + "::" + detail::nameOf(enumerator);
        if (count == 0)
            return spelled + " has no " + name + " annotation, where each enumerator needs one";
        return spelled + " has " + detail::decimal(count) + " " + name
               + " annotations, where each enumerator needs exactly one";
    }
    return {};
}

namespace detail
{
    /// Each enumerator's one annotation that @p Annotation finds, projected onto @p Row, in declaration
    /// order. An enumerator without exactly one fails to compile, naming it.
    /// @return The rows; a text views the annotation's value, which has static storage.
    template <typename Enum, typename Row, std::meta::info Annotation, auto Project>
    [[nodiscard]] consteval auto rowsOf()
    {
        static_assert(annotationProblem<Enum, Annotation>().empty(), annotationProblem<Enum, Annotation>());
        auto rows = std::array<Row, std::meta::enumerators_of(^^Enum).size()> {};
        auto index = std::size_t { 0 };
        // The annotations' types may differ (a class template's specializations), so each is spliced on
        // its own.
        template for (constexpr auto enumerator: std::define_static_array(std::meta::enumerators_of(^^Enum)))
        {
            template for (constexpr auto annotation:
                          std::define_static_array(annotationsKeyedBy(enumerator, Annotation)))
                rows.at(index) = Row(Project([:enumerator:], [:std::meta::constant_of(annotation):]));
            ++index;
        }
        return rows;
    }

    /// The storage annotatedRows views.
    template <typename Enum, typename Row, std::meta::info Annotation, auto Project>
    inline constexpr auto AnnotatedRowArray = rowsOf<Enum, Row, Annotation, Project>();
} // namespace detail

/// Each enumerator's one annotation that @p Annotation finds (a type's reflection, or a class template's
/// for any of its specializations), projected onto @p Row by @p Project, in declaration order.
/// @p Project is called as `Project(enumerator, annotation)`; the default gives a FixedText's text and any
/// other annotation as it is. @p Row is default-constructible.
template <typename Enum,
          typename Row,
          std::meta::info Annotation = ^^isoline::meta::FixedText,
          auto Project =
              AnnotationAsRow {
              }>
    requires std::is_enum_v<Enum> && std::is_default_constructible_v<Row>
inline constexpr std::span<Row const> annotatedRows =
    detail::AnnotatedRowArray<Enum, Row, Annotation, Project>;

/// Each enumerator's one FixedText<N> annotation (any N), as text, in declaration order.
template <typename Enum>
    requires std::is_enum_v<Enum>
inline constexpr std::span<std::string_view const> annotatedTexts = annotatedRows<Enum, std::string_view>;

/// Why @p table, a hand table of one row per enumerator of @p Enum in declaration order, differs from
/// the rows @p Enum's annotations derive (annotatedRows<Enum, Row, Annotation, Project>).
/// @param table The hand table.
/// @return The problem: the row counts when they differ; else the first differing row, named by its
///         index and enumerator, and for a row struct its first differing column, with both values where
///         they can be spelled (a text, an enumerator, an unsigned number). Empty when the two are equal.
template <typename Enum,
          std::meta::info Annotation = ^^isoline::meta::FixedText,
          auto Project =
              AnnotationAsRow {
              },
          typename Row,
          std::size_t Count>
    requires std::is_enum_v<Enum>
[[nodiscard]] consteval std::string derivedTableProblem(std::array<Row, Count> const& table)
{
    constexpr auto enumeratorCount = std::meta::enumerators_of(^^Enum).size();
    if constexpr (Count != enumeratorCount)
        return "the table has " + detail::decimal(Count) + " rows, where " + detail::nameOf(^^Enum) + " has "
               + detail::decimal(enumeratorCount) + " enumerators";
    else
    {
        auto const derived = annotatedRows<Enum, Row, Annotation, Project>;
        for (auto const& [index, row]: core::views::enumerate(table))
        {
            auto const where =
                "row " + detail::decimal(index) + " (" + detail::enumeratorAt<Enum>(index) + ")";
            if constexpr (std::is_aggregate_v<Row>)
            {
                template for (constexpr auto column:
                              std::define_static_array(std::meta::nonstatic_data_members_of(
                                  ^^Row, std::meta::access_context::unchecked())))
                {
                    auto const& tableValue = row.[:column:];
                    auto const& derivedValue = derived[index].[:column:];
                    auto const problem = detail::valueProblem(
                        where + ", column " + detail::nameOf(column), tableValue, derivedValue);
                    if (!problem.empty())
                        return problem;
                }
            }
            else
            {
                auto const problem = detail::valueProblem(where, row, derived[index]);
                if (!problem.empty())
                    return problem;
            }
        }
        return {};
    }
}

} // namespace isoline::meta::reflection
