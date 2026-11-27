// SPDX-License-Identifier: Apache-2.0
//
// The installed package, used through `import isoline;`: the checks of the in-tree module tests
// instantiatesAndCalls, hostFunctionTemplate and constantsEvaluate (tests/modules/ImportConsumer_test.cpp)
// as a plain main. Exits 0 if and only if every check holds; otherwise it names each one that failed.
//
// Two modes build it (tests/consumer/ConsumerModes.cmake):
//  - import: the standard headers it spells, then the import. <memory> is among them for GCC, which
//    instantiates makeHostFunction()'s use of typeid only where <typeinfo> is visible (docs/modules.md).
//    Nothing else here makes <typeinfo> visible on libstdc++ (<iostream> would), so <memory> is what
//    the rule rests on, and dropping it fails the GCC build. <string_view> is absent on purpose:
//    MSVC once needed it here (C3889), for a deduced operator<=> in api/Imports.hpp, since fixed; its
//    absence keeps that fixed on the MSVC lanes.
//  - import-std (ISOLINE_CONSUMER_IMPORT_STD): `import std;`, and no #include at all.

#if defined(ISOLINE_CONSUMER_IMPORT_STD)
import std;
#else
    #include <array>
    #include <cstdio>
    #include <expected>
    #include <memory>
    #include <span>
    #include <variant>
#endif

import isoline;
import isoline_consumer.wasm_modules;

namespace
{

using namespace isoline::api;

/// The checks of one run: each failed one is reported and counted.
class Checks
{
  public:
    /// Reports @p check on stdout, unless @p holds.
    /// @param holds Whether the check holds.
    /// @param check The check, as the source spells it.
    /// @return @p holds.
    bool expect(bool holds, char const* check)
    {
        if (!holds)
        {
            std::printf("import-consumer: check failed: %s\n", check);
            ++_failures;
        }
        return holds;
    }

    /// Whether every check so far held.
    /// @return Whether none failed.
    [[nodiscard]] bool allHeld() const noexcept { return _failures == 0; }

  private:
    int _failures = 0;
};

/// instantiatesAndCalls: load the add module, instantiate it and call add(2, 3).
void instantiatesAndCalls(Checks& checks)
{
    auto const engine = Engine { EngineConfig {} };
    auto const module = engine.load(isoline::consumer::AddModule);
    if (!checks.expect(module.has_value(), "engine.load(AddModule).has_value()"))
        return;

    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto const instance = store.instantiate(*module, Imports {});
    if (!checks.expect(instance.has_value(), "store.instantiate(module, Imports {}).has_value()"))
        return;
    auto const add = instance->function("add");
    if (!checks.expect(add.has_value(), "instance->function(\"add\").has_value()"))
        return;

    auto const args = std::array<Value, 2> { I32 { 2 }, I32 { 3 } };
    auto results = std::array<Value, 1> {};
    auto const called = instance->call(*add, args, results);
    if (!checks.expect(called.has_value(), "instance->call(add, args, results).has_value()"))
        return;
    checks.expect(results[0] == Value { I32 { 5 } }, "add(2, 3) == I32 { 5 }");
}

/// hostFunctionTemplate: makeHostFunction<F> instantiated in the importer, with a lambda of this TU.
void hostFunctionTemplate(Checks& checks)
{
    auto const doubling =
        makeHostFunction([](HostContext&,
                            std::span<Value const> args,
                            std::span<Value> results) noexcept -> std::expected<void, Abrupt> {
            results[0] = I32 { std::get<I32>(args[0]).bits * 2 };
            return {};
        });

    auto const engine = Engine { EngineConfig {} };
    auto const module = engine.load(isoline::consumer::DoubleImportModule);
    if (!checks.expect(module.has_value(), "engine.load(DoubleImportModule).has_value()"))
        return;
    auto imports = Imports {};
    imports.define("env",
                   "double",
                   FuncType { .params = { ValueType { .kind = ValueKind::I32 } },
                              .results = { ValueType { .kind = ValueKind::I32 } } },
                   doubling);

    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto const instance = store.instantiate(*module, imports);
    if (!checks.expect(instance.has_value(), "store.instantiate(module, imports).has_value()"))
        return;
    auto const imported = instance->function("double");
    if (!checks.expect(imported.has_value(), "instance->function(\"double\").has_value()"))
        return;

    auto const args = std::array<Value, 1> { I32 { 21 } };
    auto results = std::array<Value, 1> {};
    auto const called = instance->call(*imported, args, results);
    if (!checks.expect(called.has_value(), "instance->call(double, args, results).has_value()"))
        return;
    checks.expect(results[0] == Value { I32 { 42 } }, "double(21) == I32 { 42 }");
}

/// constantsEvaluate: the package's inline variables and constexpr functions, constant-evaluated here.
void constantsEvaluate(Checks& checks)
{
    static_assert(isoline::FeatureCount == isoline::FeatureTable.size());
    static_assert(isoline::version().major == isoline::config::versionMajor);
    constexpr auto isMultiValueInWasm3 = isoline::FeatureSet::wasm3().has(isoline::Feature::MultiValue);
    checks.expect(isMultiValueInWasm3, "FeatureSet::wasm3().has(Feature::MultiValue)");
}

} // namespace

int main()
{
    auto checks = Checks {};
    instantiatesAndCalls(checks);
    hostFunctionTemplate(checks);
    constantsEvaluate(checks);
    return checks.allHeld() ? 0 : 1;
}
