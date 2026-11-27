// SPDX-License-Identifier: Apache-2.0
#include <isoline/recipe_fixture/RecipeFixture.hpp>

#include <core/Assert.hpp>

#include <array>
#include <bit>
#include <variant>

namespace isoline::recipe_fixture
{

std::expected<std::int32_t, isoline::api::CallError> doubledSum(isoline::api::Instance const& instance,
                                                                std::int32_t a,
                                                                std::int32_t b)
{
    auto const add = instance.function("add");
    Require(add.has_value());
    auto const args =
        std::array<isoline::api::Value, 2> { isoline::api::I32 { std::bit_cast<std::uint32_t>(a) },
                                             isoline::api::I32 { std::bit_cast<std::uint32_t>(b) } };
    auto results = std::array<isoline::api::Value, 1> {};
    return instance.call(*add, args, results).transform([&results] {
        return std::bit_cast<std::int32_t>(std::get<isoline::api::I32>(results[0]).bits * 2U);
    });
}

} // namespace isoline::recipe_fixture
