// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <span>
#include <string_view>

#include <isoline/meta/reflection/CApiReflection.hpp>

// Every case is a STATIC_REQUIRE: what it checks is what CApiCheck.cpp asserts, at compile time, of the C
// API's rows. The fixtures are this file's, shaped as the C headers are: unscoped enumerations whose
// constants a row holds as a fixed-width integer, and structs a layout row names the fields of.

namespace
{

using isoline::meta::reflection::cEnumsProblem;
using isoline::meta::reflection::cMirrorSide;
using isoline::meta::reflection::CMirrorSide;
using isoline::meta::reflection::LayoutFieldRow;
using isoline::meta::reflection::layoutProblem;
using isoline::meta::reflection::LayoutTypeRow;
using isoline::meta::reflection::mirrorProblem;
using isoline::meta::reflection::UnmirroredEnumRow;

/// A C enumeration, as wasm.h spells one: its constants are numbers a uint8_t typedef carries.
enum fixture_kind_enum
{
    FIXTURE_KIND_A,
    FIXTURE_KIND_B,
    FIXTURE_KIND_C,
};

/// A second C enumeration of the same numbers, Isoline's beside upstream's, with its "none" value.
enum fixture_more_kind_enum
{
    FIXTURE_KIND_D = 0x7B,
    FIXTURE_KIND_INVALID = 0xFF,
};

/// A C enumeration with an alias: a Last constant equal to the last code.
enum fixture_code_enum
{
    FIXTURE_CODE_ONE = 1,
    FIXTURE_CODE_TWO = 2,
    FIXTURE_CODE_LAST = FIXTURE_CODE_TWO,
};

/// A C enumeration of details, each its row's code plus one.
enum fixture_detail_enum
{
    FIXTURE_DETAIL_ONE = 2,
    FIXTURE_DETAIL_TWO = 3,
};

/// The C++ enumeration the C one mirrors, with one enumerator the C API has no form of.
enum class Kind : uint8_t
{
    A,
    B,
    Hidden,
    C,
};

/// One row of a fixture mirror.
struct KindRow
{
    Kind kind;     ///< The C++ enumerator.
    uint8_t cKind; ///< Its C constant.
};

constexpr auto Mirrored = std::to_array<KindRow>({
    { .kind = Kind::A, .cKind = FIXTURE_KIND_A },
    { .kind = Kind::B, .cKind = FIXTURE_KIND_B },
    { .kind = Kind::C, .cKind = FIXTURE_KIND_C },
});

constexpr auto WithoutC = std::to_array<KindRow>({
    { .kind = Kind::A, .cKind = FIXTURE_KIND_A },
    { .kind = Kind::B, .cKind = FIXTURE_KIND_B },
});

constexpr auto TwiceB = std::to_array<KindRow>({
    { .kind = Kind::A, .cKind = FIXTURE_KIND_A },
    { .kind = Kind::B, .cKind = FIXTURE_KIND_B },
    { .kind = Kind::C, .cKind = FIXTURE_KIND_B },
    { .kind = Kind::C, .cKind = FIXTURE_KIND_C },
});

constexpr auto StrayValue = std::to_array<KindRow>({
    { .kind = Kind::A, .cKind = FIXTURE_KIND_A },
    { .kind = Kind::B, .cKind = FIXTURE_KIND_B },
    { .kind = Kind::C, .cKind = FIXTURE_KIND_C },
    { .kind = Kind::C, .cKind = 9 },
});

/// The C constants of two enumerations in one column, as the value kinds are.
struct BothKindsRow
{
    uint8_t cKind; ///< A constant of either enumeration.
};

constexpr auto BothKinds = std::to_array<BothKindsRow>({
    { .cKind = FIXTURE_KIND_A },
    { .cKind = FIXTURE_KIND_B },
    { .cKind = FIXTURE_KIND_C },
    { .cKind = FIXTURE_KIND_D },
});

constexpr auto BothKindsWithoutD = std::to_array<BothKindsRow>({
    { .cKind = FIXTURE_KIND_A },
    { .cKind = FIXTURE_KIND_B },
    { .cKind = FIXTURE_KIND_C },
});

/// One row of a fixture code table.
struct CodeRow
{
    uint32_t code; ///< The C code.
};

constexpr auto Codes = std::to_array<CodeRow>({ { .code = FIXTURE_CODE_ONE }, { .code = FIXTURE_CODE_TWO } });

/// The value a code row reports as a detail: its code plus one.
constexpr auto CodePlusOne = [](CodeRow const& row) {
    return row.code + 1;
};

constexpr auto NoneWithout = std::array<std::meta::info, 0> {};
constexpr auto NoneAliases = std::array<std::meta::info, 0> {};
constexpr auto LastAlias = std::to_array<std::meta::info>({ ^^FIXTURE_CODE_LAST });
constexpr auto OneAsAlias = std::to_array<std::meta::info>({ ^^FIXTURE_CODE_ONE });
constexpr auto ForeignAlias = std::to_array<std::meta::info>({ ^^FIXTURE_KIND_A });
constexpr auto InvalidWithout = std::to_array<std::meta::info>({ ^^FIXTURE_KIND_INVALID });
constexpr auto HiddenWithout = std::to_array<std::meta::info>({ ^^Kind::Hidden });
constexpr auto CWithout = std::to_array<std::meta::info>({ ^^FIXTURE_KIND_C });
constexpr auto ForeignWithout = std::to_array<std::meta::info>({ ^^FIXTURE_CODE_ONE });

/// A C struct, as layout.def describes one.
struct fixture_limits_t
{
    uint64_t struct_size; ///< Size first.
    uint64_t minimum;     ///< A bound.
    uint8_t kind;         ///< Whether it has a maximum.
};

/// A second C struct.
struct fixture_pair_t
{
    uint32_t first;  ///< One half.
    uint32_t second; ///< The other.
};

constexpr auto PairStructs = std::to_array<std::meta::info>({ ^^fixture_limits_t, ^^fixture_pair_t });
constexpr auto LimitsStructs = std::to_array<std::meta::info>({ ^^fixture_limits_t });

constexpr auto AllFields = std::to_array<LayoutFieldRow>({
    { .type = ^^fixture_limits_t, .field = "struct_size" },
    { .type = ^^fixture_limits_t, .field = "minimum" },
    { .type = ^^fixture_limits_t, .field = "kind" },
    { .type = ^^fixture_pair_t, .field = "first" },
    { .type = ^^fixture_pair_t, .field = "second" },
});

constexpr auto AllTypes = std::to_array<LayoutTypeRow>({
    { .type = ^^fixture_limits_t, .path = "struct_size", .fieldType = ^^uint64_t },
    { .type = ^^fixture_limits_t, .path = "minimum", .fieldType = ^^uint64_t },
    { .type = ^^fixture_limits_t, .path = "kind", .fieldType = ^^uint8_t },
    { .type = ^^fixture_pair_t, .path = "first", .fieldType = ^^uint32_t },
    { .type = ^^fixture_pair_t, .path = "second", .fieldType = ^^uint32_t },
});

constexpr auto LimitsTypes = std::to_array<LayoutTypeRow>({
    { .type = ^^fixture_limits_t, .path = "struct_size", .fieldType = ^^uint64_t },
    { .type = ^^fixture_limits_t, .path = "minimum", .fieldType = ^^uint64_t },
    { .type = ^^fixture_limits_t, .path = "kind", .fieldType = ^^uint8_t },
});

constexpr auto WrongType = std::to_array<LayoutTypeRow>({
    { .type = ^^fixture_limits_t, .path = "struct_size", .fieldType = ^^uint64_t },
    { .type = ^^fixture_limits_t, .path = "minimum", .fieldType = ^^uint32_t },
    { .type = ^^fixture_limits_t, .path = "kind", .fieldType = ^^uint8_t },
});

constexpr auto WithoutKindType = std::to_array<LayoutTypeRow>({
    { .type = ^^fixture_limits_t, .path = "struct_size", .fieldType = ^^uint64_t },
    { .type = ^^fixture_limits_t, .path = "minimum", .fieldType = ^^uint64_t },
});

/// A C struct with a field of an unnamed union type, as wasm_val_t's `of`.
struct fixture_value_t
{
    uint8_t kind; ///< Which member of the union holds the value.
    union {
        int32_t i32; ///< A 32-bit integer.
        int64_t i64; ///< A 64-bit integer.
    } of;            ///< The value.
};

constexpr auto ValueStructs = std::to_array<std::meta::info>({ ^^fixture_value_t });
constexpr auto ValueFields = std::to_array<LayoutFieldRow>({
    { .type = ^^fixture_value_t, .field = "kind" },
    { .type = ^^fixture_value_t, .field = "of" },
});
constexpr auto ValueTypes = std::to_array<LayoutTypeRow>({
    { .type = ^^fixture_value_t, .path = "kind", .fieldType = ^^uint8_t },
    { .type = ^^fixture_value_t, .path = "of.i32", .fieldType = ^^int32_t },
    { .type = ^^fixture_value_t, .path = "of.i64", .fieldType = ^^int64_t },
});
constexpr auto ValueTypesWithoutI64 = std::to_array<LayoutTypeRow>({
    { .type = ^^fixture_value_t, .path = "kind", .fieldType = ^^uint8_t },
    { .type = ^^fixture_value_t, .path = "of.i32", .fieldType = ^^int32_t },
});
constexpr auto ValueTypesWrongMember = std::to_array<LayoutTypeRow>({
    { .type = ^^fixture_value_t, .path = "kind", .fieldType = ^^uint8_t },
    { .type = ^^fixture_value_t, .path = "of.i32", .fieldType = ^^int32_t },
    { .type = ^^fixture_value_t, .path = "of.i64", .fieldType = ^^uint64_t },
});

constexpr auto LimitsFields = std::to_array<LayoutFieldRow>({
    { .type = ^^fixture_limits_t, .field = "struct_size" },
    { .type = ^^fixture_limits_t, .field = "minimum" },
    { .type = ^^fixture_limits_t, .field = "kind" },
});

constexpr auto WithoutLastField = std::to_array<LayoutFieldRow>({
    { .type = ^^fixture_limits_t, .field = "struct_size" },
    { .type = ^^fixture_limits_t, .field = "minimum" },
});

constexpr auto SwappedFields = std::to_array<LayoutFieldRow>({
    { .type = ^^fixture_limits_t, .field = "struct_size" },
    { .type = ^^fixture_limits_t, .field = "kind" },
    { .type = ^^fixture_limits_t, .field = "minimum" },
});

constexpr auto ExtraField = std::to_array<LayoutFieldRow>({
    { .type = ^^fixture_limits_t, .field = "struct_size" },
    { .type = ^^fixture_limits_t, .field = "minimum" },
    { .type = ^^fixture_limits_t, .field = "kind" },
    { .type = ^^fixture_limits_t, .field = "maximum" },
});

} // namespace

TEST_CASE("CApiReflection.mirroredEnumerationsPass")
{
    STATIC_REQUIRE(
        mirrorProblem<Mirrored, &KindRow::cKind, fixture_kind_enum>("Mirrored", NoneWithout, NoneAliases)
            .empty());
    STATIC_REQUIRE(
        mirrorProblem<Mirrored, &KindRow::kind, Kind>("Mirrored", HiddenWithout, NoneAliases).empty());
    // Two enumerations in one column, and a constant of the second that no value has.
    STATIC_REQUIRE(mirrorProblem<BothKinds, &BothKindsRow::cKind, fixture_kind_enum, fixture_more_kind_enum>(
                       "BothKinds", InvalidWithout, NoneAliases)
                       .empty());
    // A listed alias is in the row of the constant it equals.
    STATIC_REQUIRE(
        mirrorProblem<Codes, &CodeRow::code, fixture_code_enum>("Codes", NoneWithout, LastAlias).empty());
    // A projection as the column: the value a row reports.
    STATIC_REQUIRE(
        mirrorProblem<Codes, CodePlusOne, fixture_detail_enum>("Codes", NoneWithout, NoneAliases).empty());
}

TEST_CASE("CApiReflection.collisionIsNamedUnlessAnAlias")
{
    STATIC_REQUIRE(
        mirrorProblem<Codes, &CodeRow::code, fixture_code_enum>("Codes", NoneWithout, NoneAliases)
        == "Codes: fixture_code_enum::FIXTURE_CODE_TWO and fixture_code_enum::FIXTURE_CODE_LAST are both "
           "2: list one as an alias, with the reason, or give it a value of its own");
    // An alias that no other constant's value repeats, and one of another enumeration.
    STATIC_REQUIRE(
        mirrorProblem<Codes, &CodeRow::code, fixture_code_enum>("Codes", NoneWithout, OneAsAlias)
        == "Codes: fixture_code_enum::FIXTURE_CODE_ONE is listed as an alias, but no other constant is 1");
    STATIC_REQUIRE(mirrorProblem<Codes, &CodeRow::code, fixture_code_enum>("Codes", NoneWithout, ForeignAlias)
                   == "Codes: FIXTURE_KIND_A is listed as an alias, but is no constant of fixture_code_enum");
}

TEST_CASE("CApiReflection.constantWithoutRowIsNamed")
{
    STATIC_REQUIRE(
        mirrorProblem<WithoutC, &KindRow::cKind, fixture_kind_enum>("WithoutC", NoneWithout, NoneAliases)
        == "WithoutC: fixture_kind_enum::FIXTURE_KIND_C (2) has no row: add one, or list it as "
           "without one, with the reason");
    STATIC_REQUIRE(
        mirrorProblem<BothKindsWithoutD, &BothKindsRow::cKind, fixture_kind_enum, fixture_more_kind_enum>(
            "BothKindsWithoutD", InvalidWithout, NoneAliases)
        == "BothKindsWithoutD: fixture_more_kind_enum::FIXTURE_KIND_D (123) has no row: add one, or list it "
           "as "
           "without one, with the reason");
    // The C++ side: an enumerator added to the C++ enumeration is named as well.
    STATIC_REQUIRE(
        mirrorProblem<Mirrored, &KindRow::kind, Kind>("Mirrored", NoneWithout, NoneAliases)
        == "Mirrored: Kind::Hidden (2) has no row: add one, or list it as without one, with the reason");
}

TEST_CASE("CApiReflection.constantInTwoRowsIsNamed")
{
    STATIC_REQUIRE(
        mirrorProblem<TwiceB, &KindRow::cKind, fixture_kind_enum>("TwiceB", NoneWithout, NoneAliases)
        == "TwiceB: fixture_kind_enum::FIXTURE_KIND_B (1) is in 2 rows, rows 1 and 2: one row each");
}

TEST_CASE("CApiReflection.rowOfNoConstantIsNamed")
{
    STATIC_REQUIRE(
        mirrorProblem<StrayValue, &KindRow::cKind, fixture_kind_enum>("StrayValue", NoneWithout, NoneAliases)
        == "StrayValue: row 3 holds 9, which no constant of fixture_kind_enum is");
}

TEST_CASE("CApiReflection.staleWithoutListIsNamed")
{
    // Listed as without a row, but it has one.
    STATIC_REQUIRE(
        mirrorProblem<Mirrored, &KindRow::cKind, fixture_kind_enum>("Mirrored", CWithout, NoneAliases)
        == "Mirrored: fixture_kind_enum::FIXTURE_KIND_C is listed as without a row, but row 2 holds it");
    // Listed, but of another enumeration.
    STATIC_REQUIRE(
        mirrorProblem<Mirrored, &KindRow::cKind, fixture_kind_enum>("Mirrored", ForeignWithout, NoneAliases)
        == "Mirrored: FIXTURE_CODE_ONE is listed as without a row, but is no constant of "
           "fixture_kind_enum");
    STATIC_REQUIRE(
        mirrorProblem<BothKinds, &BothKindsRow::cKind, fixture_kind_enum, fixture_more_kind_enum>(
            "BothKinds", ForeignWithout, NoneAliases)
        == "BothKinds: FIXTURE_CODE_ONE is listed as without a row, but is no constant of fixture_kind_enum, "
           "fixture_more_kind_enum");
}

TEST_CASE("CApiReflection.fieldRowsNamingEveryMemberPass")
{
    STATIC_REQUIRE(layoutProblem(PairStructs, AllFields, AllTypes).empty());
}

TEST_CASE("CApiReflection.memberWithoutRowIsNamed")
{
    STATIC_REQUIRE(layoutProblem(LimitsStructs, WithoutLastField, LimitsTypes)
                   == "fixture_limits_t: member kind has no ISOLINE_LAYOUT_FIELD row");
    STATIC_REQUIRE(layoutProblem(PairStructs, WithoutLastField, AllTypes)
                   == "fixture_limits_t: member kind has no ISOLINE_LAYOUT_FIELD row");
}

TEST_CASE("CApiReflection.rowsOutOfOrderAreNamed")
{
    STATIC_REQUIRE(
        layoutProblem(LimitsStructs, SwappedFields, LimitsTypes)
        == "fixture_limits_t: field row 1 names kind, where member minimum is: one row per member, in "
           "declaration order");
}

TEST_CASE("CApiReflection.rowOfNoMemberIsNamed")
{
    STATIC_REQUIRE(layoutProblem(LimitsStructs, ExtraField, LimitsTypes)
                   == "fixture_limits_t: field row 3 names maximum, but the struct has 3 members");
}

TEST_CASE("CApiReflection.fieldRowsOfAStructWithoutRowAreNamed")
{
    STATIC_REQUIRE(layoutProblem(LimitsStructs, AllFields, AllTypes)
                   == "fixture_pair_t has ISOLINE_LAYOUT_FIELD rows but no ISOLINE_LAYOUT_STRUCT row");
}

TEST_CASE("CApiReflection.typeRowsOfEveryMemberPass")
{
    STATIC_REQUIRE(layoutProblem(ValueStructs, ValueFields, ValueTypes).empty());
}

TEST_CASE("CApiReflection.memberOfAnotherTypeIsNamed")
{
    STATIC_REQUIRE(
        layoutProblem(LimitsStructs, LimitsFields, WrongType)
        == "fixture_limits_t: member minimum is of long unsigned int, but its type row says unsigned int");
    STATIC_REQUIRE(
        layoutProblem(ValueStructs, ValueFields, ValueTypesWrongMember)
        == "fixture_value_t: member of.i64 is of long int, but its type row says long unsigned int");
}

TEST_CASE("CApiReflection.memberWithoutTypeRowIsNamed")
{
    STATIC_REQUIRE(layoutProblem(LimitsStructs, LimitsFields, WithoutKindType)
                   == "fixture_limits_t: member kind has no type row");
    // A union's members each have one: of.i64 is named, not of.
    STATIC_REQUIRE(layoutProblem(ValueStructs, ValueFields, ValueTypesWithoutI64)
                   == "fixture_value_t: member of.i64 has no type row");
}

// The discovery fixtures: C enumerations in a namespace of their own (the C headers' are the global one's),
// two mirrored, one listed as unmirrored and one neither; and the sides of the mirrors, as ISOLINE_C_MIRROR
// rows declare them.
namespace discovered
{
enum fixture_mirrored_enum
{
    FIXTURE_MIRRORED_A,
};
enum fixture_also_mirrored_enum
{
    FIXTURE_ALSO_MIRRORED_A,
};
enum fixture_listed_enum
{
    FIXTURE_LISTED_A,
};
/// Not of the C API's prefix: never discovered.
enum other_enum
{
    OTHER_A,
};
} // namespace discovered

namespace fixtureMirrors
{
[[maybe_unused]] constexpr auto First = cMirrorSide<discovered::fixture_mirrored_enum>("First");
[[maybe_unused]] constexpr auto Second =
    cMirrorSide<discovered::fixture_also_mirrored_enum, discovered::fixture_listed_enum>("Second");
} // namespace fixtureMirrors

namespace fewerMirrors
{
[[maybe_unused]] constexpr auto First = cMirrorSide<discovered::fixture_mirrored_enum>("First");
} // namespace fewerMirrors

namespace
{

constexpr auto FixturePrefixes = std::to_array<std::string_view>({ "fixture_" });
constexpr auto NoneUnmirrored = std::array<UnmirroredEnumRow, 0> {};
constexpr auto AlsoListed = std::to_array<UnmirroredEnumRow>({
    { .enumeration = ^^discovered::fixture_also_mirrored_enum, .reason = "the fixture's" },
    { .enumeration = ^^discovered::fixture_listed_enum, .reason = "the fixture's" },
});
constexpr auto ListedTwice = std::to_array<UnmirroredEnumRow>({
    { .enumeration = ^^discovered::fixture_listed_enum, .reason = "the fixture's" },
});
constexpr auto ListedForeign = std::to_array<UnmirroredEnumRow>({
    { .enumeration = ^^discovered::fixture_also_mirrored_enum, .reason = "the fixture's" },
    { .enumeration = ^^discovered::fixture_listed_enum, .reason = "the fixture's" },
    { .enumeration = ^^discovered::other_enum, .reason = "the fixture's" },
});

} // namespace

TEST_CASE("CApiReflection.everyCEnumerationIsMirroredOrListed")
{
    STATIC_REQUIRE(cEnumsProblem<^^fixtureMirrors>(^^discovered, NoneUnmirrored, FixturePrefixes).empty());
    STATIC_REQUIRE(cEnumsProblem<^^fewerMirrors>(^^discovered, AlsoListed, FixturePrefixes).empty());
}

TEST_CASE("CApiReflection.cEnumerationNeitherMirroredNorListedIsNamed")
{
    STATIC_REQUIRE(
        cEnumsProblem<^^fewerMirrors>(^^discovered, NoneUnmirrored, FixturePrefixes)
        == "fixture_also_mirrored_enum is a C enumeration no ISOLINE_C_MIRROR row checks: add the row of "
           "the table that converts it, or list it as unmirrored, with the reason");
}

TEST_CASE("CApiReflection.staleUnmirroredListIsNamed")
{
    STATIC_REQUIRE(cEnumsProblem<^^fixtureMirrors>(^^discovered, ListedTwice, FixturePrefixes)
                   == "fixture_listed_enum is listed as unmirrored, but an ISOLINE_C_MIRROR row checks it");
    STATIC_REQUIRE(cEnumsProblem<^^fewerMirrors>(^^discovered, ListedForeign, FixturePrefixes)
                   == "other_enum is listed as unmirrored, but is no C enumeration of the C API's headers");
}
