// SPDX-License-Identifier: Apache-2.0
#pragma once

// Compiled code run without the Compiled engine, for the execution suites and the compiler's executed tests
// (P5 Tasks 14 and 15): compileOne() takes a module through every stage (translation, the passes, selection,
// register allocation, encoding) and seals its functions behind the entry trampoline; CompiledInstance
// instantiates it in a store of its own and calls a function through the trampoline as the executor will,
// lending the budget, setting the native stack limit and settling after. Test support, linked into no shipped
// binary.

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
#include <isoline/execution/RuntimeStubs.hpp>
#include <isoline/execution/StackBounds.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
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

/// A module's code compiled and sealed: every function of its code section, each function's entry adapter,
/// and the module's entry trampoline.
struct CompiledModule
{
    std::unique_ptr<ISealedCode> sealed; ///< The code.
    std::vector<uint64_t> functions;     ///< Per code index, its function's offset.
    std::vector<uint64_t> adapters;      ///< Per code index, its entry adapter's offset.
    uint64_t trampoline = 0;             ///< The entry trampoline's offset.

    /// The trampoline, as the host calls it.
    [[nodiscard]] compiler::EntryTrampoline entry() const noexcept;

    /// The address of what lies at @p offset of the code.
    [[nodiscard]] void const* at(uint64_t offset) const noexcept;
};

/// Compiles @p module for @p config: translates, optimizes, selects, allocates and encodes every function of
/// its code section (all of them, so that calls between them resolve), emits an entry adapter per function
/// and the trampoline, resolves, and seals the code through @p memory.
/// @return The code; or the first compile error.
[[nodiscard]] std::expected<CompiledModule, compiler::CompileError> compileOne(
    validate::ValidatedModule const& module, compiler::CompileConfig const& config, ICodeMemory& memory);

/// Whether a store is interrupted before its first call.
enum class Interruption : uint8_t
{
    None,    ///< Not interrupted.
    Pending, ///< Interrupted once set up, so that the first poll of every call ends it.
};

/// How a CompiledInstance's store is set up.
struct InstanceOptions
{
    runtime::StoreLimits limits {};             ///< The store's limits.
    runtime::IPollObserver* observer = nullptr; ///< Its poll observer; none when null.
    /// The native stack an invocation is granted; none for requiredNativeStack(limits).
    std::optional<uint64_t> nativeStackBytes {};
    /// Adds the imports to the store and returns them, in import order; none when null.
    std::function<std::vector<runtime::ExternValue>(runtime::Store&)> imports {};
    Interruption interruption = Interruption::None; ///< Whether the store is interrupted once set up.
    /// Where the calling thread's stack ends; the thread's own (ThreadStackBounds) when null. Referenced, so
    /// it must outlive the instance.
    IStackBounds const* bounds = nullptr;
};

/// A compiled module instantiated in a store of its own, its functions called through the trampoline.
class CompiledInstance
{
  public:
    /// Compiles @p module for @p config and instantiates it (the reference interpreter runs nothing: a module
    /// with a start function is not for this harness), binding its dispatch rows to its compiled functions.
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

    /// Calls the function of code index @p codeIndex with @p arguments, an outermost invocation.
    /// @return Its results; or how it ended abruptly: Trap{CallStackExhausted} before any poll when the
    /// thread's
    ///         stack bounds are unknown (StackBoundsError), as no native limit can be set (T15's executor
    ///         refuses with an error of its own there).
    [[nodiscard]] std::expected<std::vector<runtime::Value>, runtime::Abrupt> call(
        uint32_t codeIndex, std::span<runtime::Value const> arguments);

    /// Runs @p work within an outermost invocation whose budget is lent, as call() lends it before the
    /// trampoline: what a test that calls a stub directly runs it in. The budget is settled after.
    void lent(std::function<void(compiler::ExecutionState&, compiler::InstanceContext const&)> const& work);

    /// The store.
    [[nodiscard]] runtime::Store& store() noexcept { return _store; }

    /// The execution state, as the last call left it.
    [[nodiscard]] compiler::ExecutionState const& state() const noexcept { return _state; }

    /// The code.
    [[nodiscard]] CompiledModule const& code() const noexcept { return _code; }

    /// The instance's context.
    [[nodiscard]] compiler::InstanceContext const& context() const noexcept { return _context; }

    /// The executor's record.
    [[nodiscard]] ExecutorRecord& record() noexcept { return _record; }

    /// Calls the function of code index @p codeIndex through @p entry instead of the module's trampoline:
    /// what a test that wraps the trampoline (sentinels in the callee-saved registers) runs.
    /// @return Its results; or how it ended abruptly.
    [[nodiscard]] std::expected<std::vector<runtime::Value>, runtime::Abrupt> callThrough(
        compiler::EntryTrampoline entry, uint32_t codeIndex, std::span<runtime::Value const> arguments);

  private:
    CompiledInstance(std::shared_ptr<validate::ValidatedModule const> module,
                     CompiledModule code,
                     InstanceOptions options);

    std::shared_ptr<validate::ValidatedModule const> _module;
    CompiledModule _code;
    InstanceOptions _options;
    runtime::HeapMemoryAllocator _allocator;
    runtime::Store _store;
    interpreter::ReferenceInterpreter _interpreter;
    runtime::PollGate _gate;
    ThreadStackBounds _bounds;
    uint32_t _instance = 0;
    std::vector<runtime::MemoryView const*> _memories;
    std::vector<runtime::TableView const*> _tables;
    std::vector<runtime::RawValue*> _globals;
    std::vector<runtime::RawValue> _hostScratch;
    compiler::InstanceContext _context {};
    ExecutorRecord _record {};
    compiler::ExecutionState _state {};
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
