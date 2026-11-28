// SPDX-License-Identifier: Apache-2.0
//
// The C API's checks, where reflection takes the native path and the C API is built: every constant of a C
// enumeration a row table of the C layer stands for has its row, as every enumerator of the C++ enumeration
// on the table's other side does; every C enumeration of the C API's headers is mirrored or listed as not;
// and the rows of layout.def name every member of their struct, in order, with its type. The C headers are
// read as C++ here, every one of them: CApiHeaders.hpp, which CMake generates from the C API's header set,
// includes each. Compile-only: the translation unit defines nothing, and nothing links it.

#include <array>
#include <meta>
#include <string_view>

#include <isoline/api/Store.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/c/detail/EnumRows.hpp>
#include <isoline/c/detail/ErrorRows.hpp>
#include <isoline/c/isoline.h>
#include <isoline/meta/reflection/CApiHeaders.hpp>
#include <isoline/meta/reflection/CApiReflection.hpp>

namespace isoline::meta::reflection
{

namespace
{
    /// No constant is without a row, and none is an alias.
    constexpr auto None = std::array<std::meta::info, 0> {};

    /// The C kinds of value no row holds: ISOLINE_VALKIND_INVALID is what wasm_valtype_kind() gives for
    /// NULL, which no value type has and wasm_valtype_new() refuses.
    constexpr auto ValueKindsWithout = std::to_array<std::meta::info>({ ^^ISOLINE_VALKIND_INVALID });

    /// The C heaps no row holds: ISOLINE_HEAP_NOT_REFERENCE is what a number or vector type, and NULL,
    /// report, which is no heap.
    constexpr auto CHeapsWithout = std::to_array<std::meta::info>({ ^^ISOLINE_HEAP_NOT_REFERENCE });

    /// The C++ heaps no row holds: the validator's bottom heap, which no module declares, has no C form
    /// (HeapRows).
    constexpr auto HeapsWithout = std::to_array<std::meta::info>({ ^^binary::AbstractHeapType::Bot });

    /// The error codes that name another code: ISOLINE_ERROR_LAST, the last code, by which a caller bounds
    /// a table of its own.
    constexpr auto ErrorCodeAliases = std::to_array<std::meta::info>({ ^^ISOLINE_ERROR_LAST });

    /// The rows of the type refusals' details.
    using InvalidTypeRows = c::detail::EnumErrorRows<api::InvalidTypeReason>;

    /// The detail an invalid-type row's error is reported with (c::detail::detailOf(): its enumerator plus
    /// one), which an ISOLINE_INVALID_TYPE_ constant names.
    constexpr auto InvalidTypeDetailOf = [](c::detail::EnumErrorRow<api::InvalidTypeReason> const& row) {
        return c::detail::detailOf(row.code, row.value);
    };

    /// The C enumerations no table converts, each with its reason. They are read by a condition, not looked
    /// up (detail/Convert.cpp), and each has exactly two constants, which Types_test.cpp's cases pin.
    constexpr auto UnmirroredCEnums = std::to_array<UnmirroredEnumRow>({
        { .enumeration = ^^isoline_nullability_enum,
          .reason = "a reference type is nullable or not: toCNullability() is one condition" },
        { .enumeration = ^^isoline_maximum_kind_enum,
          .reason = "limits have a maximum or not: the limits conversions test one condition each way" },
    });

    /// The prefixes of the C API's names: wasm.h's and Isoline's own.
    constexpr auto CApiPrefixes = std::to_array<std::string_view>({ "wasm_", "isoline_" });

    namespace mirrors
    {
        // One row per side of a mirror: a table, its column, the constants no row holds, the constants that
        // repeat another's value, and the enumerations whose constants the column holds. A table of the C
        // layer that converts a C enumeration is a row here, which cEnumsProblem() below finds.
        ISOLINE_C_MIRROR(c::detail::ValueKindRows,
                         &c::detail::ValueKindRow::kind,
                         ValueKindsWithout,
                         None,
                         wasm_valkind_enum,
                         isoline_valkind_enum);
        ISOLINE_C_MIRROR(
            c::detail::HeapRows, &c::detail::HeapRow::cHeap, CHeapsWithout, None, isoline_heap_enum);
        ISOLINE_C_MIRROR(
            c::detail::HeapRows, &c::detail::HeapRow::heap, HeapsWithout, None, binary::AbstractHeapType);
        ISOLINE_C_MIRROR(
            c::detail::ExternKindRows, &c::detail::ExternKindRow::cKind, None, None, wasm_externkind_enum);
        ISOLINE_C_MIRROR(
            c::detail::ExternKindRows, &c::detail::ExternKindRow::kind, None, None, binary::ExternKind);
        ISOLINE_C_MIRROR(c::detail::MutabilityRows,
                         &c::detail::MutabilityRow::cMutability,
                         None,
                         None,
                         wasm_mutability_enum);
        ISOLINE_C_MIRROR(
            c::detail::MutabilityRows, &c::detail::MutabilityRow::mutability, None, None, binary::Mutability);
        ISOLINE_C_MIRROR(c::detail::ErrorCodeRows,
                         &c::detail::ErrorCodeRow::code,
                         None,
                         ErrorCodeAliases,
                         isoline_error_code_enum);
        ISOLINE_C_MIRROR(
            InvalidTypeRows::Rows, InvalidTypeDetailOf, None, None, isoline_invalid_type_detail_enum);
        ISOLINE_C_MIRROR(InvalidTypeRows::Rows,
                         &c::detail::EnumErrorRow<api::InvalidTypeReason>::value,
                         None,
                         None,
                         api::InvalidTypeReason);
    } // namespace mirrors

    static_assert(cEnumsProblem<^^mirrors>(^^::, UnmirroredCEnums, CApiPrefixes).empty(),
                  cEnumsProblem<^^mirrors>(^^::, UnmirroredCEnums, CApiPrefixes));

#define ISOLINE_LAYOUT_SCALAR(type, size)
#define ISOLINE_LAYOUT_STRUCT(type, size, align) ^^type,
#define ISOLINE_LAYOUT_FIELD(type, field, offset, size)
#define ISOLINE_LAYOUT_UPSTREAM_PADDING(type, offset, size)
    /// The structs of layout.def's ISOLINE_LAYOUT_STRUCT rows.
    constexpr auto LayoutStructs = std::to_array<std::meta::info>({
#include <isoline/c/abi/layout.def>
    });
#undef ISOLINE_LAYOUT_STRUCT
#undef ISOLINE_LAYOUT_FIELD

#define ISOLINE_LAYOUT_STRUCT(type, size, align)
#define ISOLINE_LAYOUT_FIELD(name, member, offset, size) { .type = ^^name, .field = #member },
    /// layout.def's ISOLINE_LAYOUT_FIELD rows, in order.
    constexpr auto LayoutFields = std::to_array<LayoutFieldRow>({
#include <isoline/c/abi/layout.def>
    });
#undef ISOLINE_LAYOUT_FIELD

#define ISOLINE_LAYOUT_FIELD(type, field, offset, size)
#define ISOLINE_LAYOUT_FIELD_TYPE(name, member, memberType) \
    { .type = ^^name, .path = #member, .fieldType = ^^memberType },
#define ISOLINE_LAYOUT_UNION_MEMBER_TYPE(name, field, member, memberType) \
    { .type = ^^name, .path = #field "." #member, .fieldType = ^^memberType },
    /// layout.def's type rows, in order.
    constexpr auto LayoutTypes = std::to_array<LayoutTypeRow>({
#include <isoline/c/abi/layout.def>
    });
#undef ISOLINE_LAYOUT_SCALAR
#undef ISOLINE_LAYOUT_STRUCT
#undef ISOLINE_LAYOUT_FIELD
#undef ISOLINE_LAYOUT_UPSTREAM_PADDING
#undef ISOLINE_LAYOUT_FIELD_TYPE
#undef ISOLINE_LAYOUT_UNION_MEMBER_TYPE

    static_assert(layoutProblem(LayoutStructs, LayoutFields, LayoutTypes).empty(),
                  "layout.def: " + layoutProblem(LayoutStructs, LayoutFields, LayoutTypes));
} // namespace

} // namespace isoline::meta::reflection
