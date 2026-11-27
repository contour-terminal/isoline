// SPDX-License-Identifier: Apache-2.0
#pragma once

// Components the loading and planning tests share, built with ComponentBuilder.

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/testing/Builder.hpp>

namespace isoline::component::binary::testing
{

/// A core module exporting `id`, a function (i32) -> i32 returning its argument.
/// @return Its bytes.
[[nodiscard]] inline std::vector<std::byte> identityModule()
{
    return isoline::binary::testing::ModuleBuilder {}
        .type(isoline::binary::FuncType { .params = { isoline::binary::I32 },
                                          .results = { isoline::binary::I32 } })
        .function(0, {}, { 0x20, 0x00 })
        .exportFunction("id", 0)
        .build();
}

/// A component exporting `id`, a func (x: u32) -> u32 lifted from identityModule()'s export.
/// @return Its builder.
[[nodiscard]] inline ComponentBuilder liftedIdentity()
{
    auto builder = ComponentBuilder {};
    auto const module = builder.coreModule(identityModule());
    auto const instance = builder.coreInstantiate(module, {});
    auto const core = builder.aliasCoreExport(instance, "id", CoreSort::Func);
    auto const type = builder.type(
        TypeDefinition { .type = FunctionTypeDefinition {
                             .asyncness = Asyncness::Sync,
                             .params = { LabeledType { .label = "x", .type = PrimitiveValueType::U32 } },
                             .result = PrimitiveValueType::U32 } });
    auto const lifted = builder.canonLift(core, {}, type);
    std::ignore = builder.exportItem("id", Sort::Func, lifted);
    return builder;
}

/// @p levels components, each but the innermost nesting the next and instantiating it twice: the
/// component that inlining would make 2^(levels - 1) instances of, in a few bytes per level.
/// @param levels The number of components; at least one.
/// @return The outermost one's builder.
[[nodiscard]] inline ComponentBuilder doublingComponent(uint32_t levels)
{
    auto builder = ComponentBuilder {};
    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 1 }, levels))
    {
        auto outer = ComponentBuilder {};
        auto const child = outer.nested(std::move(builder));
        std::ignore = outer.instantiate(child, {});
        std::ignore = outer.instantiate(child, {});
        builder = std::move(outer);
    }
    return builder;
}

} // namespace isoline::component::binary::testing
