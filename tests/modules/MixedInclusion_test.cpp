// SPDX-License-Identifier: Apache-2.0
//
// Isoline included and imported in one TU, in the supported order (D7, docs/modules.md): every Isoline
// header first, then `import isoline;`. The names of both sides are the same entities, so they mix freely:
// isoline::version() comes from the import alone (no header here declares it), and the rest from both.

#include <catch2/catch_test_macros.hpp>

#include <array>

#include "AddModule.hpp"
#include "HeaderSide.hpp"
#include <isoline/api/Store.hpp>

import isoline;

TEST_CASE("Modules.includeThenImportInOneTu")
{
    static_assert(isoline::version().major == isoline::config::versionMajor);

    auto const engine = isoline::api::Engine { isoline::api::EngineConfig {} };
    auto const module = engine.load(isoline::consumer::AddModule);
    REQUIRE(module.has_value());
    auto allocator = isoline::api::HeapMemoryAllocator {};
    auto store = isoline::api::Store { engine, allocator };

    // The header side's function, declared by the header, takes and returns the import's types too.
    auto const instance = isoline::modules_test::instantiateAdd(store, *module);
    REQUIRE(instance.has_value());
    auto const add = instance->function("add");
    REQUIRE(add.has_value());
    auto const args =
        std::array<isoline::api::Value, 2> { isoline::api::I32 { 40 }, isoline::api::I32 { 2 } };
    auto results = std::array<isoline::api::Value, 1> {};
    REQUIRE(instance->call(*add, args, results).has_value());
    CHECK(results[0] == isoline::api::Value { isoline::api::I32 { 42 } });
    CHECK(isoline::modules_test::featureTableAddressFromHeaders() == &isoline::FeatureTable);
}
