// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>

#include <isoline/binary/Types.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/validate/Subtyping.hpp>
#include <isoline/validate/TypeMatching.hpp>

namespace isoline::component::validate
{

/// Why a provided core entity does not stand where a required one is expected.
///
/// The enumerators are the rows of CoreMismatchMessages, in the same order.
enum class CoreMismatchCode : uint8_t
{
    KindMismatch,
    FunctionType,
    TableElementType,
    TableIndexType,
    TableLimits,
    MemoryIndexType,
    MemoryPageSize,
    MemoryLimits,
    GlobalMutability,
    GlobalType,
};

/// One row of CoreMismatchMessages: a mismatch and its text, a std::format string of up to two strings.
struct CoreMismatchRow
{
    CoreMismatchCode code; ///< The mismatch; equal to the row's index.
    std::string_view text; ///< Its text: {0} what was expected, {1} what was found.
};

/// Every core mismatch, in CoreMismatchCode's order: wasm-tools 1.263.0's texts (SubtypeCx::entity_type,
/// table_type, memory_type, global_type and core_func_type, wasmparser 0.263.0
/// src/validator/component_types.rs:4069-4207). Isoline has no shared tables, memories or globals, so
/// their rows are absent.
inline constexpr auto CoreMismatchMessages = std::to_array<CoreMismatchRow>({
    { .code = CoreMismatchCode::KindMismatch, .text = "expected {0}, found {1}" },
    { .code = CoreMismatchCode::FunctionType, .text = "expected: {0}\nfound:    {1}" },
    { .code = CoreMismatchCode::TableElementType, .text = "expected table element type {0}, found {1}" },
    { .code = CoreMismatchCode::TableIndexType, .text = "mismatch in index type used for tables" },
    { .code = CoreMismatchCode::TableLimits, .text = "mismatch in table limits" },
    { .code = CoreMismatchCode::MemoryIndexType, .text = "mismatch in index type used for memories" },
    { .code = CoreMismatchCode::MemoryPageSize, .text = "mismatch in page size for memories" },
    { .code = CoreMismatchCode::MemoryLimits, .text = "mismatch in memory limits" },
    { .code = CoreMismatchCode::GlobalMutability, .text = "global types differ in mutability" },
    { .code = CoreMismatchCode::GlobalType, .text = "expected global type {0}, found {1}" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: CoreMismatchMessages)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(CoreMismatchCode::GlobalType) + 1;
    }(),
    "CoreMismatchMessages must hold one row per CoreMismatchCode, at the index of its enumerator");

/// A core mismatch: why, and wasm-tools' message for it.
struct CoreMismatch
{
    CoreMismatchCode code = CoreMismatchCode::KindMismatch; ///< Why.
    std::string message {};                                 ///< The row's text, formatted.
};

/// What each core extern kind is called in messages, by isoline::binary::ExternKind (EntityType::desc,
/// wasmparser 0.263.0 src/validator/types.rs).
inline constexpr auto CoreKindWords =
    std::to_array<std::string_view>({ "func", "table", "memory", "global", "tag" });

static_assert(CoreKindWords.size() == isoline::binary::ExternKindCount, "one word per core extern kind");

/// The word messages use for @p kind.
/// @param kind A core extern kind.
/// @return "func", "table", "memory", "global" or "tag".
[[nodiscard]] constexpr std::string_view coreKindWord(isoline::binary::ExternKind kind) noexcept
{
    return CoreKindWords[static_cast<std::size_t>(kind)];
}

/// The spelling wasm-tools gives a core value type in messages (ValType's Display): i32, funcref,
/// (ref func), (ref null 3). A concrete reference names the arena's canonical id, where wasm-tools
/// names its own internal one.
/// @param type The type.
/// @return Its spelling.
[[nodiscard]] std::string coreValueTypeText(isoline::binary::ValueType const& type);

/// The spelling wasm-tools gives a core function type in messages (FuncType's Display): `(func (param
/// i32) (result i32))`.
/// @param type The type.
/// @return Its spelling.
[[nodiscard]] std::string coreFunctionTypeText(isoline::binary::FuncType const& type);

/// The effective size of a core entity as wasm-tools measures it (EntityType::info, wasmparser 0.263.0
/// src/validator/types.rs:297-302 and SubType::type_info, types.rs:139-151): a function or a tag 2
/// plus its parameters and results, any other entity 1.
/// @param arena The arena whose ids @p type names.
/// @param type The entity.
/// @return Its size.
[[nodiscard]] uint32_t coreEntitySize(types::TypeArena const& arena, types::CoreEntityType const& type);

/// The text of a core mismatch.
/// @param code The mismatch.
/// @param expected What {0} names.
/// @param found What {1} names.
/// @return The row's text, formatted.
[[nodiscard]] inline CoreMismatch coreMismatch(CoreMismatchCode code,
                                               std::string_view expected = {},
                                               std::string_view found = {})
{
    return CoreMismatch { .code = code,
                          .message = std::vformat(CoreMismatchMessages[static_cast<std::size_t>(code)].text,
                                                  std::make_format_args(expected, found)) };
}

/// Whether core entity @p sub, provided, may stand where @p required is expected, by the rules and in
/// the order wasm-tools checks them (SubtypeCx::entity_type): the same kind; a function of an
/// equivalent type, and a tag of one (both ways); a table of an equivalent element type, the same
/// address type and matching limits; a memory of the same address type, the same page size and
/// matching limits; a global of the same mutability whose type is a subtype (equivalent, when mutable).
/// @param subArena The arena whose ids @p sub names.
/// @param sub The entity provided.
/// @param superArena The arena whose ids @p required names.
/// @param required The entity expected.
/// @param equivalent Whether a core function type id of @p subArena and one of @p superArena are
///                   equivalent; within one arena, equality of the ids.
/// @return Nothing when it may, else the mismatch.
template <typename Equivalent>
[[nodiscard]] std::expected<void, CoreMismatch> matchCoreEntity(types::TypeArena const& subArena,
                                                                types::CoreEntityType const& sub,
                                                                types::TypeArena const& superArena,
                                                                types::CoreEntityType const& required,
                                                                Equivalent const& equivalent)
{
    using isoline::binary::ExternKind;
    if (sub.kind != required.kind)
        return std::unexpected { coreMismatch(
            CoreMismatchCode::KindMismatch, coreKindWord(required.kind), coreKindWord(sub.kind)) };
    static_assert(isoline::binary::ExternKindCount == 5, "a new core extern kind needs its rule here");
    switch (sub.kind)
    {
        case ExternKind::Func:
        case ExternKind::Tag:
            if (!equivalent(sub.typeIndex, required.typeIndex))
                return std::unexpected { coreMismatch(
                    CoreMismatchCode::FunctionType,
                    coreFunctionTypeText(
                        superArena[types::CoreFunctionTypeId { .value = required.typeIndex }]),
                    coreFunctionTypeText(subArena[types::CoreFunctionTypeId { .value = sub.typeIndex }])) };
            return {};
        case ExternKind::Table:
            if (!isoline::validate::isEquivalentWith(sub.table.element, required.table.element, equivalent))
                return std::unexpected { coreMismatch(CoreMismatchCode::TableElementType,
                                                      coreValueTypeText(required.table.element),
                                                      coreValueTypeText(sub.table.element)) };
            if (sub.table.limits.addressType != required.table.limits.addressType)
                return std::unexpected { coreMismatch(CoreMismatchCode::TableIndexType) };
            if (!isoline::validate::limitsMatch(sub.table.limits, required.table.limits))
                return std::unexpected { coreMismatch(CoreMismatchCode::TableLimits) };
            return {};
        case ExternKind::Memory:
            if (sub.memory.limits.addressType != required.memory.limits.addressType)
                return std::unexpected { coreMismatch(CoreMismatchCode::MemoryIndexType) };
            if (sub.memory.pageSizeLog2 != required.memory.pageSizeLog2)
                return std::unexpected { coreMismatch(CoreMismatchCode::MemoryPageSize) };
            if (!isoline::validate::limitsMatch(sub.memory.limits, required.memory.limits))
                return std::unexpected { coreMismatch(CoreMismatchCode::MemoryLimits) };
            return {};
        case ExternKind::Global:
            if (sub.global.mutability != required.global.mutability)
                return std::unexpected { coreMismatch(CoreMismatchCode::GlobalMutability) };
            if (!isoline::validate::globalTypeMatches(sub.global, required.global, equivalent))
                return std::unexpected { coreMismatch(CoreMismatchCode::GlobalType,
                                                      coreValueTypeText(required.global.type),
                                                      coreValueTypeText(sub.global.type)) };
            return {};
    }
    return {}; // uncovered(exhaustive-switch)
}

} // namespace isoline::component::validate
