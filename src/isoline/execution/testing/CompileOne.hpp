// SPDX-License-Identifier: Apache-2.0
#pragma once

// Compiled code run as the Compiled engine will run it, for the execution suites and the compiler's executed
// tests (P5 Tasks 14 and 15): compileModule() takes a module through the Compiler and seals it;
// CompiledInstance instantiates it in a store of its own through a CompiledExecutor, and calls its functions
// as an embedder does (runtime::invokeChecked()), or through another trampoline, or runs a stub within a lent
// budget. Test support, linked into no shipped binary.

#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/compiler/machine/TargetDescription.hpp>
#include <isoline/execution/CodeMemory.hpp>
#include <isoline/execution/CompiledExecutor.hpp>
#include <isoline/execution/CompiledModuleCode.hpp>
#include <isoline/execution/RuntimeStubs.hpp>
#include <isoline/execution/StackBounds.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/PollGate.hpp>
#include <isoline/runtime/RawValue.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::execution::testing
{

/// The validated module of the WebAssembly text @p wat (Require: it compiles and validates with every
/// feature).
[[nodiscard]] std::shared_ptr<validate::ValidatedModule const> moduleOf(std::string_view wat);

/// A compile for this host's ABI on x86-64, in @p mode, with @p features: what code the tests run is compiled
/// for (a feature the running CPU lacks is the test's to avoid).
[[nodiscard]] compiler::CompileConfig hostConfig(runtime::ExecutionMode mode,
                                                 compiler::machine::CpuFeatures features);

/// Whether this host runs x86-64 code: where compiled code executes.
[[nodiscard]] bool hostRunsCompiledCode() noexcept;

/// Store limits whose requiredNativeStack() a test thread holds, the main thread of every platform included
/// (Windows' 1 MiB too): what the harness's stores have unless a test says otherwise, so that Deterministic
/// code, whose executor refuses a thread with less than the grant left, runs anywhere.
inline constexpr auto HarnessLimits =
    runtime::StoreLimits { .maxCallDepth = 500, .maxStackValues = 4'096, .maxNestedInvocations = 4 };

/// Compiles @p module for @p config through the Compiler (on a budget no test reaches), and seals it.
/// @return The module's code; or the compile error.
[[nodiscard]] std::expected<std::shared_ptr<CompiledModuleCode const>, compiler::CompileError> compileModule(
    validate::ValidatedModule const& module, compiler::CompileConfig const& config);

/// Whether a store is interrupted before its first call.
enum class Interruption : uint8_t
{
    None,    ///< Not interrupted.
    Pending, ///< Interrupted once set up, so that the first poll of every call ends it.
};

/// How a CompiledInstance's store is set up.
struct InstanceOptions
{
    runtime::StoreLimits limits = HarnessLimits; ///< The store's limits.
    runtime::IPollObserver* observer = nullptr;  ///< Its poll observer; none when null.
    /// The native stack an invocation is granted; none for requiredNativeStack(limits).
    std::optional<uint64_t> nativeStackBytes {};
    /// Adds the imports to the store and returns them, in import order; none when null.
    std::function<std::vector<runtime::ExternValue>(runtime::Store&)> imports {};
    Interruption interruption = Interruption::None; ///< Whether the store is interrupted once set up.
    /// Where the calling thread's stack ends; the thread's own (ThreadStackBounds) when null. Referenced, so
    /// it must outlive the instance.
    IStackBounds const* bounds = nullptr;
};

/// A compiled module instantiated in a store of its own, run by a CompiledExecutor of its config's mode.
class CompiledInstance
{
  public:
    /// Compiles @p module for @p config and instantiates it through a CompiledExecutor (Require: the executor
    /// is created, and the instance is).
    /// @return The instance; or the compile error.
    [[nodiscard]] static std::expected<std::unique_ptr<CompiledInstance>, compiler::CompileError> create(
        std::shared_ptr<validate::ValidatedModule const> module,
        compiler::CompileConfig const& config,
        InstanceOptions options = {});

    CompiledInstance(CompiledInstance const&) = delete;
    CompiledInstance(CompiledInstance&&) = delete;
    CompiledInstance& operator=(CompiledInstance const&) = delete;
    CompiledInstance& operator=(CompiledInstance&&) = delete;
    ~CompiledInstance() = default;

    /// Calls the function of code index @p codeIndex with @p arguments, as the embedder does
    /// (runtime::invokeChecked()).
    /// @return Its results; or how it ended abruptly.
    [[nodiscard]] std::expected<std::vector<runtime::Value>, runtime::Abrupt> call(
        uint32_t codeIndex, std::span<runtime::Value const> arguments);

    /// Invokes the function at @p address of the store with @p arguments, as the embedder does
    /// (runtime::invokeChecked()): a host function too.
    /// @return Its results; or how it ended abruptly.
    [[nodiscard]] std::expected<std::vector<runtime::Value>, runtime::Abrupt> invoke(
        runtime::FuncAddr address, std::span<runtime::Value const> arguments);

    /// Runs @p work within an outermost invocation whose budget is lent, as an entry lends it before the
    /// trampoline: what a test that calls a stub directly runs it in. The budget is settled after.
    void lent(std::function<void(compiler::ExecutionState&, compiler::InstanceContext const&)> const& work);

    /// The store.
    [[nodiscard]] runtime::Store& store() noexcept { return _store; }

    /// The execution state, as the last call left it.
    [[nodiscard]] compiler::ExecutionState const& state() const noexcept;

    /// The code.
    [[nodiscard]] CompiledModuleCode const& code() const noexcept { return *_code; }

    /// The instance's context.
    [[nodiscard]] compiler::InstanceContext const& context() const noexcept;

    /// The executor's record.
    [[nodiscard]] ExecutorRecord& record() noexcept;

    /// The executor.
    [[nodiscard]] CompiledExecutor& executor() noexcept { return *_executor; }

    /// The address of the function of code index @p codeIndex.
    [[nodiscard]] runtime::FuncAddr addressOf(uint32_t codeIndex) const;

    /// Calls the function of code index @p codeIndex through @p entry instead of the module's trampoline, in
    /// an outermost invocation: what a test that wraps the trampoline (sentinels in the callee-saved
    /// registers) runs.
    /// @return Its results; or how it ended abruptly.
    [[nodiscard]] std::expected<std::vector<runtime::Value>, runtime::Abrupt> callThrough(
        compiler::EntryTrampoline entry, uint32_t codeIndex, std::span<runtime::Value const> arguments);

  private:
    CompiledInstance(std::shared_ptr<validate::ValidatedModule const> module,
                     std::shared_ptr<CompiledModuleCode const> code,
                     InstanceOptions options);

    std::shared_ptr<validate::ValidatedModule const> _module;
    std::shared_ptr<CompiledModuleCode const> _code;
    InstanceOptions _options;
    ThreadStackBounds _threadBounds;
    runtime::HeapMemoryAllocator _allocator;
    runtime::Store _store;
    std::unique_ptr<CompiledExecutor> _executor;
    uint32_t _instance = 0;
};

/// What a test reaches of a CompiledExecutor's state, which it keeps to itself otherwise.
struct ExecutorInternals
{
    /// @p executor's execution state for @p store.
    [[nodiscard]] static compiler::ExecutionState& state(CompiledExecutor& executor, runtime::Store& store)
    {
        return executor.bindingOf(store).state;
    }

    /// @p executor's record for @p store.
    [[nodiscard]] static ExecutorRecord& record(CompiledExecutor& executor, runtime::Store& store)
    {
        return executor.bindingOf(store).record;
    }

    /// @p executor's poll gate for @p store.
    [[nodiscard]] static runtime::PollGate& gate(CompiledExecutor& executor, runtime::Store& store)
    {
        return executor.bindingOf(store).gate;
    }

    /// The context of the instance @p moduleInstance of @p store (Require: prepared).
    [[nodiscard]] static compiler::InstanceContext const& context(CompiledExecutor& executor,
                                                                  runtime::Store& store,
                                                                  uint32_t moduleInstance)
    {
        auto const& instances = executor.bindingOf(store).instances;
        Require(moduleInstance < instances.size() && instances[moduleInstance] != nullptr);
        return instances[moduleInstance]->context;
    }

    /// CompiledExecutor::invoke() through @p trampoline instead of the module's.
    [[nodiscard]] static std::expected<void, runtime::Abrupt> enterThrough(
        CompiledExecutor& executor,
        runtime::Store& store,
        runtime::FuncAddr function,
        std::span<runtime::Value const> args,
        std::span<runtime::Value> results,
        compiler::EntryTrampoline trampoline)
    {
        return executor.enter(store, function, args, results, trampoline);
    }
};

/// Calls @p entry's stub with @p state, @p context and @p words, one per operand it takes (Require).
[[nodiscard]] inline compiler::StubResult callStub(StubEntry const& entry,
                                                   compiler::ExecutionState& state,
                                                   compiler::InstanceContext const& context,
                                                   std::span<uint64_t const> words)
{
    Require(words.size() == operandCountOf(entry));
    auto const callWith =
        [&]<std::size_t Operands>(std::integral_constant<std::size_t, Operands> /*operands*/) {
            return [&]<std::size_t... Index>(std::index_sequence<Index...> /*indices*/) {
                return std::get<Operands>(entry)(&state, &context, words[Index]...);
            }(std::make_index_sequence<Operands> {});
        };
    switch (entry.index())
    {
        case 0: return callWith(std::integral_constant<std::size_t, 0> {});
        case 1: return callWith(std::integral_constant<std::size_t, 1> {});
        case 2: return callWith(std::integral_constant<std::size_t, 2> {});
        case 3: return callWith(std::integral_constant<std::size_t, 3> {});
        case 4: return callWith(std::integral_constant<std::size_t, 4> {});
        default: return callWith(std::integral_constant<std::size_t, 5> {});
    }
}

/// What the reference interpreter ended a call with, for comparing compiled code against it.
struct ReferenceRun
{
    std::expected<std::vector<runtime::Value>, runtime::Abrupt> outcome; ///< Its results, or how it ended.
    uint64_t steps = 0;                                                  ///< The steps it took.
};

/// Runs the function of code index @p codeIndex of @p module with @p arguments on the reference interpreter,
/// in a store set up as @p options says.
[[nodiscard]] ReferenceRun runReference(std::shared_ptr<validate::ValidatedModule const> const& module,
                                        uint32_t codeIndex,
                                        std::span<runtime::Value const> arguments,
                                        InstanceOptions const& options = {});

} // namespace isoline::execution::testing
