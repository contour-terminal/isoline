// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/validate/CoreTypes.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Module.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/meta/Check.hpp>
#include <isoline/validate/TypeMatching.hpp>

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

namespace
{
    /// The core validator's error for an export of an index its kind's space does not hold, by ExternKind.
    constexpr auto UnknownIndexCodes = std::to_array<isoline::validate::ValidationErrorCode>({
        isoline::validate::ValidationErrorCode::UnknownFunction,
        isoline::validate::ValidationErrorCode::UnknownTable,
        isoline::validate::ValidationErrorCode::UnknownMemory,
        isoline::validate::ValidationErrorCode::UnknownGlobal,
        isoline::validate::ValidationErrorCode::UnknownTag,
    });
    static_assert(UnknownIndexCodes.size() == isoline::binary::ExternKindCount,
                  "UnknownIndexCodes holds one code per ExternKind, in its order");

    /// A module's types interned into an arena's canonical space, in section order, and the rewriting
    /// of a module's types into it: what both coreModuleTypeOf()s share.
    class ModuleTypeReader
    {
      public:
        ModuleTypeReader(types::TypeArena& arena, std::span<isoline::binary::FuncType const> section):
            _arena { arena }
        {
            _scope.reserve(section.size());
            for (auto type: section)
            {
                for (auto* list: { &type.params, &type.results })
                    for (auto& entry: *list)
                        entry = rewrite(entry);
                _scope.push_back(_arena.intern(std::move(type)));
            }
        }

        /// @p value with a concrete reference to the section rewritten to the arena's canonical id.
        [[nodiscard]] isoline::binary::ValueType rewrite(isoline::binary::ValueType value) const
        {
            if (value.kind == isoline::binary::ValueKind::Ref
                && value.heap.kind == isoline::binary::AbstractHeapType::Concrete)
            {
                Require(value.heap.typeIndex < _scope.size()); // a validated module names its own types
                value.heap.typeIndex = _scope[value.heap.typeIndex].value;
            }
            return value;
        }

        /// @p entity, in the section's indices, in the arena's.
        [[nodiscard]] isoline::validate::CoreExternType canonical(
            isoline::validate::CoreExternType entity) const
        {
            using isoline::binary::ExternKind;
            if (entity.kind == ExternKind::Func || entity.kind == ExternKind::Tag)
            {
                Require(entity.typeIndex < _scope.size());
                entity.typeIndex = _scope[entity.typeIndex].value;
            }
            entity.table.element = rewrite(entity.table.element);
            entity.global.type = rewrite(entity.global.type);
            return entity;
        }

        /// @p info with its measure: depth 1 and the module's type size.
        [[nodiscard]] types::CoreModuleTypeInfo measured(types::CoreModuleTypeInfo info) const
        {
            auto size = uint64_t { 1 };
            for (auto const& import: info.imports)
                size += coreEntitySize(_arena, import.type);
            for (auto const& entry: info.exports)
                size += coreEntitySize(_arena, entry.type);
            info.measure = types::TypeMeasure {
                .depth = 1, .effectiveSize = static_cast<uint32_t>(std::min<uint64_t>(size, UINT32_MAX))
            };
            return info;
        }

      private:
        types::TypeArena& _arena;
        std::vector<types::CoreFunctionTypeId> _scope {};
    };

    /// The extern type an api description names, in its module's section indices.
    [[nodiscard]] isoline::validate::CoreExternType externTypeOf(
        isoline::api::ExternType const& type, std::span<isoline::binary::FuncType const> section)
    {
        using isoline::binary::ExternKind;
        using isoline::validate::CoreExternType;
        auto const indexOf = [&](isoline::binary::FuncType const* function) {
            return static_cast<uint32_t>(function - section.data());
        };
        return std::visit(
            core::Overloaded {
                [&](isoline::binary::FuncType const* function) {
                    return CoreExternType { .kind = ExternKind::Func, .typeIndex = indexOf(function) };
                },
                [](isoline::binary::TableType const* table) {
                    return CoreExternType { .kind = ExternKind::Table, .table = *table };
                },
                [](isoline::binary::MemoryType const* memory) {
                    return CoreExternType { .kind = ExternKind::Memory, .memory = *memory };
                },
                [](isoline::binary::GlobalType const* global) {
                    return CoreExternType { .kind = ExternKind::Global, .global = *global };
                },
                [&](isoline::api::TagType const& tag) {
                    return CoreExternType { .kind = ExternKind::Tag, .typeIndex = indexOf(tag.type) };
                },
            },
            type);
    }
} // namespace

std::expected<types::CoreModuleTypeInfo, isoline::validate::ValidationError> coreModuleTypeOf(
    types::TypeArena& arena, isoline::binary::Module const& module)
{
    using isoline::binary::ExternKind;
    using isoline::validate::CoreExternType;
    auto const reader = ModuleTypeReader { arena, module.types };
    auto info = types::CoreModuleTypeInfo {};
    info.imports.reserve(module.imports.size());
    // Each index space, by kind: the imports of its kind first, then the module's own definitions.
    auto spaces = std::array<std::vector<CoreExternType>, isoline::binary::ExternKindCount> {};
    auto const spaceOf = [&](ExternKind kind) -> std::vector<CoreExternType>& {
        return spaces[static_cast<std::size_t>(kind)];
    };
    for (auto const& import: module.imports)
    {
        auto const entity = isoline::validate::externTypeOf(import);
        info.imports.push_back(
            types::CoreImportType { .module = std::string { isoline::binary::importModuleOf(module, import) },
                                    .name = import.name,
                                    .type = reader.canonical(entity) });
        spaceOf(import.kind).push_back(entity);
    }
    for (auto const typeIndex: module.functions)
        spaceOf(ExternKind::Func)
            .push_back(CoreExternType { .kind = ExternKind::Func, .typeIndex = typeIndex });
    for (auto const& table: module.tables)
        spaceOf(ExternKind::Table)
            .push_back(CoreExternType { .kind = ExternKind::Table, .table = table.type });
    for (auto const& memory: module.memories)
        spaceOf(ExternKind::Memory)
            .push_back(CoreExternType { .kind = ExternKind::Memory, .memory = memory });
    for (auto const& global: module.globals)
        spaceOf(ExternKind::Global)
            .push_back(CoreExternType { .kind = ExternKind::Global, .global = global.type });
    for (auto const& tag: module.tags)
        spaceOf(ExternKind::Tag)
            .push_back(CoreExternType { .kind = ExternKind::Tag, .typeIndex = tag.typeIndex });
    info.exports.reserve(module.exports.size());
    for (auto const& entry: module.exports)
    {
        auto const& space = spaceOf(entry.kind);
        if (entry.index >= space.size())
            return std::unexpected { isoline::validate::ValidationError {
                UnknownIndexCodes[static_cast<std::size_t>(entry.kind)],
                isoline::validate::NoFunction,
                isoline::validate::NoInstruction,
                entry.index } };
        info.exports.push_back(
            types::CoreExportType { .name = entry.name, .type = reader.canonical(space[entry.index]) });
    }
    return reader.measured(std::move(info));
}

types::CoreModuleTypeInfo coreModuleTypeOf(types::TypeArena& arena, isoline::api::Module const& module)
{
    auto const section = module.types();
    auto const reader = ModuleTypeReader { arena, section };
    auto info = types::CoreModuleTypeInfo {};
    info.imports.reserve(module.imports().size());
    for (auto const& import: module.imports())
        info.imports.push_back(
            types::CoreImportType { .module = std::string { import.module },
                                    .name = std::string { import.name },
                                    .type = reader.canonical(externTypeOf(import.type, section)) });
    info.exports.reserve(module.exports().size());
    for (auto const& entry: module.exports())
        info.exports.push_back(
            types::CoreExportType { .name = std::string { entry.name },
                                    .type = reader.canonical(externTypeOf(entry.type, section)) });
    return reader.measured(std::move(info));
}

} // namespace isoline::component::validate
