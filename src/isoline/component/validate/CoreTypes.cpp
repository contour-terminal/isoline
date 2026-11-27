// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/validate/CoreTypes.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include <isoline/binary/Types.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::validate
{

namespace
{
    using isoline::binary::AbstractHeapType;

    /// One row of AbstractHeapWords: how an abstract heap is spelled in a reference type, nullable or
    /// not (AbstractHeapType::as_str, wasmparser 0.263.0 src/readers/core/types.rs:1653-1676).
    struct AbstractHeapWord
    {
        std::string_view nullable; ///< In `<word>ref`.
        std::string_view nonNull;  ///< In `(ref <word>)`.
    };

    /// Per isoline::binary::AbstractHeapType up to Bot, in its order; Bot is never a declared type.
    constexpr auto AbstractHeapWords = std::to_array<AbstractHeapWord>({
        { .nullable = "func", .nonNull = "func" },
        { .nullable = "extern", .nonNull = "extern" },
        { .nullable = "exn", .nonNull = "exn" },
        { .nullable = "nullfunc", .nonNull = "nofunc" },
        { .nullable = "nullextern", .nonNull = "noextern" },
        { .nullable = "nullexn", .nonNull = "noexn" },
        { .nullable = "", .nonNull = "" },
    });

    static_assert(AbstractHeapWords.size() == static_cast<std::size_t>(AbstractHeapType::Concrete),
                  "one spelling per abstract heap type");

    /// The numeric types' spellings, by isoline::binary::ValueKind up to V128.
    constexpr auto NumberWords = std::to_array<std::string_view>({ "i32", "i64", "f32", "f64", "v128" });

    static_assert(NumberWords.size() == static_cast<std::size_t>(isoline::binary::ValueKind::Ref),
                  "one spelling per numeric value kind");

    void appendTypes(std::string& text,
                     std::string_view keyword,
                     std::vector<isoline::binary::ValueType> const& types)
    {
        if (types.empty())
            return;
        text += std::format(" ({}", keyword);
        for (auto const& type: types)
            text += " " + coreValueTypeText(type);
        text += ")";
    }
} // namespace

std::string coreValueTypeText(isoline::binary::ValueType const& type)
{
    if (type.kind != isoline::binary::ValueKind::Ref)
        return std::string { NumberWords[static_cast<std::size_t>(type.kind)] };
    auto const isNullable = type.nullable == isoline::binary::Nullable::Yes;
    if (type.heap.kind == AbstractHeapType::Concrete)
        return std::format("(ref {}{})", isNullable ? "null " : "", type.heap.typeIndex);
    auto const& word = AbstractHeapWords[static_cast<std::size_t>(type.heap.kind)];
    if (isNullable)
        return std::format("{}ref", word.nullable);
    return std::format("(ref {})", word.nonNull);
}

std::string coreFunctionTypeText(isoline::binary::FuncType const& type)
{
    auto text = std::string { "(func" };
    appendTypes(text, "param", type.params);
    appendTypes(text, "result", type.results);
    return text + ")";
}

uint32_t coreEntitySize(types::TypeArena const& arena, types::CoreEntityType const& type)
{
    if (type.kind != isoline::binary::ExternKind::Func && type.kind != isoline::binary::ExternKind::Tag)
        return 1;
    auto const& function = arena[types::CoreFunctionTypeId { .value = type.typeIndex }];
    return static_cast<uint32_t>(2 + function.params.size() + function.results.size());
}

} // namespace isoline::component::validate
