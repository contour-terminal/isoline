// SPDX-License-Identifier: Apache-2.0
//
// The installed recipe fixture, used through `import isoline.recipe_fixture;` alone (mode import-fixture of
// ConsumerModes.cmake): its package component is recipe_fixture, which the consumer asks for alone, and
// module isoline is reachable only as the fixture's `export import isoline;`. Exits 0 if and only if
// doubledSum(2, 3) is 10; otherwise it names the check that failed.

#include <cstdint>
#include <cstdio>
#include <expected>

import isoline.recipe_fixture;
import isoline_consumer.wasm_modules;

namespace
{

/// Reports @p check on stdout, unless @p holds.
/// @param holds Whether the check holds.
/// @param check The check, as the source spells it.
/// @return @p holds.
bool expect(bool holds, char const* check)
{
    if (!holds)
        std::printf("fixture-consumer: check failed: %s\n", check);
    return holds;
}

} // namespace

int main()
{
    using namespace isoline::api;

    auto const engine = Engine { EngineConfig {} };
    auto const module = engine.load(isoline::consumer::AddModule);
    if (!expect(module.has_value(), "engine.load(AddModule).has_value()"))
        return 1;
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto const instance = store.instantiate(*module, Imports {});
    if (!expect(instance.has_value(), "store.instantiate(module, Imports {}).has_value()"))
        return 1;
    std::expected<std::int32_t, CallError> const doubled =
        isoline::recipe_fixture::doubledSum(*instance, 2, 3);
    if (!expect(doubled.has_value(), "doubledSum(instance, 2, 3).has_value()"))
        return 1;
    return expect(*doubled == 10, "doubledSum(instance, 2, 3) == 10") ? 0 : 1;
}
