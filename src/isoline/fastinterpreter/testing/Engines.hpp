// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/fastinterpreter/FastInterpreter.hpp>
#include <isoline/fastinterpreter/Lowering.hpp>
#include <isoline/fastinterpreter/testing/Lowering.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidatedModule.hpp>

/// What the execution tests share: one module instantiated on a fresh store, run by the reference interpreter
/// or by the fast interpreter under one dispatch, so that a test runs the same module on every engine and
/// compares what each did.
namespace isoline::fastinterpreter::testing
{

/// The engine a Harness runs its module on: the reference interpreter, or the fast one under a dispatch.
struct EngineUnderTest
{
    /// The fast interpreter's dispatch; none for the reference interpreter.
    std::optional<Dispatch> dispatch;
    /// What the fast interpreter's slot chunks come from; the global heap when null.
    std::pmr::memory_resource* slotMemory = nullptr;
};

/// The reference interpreter.
inline constexpr auto Reference = EngineUnderTest {};

/// The fast interpreter under @p dispatch.
[[nodiscard]] inline EngineUnderTest fast(Dispatch dispatch) noexcept
{
    return EngineUnderTest { .dispatch = dispatch };
}

/// The fast interpreter under each available dispatch, one per run of the calling test case (a Catch2
/// generator: call it once, at the test case's top level).
[[nodiscard]] inline EngineUnderTest fastEngineOfEachDispatch()
{
    return fast(GENERATE(from_range(availableDispatches())));
}

/// The reference interpreter, then the fast one under each available dispatch, one per run of the calling
/// test case (a Catch2 generator: call it once, at the test case's top level).
[[nodiscard]] inline EngineUnderTest referenceAndFastEngineOfEachDispatch()
{
    auto engines = std::vector<EngineUnderTest> { Reference };
    for (auto const dispatch: availableDispatches())
        engines.push_back(fast(dispatch));
    return GENERATE_COPY(from_range(engines));
}

/// One module instantiated on a fresh store, executed by one engine.
class Harness
{
  public:
    /// What a Harness may make of the fast engine's lowered code before it instantiates: code written by
    /// hand.
    using CodeRewrite = std::function<std::shared_ptr<LoweredModule const>(LoweredModule const&)>;

    /// What a Harness binds its module's imports to: the external values, in import order, which it may add
    /// to the store first (host functions).
    using Imports = std::function<std::vector<runtime::ExternValue>(runtime::Store&)>;

    /// Decodes, validates and instantiates @p builder's module on @p engine, in a store of @p limits.
    /// @param observer Observes the store's polls; none when null.
    Harness(ModuleBuilder const& builder,
            EngineUnderTest engine,
            runtime::StoreLimits limits = {},
            runtime::IPollObserver* observer = nullptr):
        Harness { builder, engine, CodeRewrite {}, Imports {}, limits, observer }
    {
    }

    /// Decodes, validates and instantiates @p builder's module on @p engine, its imports bound to what
    /// @p imports adds, in a store of @p limits.
    /// @param observer Observes the store's polls; none when null.
    Harness(ModuleBuilder const& builder,
            EngineUnderTest engine,
            Imports const& imports,
            runtime::StoreLimits limits = {},
            runtime::IPollObserver* observer = nullptr):
        Harness { builder, engine, CodeRewrite {}, imports, limits, observer }
    {
    }

    /// Decodes, validates and lowers @p builder's module for the fast engine @p engine, then instantiates it
    /// with the code @p rewrite makes of the lowered code, in a store of @p limits.
    /// @param observer Observes the store's polls; none when null.
    Harness(ModuleBuilder const& builder,
            EngineUnderTest engine,
            CodeRewrite const& rewrite,
            runtime::StoreLimits limits = {},
            runtime::IPollObserver* observer = nullptr):
        Harness { builder, engine, rewrite, Imports {}, limits, observer }
    {
    }

    /// Decodes, validates and instantiates @p builder's module on @p engine, its imports bound to what
    /// @p imports adds, in a store of @p limits whose memories come from @p allocator.
    /// @param allocator Allocates the store's memories; must outlive the harness.
    Harness(ModuleBuilder const& builder,
            EngineUnderTest engine,
            Imports const& imports,
            runtime::IMemoryAllocator& allocator,
            runtime::StoreLimits limits = {}):
        Harness { builder, engine, CodeRewrite {}, imports, limits, nullptr, &allocator }
    {
    }

    /// The executor that runs the module: the reference interpreter, or the fast one.
    [[nodiscard]] runtime::ICodeExecutor& executor() noexcept { return *_executor; }

    /// The validated module.
    [[nodiscard]] validate::ValidatedModule const& validated() const noexcept { return *_validated; }

  private:
    Harness(ModuleBuilder const& builder,
            EngineUnderTest engine,
            CodeRewrite const& rewrite,
            Imports const& imports,
            runtime::StoreLimits limits,
            runtime::IPollObserver* observer,
            runtime::IMemoryAllocator* allocator = nullptr):
        _store { limits,
                 runtime::StoreServices { .allocator = allocator != nullptr ? *allocator : _allocator,
                                          .pollObserver = observer } },
        _executor { executorOf(engine) }
    {
        auto validated =
            validate::ValidatedModule::load(builder.build(), FeatureSet::all(), binary::DecodeLimits {});
        REQUIRE(validated.has_value());
        _validated = *validated;
        if (engine.dispatch)
            _code = lowered(_validated, LoweringConfig { .dispatch = *engine.dispatch });
        if (rewrite)
        {
            REQUIRE(_code != nullptr);
            _code = rewrite(*_code);
        }
        auto const externs = imports ? imports(_store) : std::vector<runtime::ExternValue> {};
        auto const address = runtime::instantiate(_store, *_executor, _validated, _code, externs);
        REQUIRE(address.has_value());
        _address = *address;
    }

  public:
    /// Invokes the module's function @p index.
    /// @param index The function's index in the module's function index space.
    /// @param args Its arguments.
    /// @return Its results, or how it ended abruptly.
    [[nodiscard]] std::expected<std::vector<runtime::Value>, runtime::Abrupt> outcome(
        uint32_t index, std::vector<runtime::Value> const& args = {})
    {
        auto const address = function(index);
        auto results = std::vector<runtime::Value>(_store.functionType(address).results.size());
        return runtime::invokeChecked(_store, *_executor, address, args, results).outcome.transform([&] {
            return results;
        });
    }

    /// The single result of invoking function @p index, which must return one.
    [[nodiscard]] runtime::Value result(uint32_t index, std::vector<runtime::Value> const& args = {})
    {
        auto const results = outcome(index, args);
        REQUIRE(results.has_value());
        REQUIRE(results->size() == 1);
        return results->front();
    }

    /// The trap invoking function @p index ends with, which it must.
    [[nodiscard]] runtime::TrapCode trap(uint32_t index, std::vector<runtime::Value> const& args = {})
    {
        auto const ended = outcome(index, args);
        REQUIRE_FALSE(ended.has_value());
        REQUIRE(std::holds_alternative<runtime::Trap>(ended.error()));
        return std::get<runtime::Trap>(ended.error()).code;
    }

    /// The address of the module's function @p index.
    [[nodiscard]] runtime::FuncAddr function(uint32_t index)
    {
        return _store.moduleInstance(_address).functions.at(index);
    }

    /// The store.
    [[nodiscard]] runtime::Store& store() noexcept { return _store; }

    /// The module instance.
    [[nodiscard]] runtime::ModuleInstance const& instance() const { return _store.moduleInstance(_address); }

    /// The lowered code of the module; null on the reference engine.
    [[nodiscard]] std::shared_ptr<LoweredModule const> const& code() const noexcept { return _code; }

  private:
    [[nodiscard]] static std::unique_ptr<runtime::ICodeExecutor> executorOf(EngineUnderTest engine)
    {
        if (engine.dispatch && engine.slotMemory != nullptr)
            return std::make_unique<FastInterpreter>(*engine.dispatch, *engine.slotMemory);
        if (engine.dispatch)
            return std::make_unique<FastInterpreter>(*engine.dispatch);
        return std::make_unique<interpreter::ReferenceInterpreter>();
    }

    runtime::HeapMemoryAllocator _allocator;
    runtime::Store _store;
    std::unique_ptr<runtime::ICodeExecutor> _executor;
    std::shared_ptr<validate::ValidatedModule const> _validated;
    std::shared_ptr<LoweredModule const> _code;
    uint32_t _address = 0;
};

/// The operations of @p function's code, whichever dispatch it was lowered for.
[[nodiscard]] inline std::set<OperationId> operationsOf(LoweredFunction const& function)
{
    auto operations = std::set<OperationId> {};
    auto const code = std::span { function.code };
    auto offset = std::size_t { 0 };
    while (offset < code.size())
    {
        auto const operation = operationOfHeader(code[offset]);
        operations.insert(operation);
        offset += operationExtent(operation, code.subspan(offset));
    }
    return operations;
}

/// The operations of every function of @p module's code.
[[nodiscard]] inline std::set<OperationId> operationsOf(LoweredModule const& module)
{
    auto operations = std::set<OperationId> {};
    for (auto const& function: module.functions())
        operations.merge(operationsOf(function));
    return operations;
}

} // namespace isoline::fastinterpreter::testing
