// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <meta>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <isoline/meta/reflection/EnumReflection.hpp>

// C++26 reflection checks of the C API's rows, which add what C++23 cannot see of a C header: that every
// constant of a C enumeration a row table mirrors has its row (a C enumeration has no Last to count to), and
// that the field rows of layout.def name every member of their struct, in order, each of its size. The C
// headers are read as C++ here, in the check translation unit (CApiCheck.cpp). Only compiled where reflection
// takes the native path (cmake/IsolineLanguageFeatures.cmake), and never installed. Reflection only verifies:
// the rows stay the source.

namespace isoline::meta::reflection
{

/// One ISOLINE_LAYOUT_FIELD row of layout.def, as layoutProblem() reads it.
struct LayoutFieldRow
{
    std::meta::info type;   ///< The struct the row is a field of.
    std::string_view field; ///< The member it names.
};

/// One ISOLINE_LAYOUT_FIELD_TYPE or ISOLINE_LAYOUT_UNION_MEMBER_TYPE row of layout.def, as layoutProblem()
/// reads it.
struct LayoutTypeRow
{
    std::meta::info type;      ///< The struct.
    std::string_view path;     ///< The member: `field`, or `field.member` for a member of a field's union.
    std::meta::info fieldType; ///< Its type, as the row spells it.
};

namespace detail
{
    /// One constant of a mirrored enumeration.
    struct MirroredConstant
    {
        std::meta::info constant; ///< The enumerator.
        int64_t value;            ///< Its value.
        std::string name;         ///< `Enum::NAME`.
    };

    /// @p value as a number, an enumerator by its underlying value.
    /// @param value A row's column, or an enumerator.
    /// @return Its value.
    template <typename T>
        requires std::is_integral_v<T> || std::is_enum_v<T>
    [[nodiscard]] consteval int64_t integerOf(T value)
    {
        if constexpr (std::is_enum_v<T>)
            return static_cast<int64_t>(std::to_underlying(value));
        else
            return static_cast<int64_t>(value);
    }

    /// @p value in decimal, with its sign.
    /// @param value The number.
    /// @return Its digits.
    [[nodiscard]] consteval std::string signedDecimal(int64_t value)
    {
        if (value < 0)
            return "-" + decimal(static_cast<std::size_t>(-(value + 1)) + 1);
        return decimal(static_cast<std::size_t>(value));
    }

    /// The name of @p entity: its identifier, or how the compiler displays it when it has none.
    /// @param entity A reflection.
    /// @return Its name.
    [[nodiscard]] consteval std::string nameOf(std::meta::info entity)
    {
        if (std::meta::has_identifier(entity))
            return std::string { std::meta::identifier_of(entity) };
        return std::string { std::meta::display_string_of(entity) };
    }

    /// Appends the enumerators of @p Enum to @p constants, in declaration order.
    /// @param constants Where they go.
    template <typename Enum>
    consteval void appendConstants(std::vector<MirroredConstant>& constants)
    {
        auto const enumName = nameOf(^^Enum);
        for (auto const enumerator: std::meta::enumerators_of(^^Enum))
            constants.push_back({ .constant = enumerator,
                                  .value = integerOf(std::meta::extract<Enum>(enumerator)),
                                  .name = enumName + "::" + nameOf(enumerator) });
    }

    /// @p indices as prose: `1`, `1 and 2`, `1, 2 and 3`.
    /// @param indices Row indices, at least one.
    /// @return Their list.
    [[nodiscard]] consteval std::string listed(std::span<std::size_t const> indices)
    {
        auto text = std::string {};
        for (auto const& [position, index]: core::views::enumerate(indices))
        {
            if (position > 0)
                text += position + 1 == indices.size() ? " and " : ", ";
            text += decimal(index);
        }
        return text;
    }
} // namespace detail

/// Why the column @p Column of @p Rows does not mirror the enumerations @p Enums: every constant of theirs is
/// in exactly one row, but those @p without names, which are in none; and every row holds one of their
/// constants. A C table's C column mirrors the C enumerations (one or more: Isoline's constants beside
/// wasm.h's), its C++ column the C++ enumeration. Two constants of one value are a problem unless one of them
/// is listed in @p aliases (a Last constant), which is then in its constant's row.
/// It checks which constants the rows hold, not how a row pairs them with its other columns: two rows that
/// swap their constants pass, and a test that spells the pairs out pins them.
/// @p Column is a member pointer, or a projection a row is called with (the value a row reports, as an
/// error's detail is its enumerator plus one).
/// @param rowsName The table's name, for the message.
/// @param without The constants no row holds, each deliberately: a "none" or "invalid" value, or an
///        enumerator the C API has no form of.
/// @param aliases The constants that are another constant's value under a second name.
/// @return The first problem, naming the constant or the row; empty when there is none.
template <auto const& Rows, auto Column, typename... Enums>
    requires(sizeof...(Enums) > 0 && (std::is_enum_v<Enums> && ...))
[[nodiscard]] consteval std::string mirrorProblem(std::string_view rowsName,
                                                  std::span<std::meta::info const> without,
                                                  std::span<std::meta::info const> aliases)
{
    auto constants = std::vector<detail::MirroredConstant> {};
    (detail::appendConstants<Enums>(constants), ...);
    auto enumNames = std::string {};
    ((enumNames += (enumNames.empty() ? "" : ", ") + detail::nameOf(^^Enums)), ...);
    auto const prefix = std::string { rowsName } + ": ";
    auto const isConstant = [&](std::meta::info entity) {
        return std::ranges::any_of(constants,
                                   [entity](auto const& constant) { return constant.constant == entity; });
    };
    auto const isAlias = [aliases](std::meta::info entity) {
        return std::ranges::find(aliases, entity) != aliases.end();
    };
    auto const valueOf = [](auto const& row) {
        return detail::integerOf(std::invoke(Column, row));
    };

    for (auto const entry: without)
        if (!isConstant(entry))
            return prefix + detail::nameOf(entry) + " is listed as without a row, but is no constant of "
                   + enumNames;
    for (auto const entry: aliases)
        if (!isConstant(entry))
            return prefix + detail::nameOf(entry) + " is listed as an alias, but is no constant of "
                   + enumNames;

    for (auto const& [position, constant]: core::views::enumerate(constants))
    {
        if (isAlias(constant.constant))
        {
            auto const aliased = std::ranges::any_of(constants, [&](auto const& other) {
                return other.value == constant.value && !isAlias(other.constant);
            });
            if (!aliased)
                return prefix + constant.name + " is listed as an alias, but no other constant is "
                       + detail::signedDecimal(constant.value);
            continue;
        }
        for (auto const& other: std::span { constants }.subspan(static_cast<std::size_t>(position) + 1))
            if (other.value == constant.value && !isAlias(other.constant))
                return prefix + constant.name + " and " + other.name + " are both "
                       + detail::signedDecimal(constant.value)
                       + ": list one as an alias, with the reason, or give it a value of its own";
    }

    for (auto const& constant: constants)
    {
        if (isAlias(constant.constant))
            continue;
        auto holders = std::vector<std::size_t> {};
        auto index = std::size_t { 0 };
        for (auto const& row: Rows)
        {
            if (valueOf(row) == constant.value)
                holders.push_back(index);
            ++index;
        }
        if (std::ranges::find(without, constant.constant) != without.end())
        {
            if (!holders.empty())
                return prefix + constant.name + " is listed as without a row, but row "
                       + detail::decimal(holders.front()) + " holds it";
            continue;
        }
        if (holders.empty())
            return prefix + constant.name + " (" + detail::signedDecimal(constant.value)
                   + ") has no row: add one, or list it as without one, with the reason";
        if (holders.size() > 1)
            return prefix + constant.name + " (" + detail::signedDecimal(constant.value) + ") is in "
                   + detail::decimal(holders.size()) + " rows, rows " + detail::listed(holders)
                   + ": one row each";
    }

    auto index = std::size_t { 0 };
    for (auto const& row: Rows)
    {
        auto const value = valueOf(row);
        if (std::ranges::none_of(constants,
                                 [value](auto const& constant) { return constant.value == value; }))
            return prefix + "row " + detail::decimal(index) + " holds " + detail::signedDecimal(value)
                   + ", which no constant of " + enumNames + " is";
        ++index;
    }
    return {};
}

/// One side of a mirror, as cEnumsProblem() reads it: the enumerations an ISOLINE_C_MIRROR row checks.
struct CMirrorSide
{
    std::string_view rows;                  ///< The table's name.
    std::span<std::meta::info const> enums; ///< The enumerations whose constants its column holds.
};

/// The side of a mirror of the table @p rows over @p Enums.
/// @param rows The table's name.
/// @return The side.
template <typename... Enums>
[[nodiscard]] consteval CMirrorSide cMirrorSide(std::string_view rows)
{
    return { .rows = rows,
             .enums =
                 std::define_static_array(std::array<std::meta::info, sizeof...(Enums)> { ^^Enums... }) };
}

/// One C enumeration no mirror checks, deliberately.
struct UnmirroredEnumRow
{
    std::meta::info enumeration; ///< The enumeration.
    std::string_view reason;     ///< Why no table converts it.
};

/// Why the C enumerations of the namespace @p scope (the global one, for the C headers) whose names begin
/// with one of @p prefixes (the C API's: wasm.h's and Isoline's) are not each either an enumeration a
/// CMirrorSide variable of the namespace @p Mirrors names, or a row of @p unmirrored; a row of @p unmirrored
/// that names none of them, or one a mirror checks, is a problem too.
/// @param scope The namespace the C enumerations are declared in.
/// @param unmirrored The enumerations no table converts, each with its reason.
/// @param prefixes The C API's prefixes.
/// @return The first problem, naming the enumeration; empty when there is none.
template <std::meta::info Mirrors>
[[nodiscard]] consteval std::string cEnumsProblem(std::meta::info scope,
                                                  std::span<UnmirroredEnumRow const> unmirrored,
                                                  std::span<std::string_view const> prefixes)
{
    auto mirrored = std::vector<std::meta::info> {};
    template for (constexpr auto member: std::define_static_array(
                      std::meta::members_of(Mirrors, std::meta::access_context::unchecked())))
    {
        if constexpr (std::meta::is_variable(member)
                      && std::meta::remove_cv(std::meta::type_of(member)) == ^^CMirrorSide)
            for (auto const enumeration: [:member:].enums)
                mirrored.push_back(enumeration);
    }
    auto const isUnmirrored = [unmirrored](std::meta::info enumeration) {
        return std::ranges::any_of(unmirrored,
                                   [enumeration](auto const& row) { return row.enumeration == enumeration; });
    };
    auto const isMirrored = [&](std::meta::info enumeration) {
        return std::ranges::find(mirrored, enumeration) != mirrored.end();
    };
    auto found = std::vector<std::meta::info> {};
    for (auto const member: std::meta::members_of(scope, std::meta::access_context::unchecked()))
    {
        if (!std::meta::is_type(member) || !std::meta::is_enum_type(member)
            || std::meta::is_scoped_enum_type(member) || !std::meta::has_identifier(member))
            continue;
        auto const name = std::meta::identifier_of(member);
        if (std::ranges::none_of(prefixes,
                                 [name](std::string_view prefix) { return name.starts_with(prefix); }))
            continue;
        found.push_back(member);
        if (!isMirrored(member) && !isUnmirrored(member))
            return std::string { name }
                   + " is a C enumeration no ISOLINE_C_MIRROR row checks: add the row of the table that "
                     "converts "
                     "it, or list it as unmirrored, with the reason";
        if (isMirrored(member) && isUnmirrored(member))
            return std::string { name } + " is listed as unmirrored, but an ISOLINE_C_MIRROR row checks it";
    }
    for (auto const& row: unmirrored)
        if (std::ranges::find(found, row.enumeration) == found.end())
            return detail::nameOf(row.enumeration)
                   + " is listed as unmirrored, but is no C enumeration of the C API's headers";
    return {};
}

namespace detail
{
    /// The members of @p type a type row is owed to, in declaration order, each with its type: a member
    /// itself, or, where its type is an unnamed union, each of the union's members as `field.member`.
    /// @param type A struct.
    /// @return The paths and types.
    [[nodiscard]] consteval std::vector<std::pair<std::string, std::meta::info>> typedMembersOf(
        std::meta::info type)
    {
        auto typed = std::vector<std::pair<std::string, std::meta::info>> {};
        auto const unchecked = std::meta::access_context::unchecked();
        for (auto const member: std::meta::nonstatic_data_members_of(std::meta::dealias(type), unchecked))
        {
            auto const memberType = std::meta::type_of(member);
            if (std::meta::is_union_type(memberType) && !std::meta::has_identifier(memberType))
                for (auto const alternative: std::meta::nonstatic_data_members_of(memberType, unchecked))
                    typed.emplace_back(nameOf(member) + "." + nameOf(alternative),
                                       std::meta::type_of(alternative));
            else
                typed.emplace_back(nameOf(member), memberType);
        }
        return typed;
    }
} // namespace detail

/// Why the field rows @p fields do not name every member of the structs @p structs, in declaration order, one
/// row each, and the type rows @p types every member's type (a union's members each, for a field of an
/// unnamed union type), in order, as type_of() sees it; a row of a struct @p structs does not hold is a
/// problem too.
/// @param structs The structs layout.def has an ISOLINE_LAYOUT_STRUCT row of.
/// @param fields Its ISOLINE_LAYOUT_FIELD rows, in order.
/// @param types Its ISOLINE_LAYOUT_FIELD_TYPE and ISOLINE_LAYOUT_UNION_MEMBER_TYPE rows, in order.
/// @return The first problem, naming the struct and the member or row; empty when there is none.
[[nodiscard]] consteval std::string layoutProblem(std::span<std::meta::info const> structs,
                                                  std::span<LayoutFieldRow const> fields,
                                                  std::span<LayoutTypeRow const> types)
{
    auto const sameType = [](std::meta::info a, std::meta::info b) {
        return std::meta::dealias(a) == std::meta::dealias(b);
    };
    for (auto const& row: fields)
        if (std::ranges::none_of(structs, [&](std::meta::info type) { return sameType(type, row.type); }))
            return detail::nameOf(row.type)
                   + " has ISOLINE_LAYOUT_FIELD rows but no ISOLINE_LAYOUT_STRUCT row";
    for (auto const& row: types)
        if (std::ranges::none_of(structs, [&](std::meta::info type) { return sameType(type, row.type); }))
            return detail::nameOf(row.type) + " has type rows but no ISOLINE_LAYOUT_STRUCT row";

    for (auto const type: structs)
    {
        auto const typeName = detail::nameOf(type);
        auto const members = std::meta::nonstatic_data_members_of(std::meta::dealias(type),
                                                                  std::meta::access_context::unchecked());
        auto named = std::vector<LayoutFieldRow> {};
        for (auto const& row: fields)
            if (sameType(row.type, type))
                named.push_back(row);
        for (auto const index: std::views::iota(std::size_t { 0 }, std::max(members.size(), named.size())))
        {
            if (index >= named.size())
                return typeName + ": member " + detail::nameOf(members[index])
                       + " has no ISOLINE_LAYOUT_FIELD row";
            if (index >= members.size())
                return typeName + ": field row " + detail::decimal(index) + " names "
                       + std::string { named[index].field } + ", but the struct has "
                       + detail::decimal(members.size()) + " members";
            if (detail::nameOf(members[index]) != named[index].field)
                return typeName + ": field row " + detail::decimal(index) + " names "
                       + std::string { named[index].field } + ", where member "
                       + detail::nameOf(members[index]) + " is: one row per member, in declaration order";
        }
        auto typedRows = std::vector<LayoutTypeRow> {};
        for (auto const& row: types)
            if (sameType(row.type, type))
                typedRows.push_back(row);
        auto const typed = detail::typedMembersOf(type);
        for (auto const index: std::views::iota(std::size_t { 0 }, std::max(typed.size(), typedRows.size())))
        {
            if (index >= typedRows.size())
                return typeName + ": member " + typed[index].first + " has no type row";
            if (index >= typed.size())
                return typeName + ": type row " + detail::decimal(index) + " names "
                       + std::string { typedRows[index].path } + ", which is no member";
            auto const& [path, memberType] = typed[index];
            if (path != typedRows[index].path)
                return typeName + ": type row " + detail::decimal(index) + " names "
                       + std::string { typedRows[index].path } + ", where member " + path
                       + " is: one row per member, in declaration order";
            if (std::meta::dealias(memberType) != std::meta::dealias(typedRows[index].fieldType))
                return typeName + ": member " + path + " is of "
                       + std::string { std::meta::display_string_of(std::meta::dealias(memberType)) }
                       + ", but its type row says "
                       + std::string { std::meta::display_string_of(
                           std::meta::dealias(typedRows[index].fieldType)) };
        }
    }
    return {};
}

} // namespace isoline::meta::reflection

#define ISOLINE_C_MIRROR_CONCAT_(a, b) a##b
#define ISOLINE_C_MIRROR_CONCAT(a, b)  ISOLINE_C_MIRROR_CONCAT_(a, b)

/// Checks one side of a C mirror (mirrorProblem()): that the column @p column of the table @p Rows holds
/// every constant of the enumerations after it, one row each, but those of the array @p without, with the
/// array @p aliases naming the constants that repeat another's value. The build fails naming the constant or
/// the row. The row also declares the side (cMirrorSide()), which cEnumsProblem() reads among the variables
/// of its namespace: the enumerations a row names are the ones it checks.
#define ISOLINE_C_MIRROR(Rows, column, without, aliases, ...)                                            \
    static_assert(                                                                                       \
        ::isoline::meta::reflection::mirrorProblem<Rows, column, __VA_ARGS__>(#Rows, without, aliases)   \
            .empty(),                                                                                    \
        ::isoline::meta::reflection::mirrorProblem<Rows, column, __VA_ARGS__>(#Rows, without, aliases)); \
    [[maybe_unused]] constexpr auto ISOLINE_C_MIRROR_CONCAT(cMirrorSideOfLine, __LINE__) =               \
        ::isoline::meta::reflection::cMirrorSide<__VA_ARGS__>(#Rows)
