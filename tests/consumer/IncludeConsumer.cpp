// SPDX-License-Identifier: Apache-2.0
//
// The installed package, used through its headers: load the add module, instantiate it and call it.
// Exits 0 if and only if add(2, 3) returned 5; otherwise it names the check that failed.

#include <array>
#include <cstdio>

#include "AddModule.hpp"
#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>

namespace
{

/// Reports the failed @p check on stderr.
/// @param check The check, as the source spells it.
/// @return The exit status of a failed run.
int fail(char const* check)
{
    std::fputs("include-consumer: check failed: ", stderr);
    std::fputs(check, stderr);
    std::fputs("\n", stderr);
    return 1;
}

} // namespace

int main()
{
    using namespace isoline::api;

    auto const engine = Engine { EngineConfig {} };
    auto const module = engine.load(isoline::consumer::AddModule);
    if (!module.has_value())
        return fail("engine.load(AddModule).has_value()");

    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto const instance = store.instantiate(*module, Imports {});
    if (!instance.has_value())
        return fail("store.instantiate(module, Imports {}).has_value()");

    auto const add = instance->function("add");
    if (!add.has_value())
        return fail("instance->function(\"add\").has_value()");

    auto const args = std::array<Value, 2> { I32 { 2 }, I32 { 3 } };
    auto results = std::array<Value, 1> {};
    auto const called = instance->call(*add, args, results);
    if (!called.has_value())
        return fail("instance->call(add, args, results).has_value()");
    if (results[0] != Value { I32 { 5 } })
        return fail("results[0] == I32 { 5 }");
    return 0;
}
