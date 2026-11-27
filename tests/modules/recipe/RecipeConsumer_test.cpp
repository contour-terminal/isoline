// SPDX-License-Identifier: Apache-2.0
//
// The recipe's consumer: `import isoline.recipe_fixture;` and no other import, no Isoline header. Module
// isoline is reachable only as the fixture's `export import isoline;` (the row's IMPORTS), so every
// isoline::api name spelled here proves that re-export.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>

#include "AddModule.hpp"

import isoline.recipe_fixture;

TEST_CASE("Modules.recipeModuleReexportsIsoline")
{
    using namespace isoline::api;

    auto const engine = Engine { EngineConfig {} };
    auto const module = engine.load(isoline::consumer::AddModule);
    REQUIRE(module.has_value());
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto const instance = store.instantiate(*module, Imports {});
    REQUIRE(instance.has_value());

    std::expected<std::int32_t, CallError> const doubled =
        isoline::recipe_fixture::doubledSum(*instance, 2, 3);
    REQUIRE(doubled.has_value());
    CHECK(*doubled == 10);
}
