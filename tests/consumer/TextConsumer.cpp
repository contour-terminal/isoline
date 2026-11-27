// SPDX-License-Identifier: Apache-2.0
//
// The installed text format, isoline::text, used through its headers (mode include-text of
// ConsumerModes.cmake): the consumer asks find_package() for component text alone, and the package loads
// core beneath it. Compiles the add module from its text, then loads, instantiates and calls it. Exits 0 if
// and only if add(2, 3) returned 5; otherwise it names the check that failed.

#include <array>
#include <cstdio>
#include <string_view>

#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

namespace
{

/// The add module, as text.
constexpr auto AddModuleText = std::string_view {
    R"((module (func (export "add") (param i32 i32) (result i32) local.get 0 local.get 1 i32.add)))"
};

/// Reports the failed @p check on stderr.
/// @param check The check, as the source spells it.
/// @return The exit status of a failed run.
int fail(char const* check)
{
    std::fputs("text-consumer: check failed: ", stderr);
    std::fputs(check, stderr);
    std::fputs("\n", stderr);
    return 1;
}

} // namespace

int main()
{
    using namespace isoline::api;

    auto const bytes = isoline::text::compileText(AddModuleText, isoline::text::TextLimits {});
    if (!bytes.has_value())
        return fail("compileText(AddModuleText).has_value()");

    auto const engine = Engine { EngineConfig {} };
    auto const module = engine.load(*bytes);
    if (!module.has_value())
        return fail("engine.load(bytes).has_value()");

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
