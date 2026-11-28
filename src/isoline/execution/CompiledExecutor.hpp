// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/compiler/machine/FrameRule.hpp>
#include <isoline/execution/CompiledModuleCode.hpp>
#include <isoline/execution/RuntimeStubs.hpp>
#include <isoline/execution/StackBounds.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/PollGate.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::execution
{

namespace testing
{
    struct ExecutorInternals; // execution/testing/CompileOne.hpp
} // namespace testing

/// How a CompiledExecutor runs code. The mode has no default: every caller names it, as CompileConfig's.
struct CompiledExecutorConfig
{
    /// A configuration for code of @p mode.
    /// @param mode The mode of the code it runs.
    /// @param nativeStackBytes The native stack an outermost invocation is granted.
    explicit CompiledExecutorConfig(runtime::ExecutionMode mode,
                                    uint64_t nativeStackBytes = uint64_t { 1 } << 20) noexcept:
        mode { mode }, nativeStackBytes { nativeStackBytes }
    {
    }

    /// The execution mode of the code it runs (CompiledModuleCode::mode()), and of its native stack: in
    /// Deterministic mode the grant must cover requiredNativeStack(), so that a store's static limits always
    /// fire before the native one.
    runtime::ExecutionMode mode;
    /// The native stack an outermost invocation may take below its entry (P5 Decision "Native stack limit
    /// from the real stack"): 1 MiB unless the embedder grants another.
    uint64_t nativeStackBytes;
};

/// Why a CompiledExecutor could not be created.
enum class ExecutorError : uint8_t
{
    /// Deterministic mode, and a grant below requiredNativeStack() of the limits: the native limit could fire
    /// before a static one.
    GrantBelowRequired,
    /// The creating thread's stack bounds are unknown (StackBoundsError::Unavailable): no native stack limit
    /// can be set, so no compiled code could run safely.
    StackBoundsUnavailable,
    /// No compiled backend runs on this host: its architecture has no frame rule row (HostFrameRuleTable).
    NoBackendForHost,
};

/// The frame rule of the compiled backend of this host's architecture.
/// @return The rule; nullopt where no backend runs (HostFrameRuleTable).
[[nodiscard]] std::optional<compiler::machine::FrameRule> hostFrameRule() noexcept;

/// Runs compiled code (CompiledModuleCode) behind runtime::ICodeExecutor, with the published contract of
/// every engine: polls at every PollSiteTable site in row order, the callee's entry as FunctionEntry poll →
/// depth check (one count over every invocation in progress) → stack charge, and the same traps, steps and
/// stack values as the reference interpreter.
///
/// It serves one store: the first it prepares an instance in or invokes in (checked), and keeps that store's
/// ExecutionState, ExecutorRecord and PollGate, shared by the invocations host functions nest
/// (P5 Decision "Per-invocation state"). Each invocation saves {entryStackPointer, callDepthLeft, the stack
/// values left at its entry, pending} on its own C++ frame and restores them on both ways out of the
/// trampoline, so that a nested trap gives back the depth and stack its dropped frames held, and the outer
/// invocation's trap exit finds its own entry stack pointer. Only the outermost invocation starts the depth
/// count and sets the native stack limit, max(entry − grant, lowest + StubReserve); every entry, nested or
/// not, first checks that the trampoline, its adapter's area and the stub reserve fit above that limit, and
/// refuses the call with Trap{CallStackExhausted} before any poll when they do not.
///
/// The embedder's callbacks reached from generated code run on the stack it left: the poll observer
/// (PollSlow) and the memory allocator a memory.grow calls (MemoryGrow) run within the stub reserve
/// (StubReserve; StubStackBudgetBytes for the stub's own frames), and a host function gets whatever stack is
/// left beyond its caller's frames (docs/embedding.md).
///
/// It registers no exception roots: the compiler refuses exnref and tags (P5b), so no compiled frame holds
/// one, and a store sweeps only with no host call in progress.
///
/// An executor is not thread-safe: its store's invocations run on one thread at a time.
class CompiledExecutor final: public runtime::ICodeExecutor
{
  public:
    /// An executor for a store of @p limits.
    /// @param config How it runs code.
    /// @param limits The limits of the store it serves.
    /// @param stackBounds Where the calling thread's stack ends; must outlive the executor.
    /// @return The executor; GrantBelowRequired in Deterministic mode with a grant below
    ///         requiredNativeStack(@p limits); StackBoundsUnavailable when the creating thread's bounds are
    ///         unknown; NoBackendForHost on a host no compiled backend runs on.
    [[nodiscard]] static std::expected<std::unique_ptr<CompiledExecutor>, ExecutorError> create(
        CompiledExecutorConfig config, runtime::StoreLimits const& limits, IStackBounds const& stackBounds);

    CompiledExecutor(CompiledExecutor const&) = delete;
    CompiledExecutor(CompiledExecutor&&) = delete;
    CompiledExecutor& operator=(CompiledExecutor const&) = delete;
    CompiledExecutor& operator=(CompiledExecutor&&) = delete;
    ~CompiledExecutor() override;

    /// The compiled engine, whose instances hold CompiledModuleCode.
    /// @return ExecutionEngine::Compiled.
    [[nodiscard]] runtime::ExecutionEngine engine() const noexcept override;

    /// Invokes a function, within an invocation already open (checked).
    ///
    /// A host function polls at FunctionEntry, then runs through runtime::callHostFunction(), the embedder
    /// its caller. A module function's arguments go into RawValue cells, the store's budget is lent, the
    /// module's trampoline calls the function's adapter, and the budget is settled again however it ended.
    ///
    /// Precondition (checked): an invocation is open on @p store; @p store is the one it serves; @p function
    /// is a host function or a function of an instance it prepared; @p args are of its parameter types and
    /// @p results has room for exactly its results.
    /// @param store The store holding it.
    /// @param function Its address.
    /// @param args Its arguments.
    /// @param results Receives its results.
    /// @return Nothing, or how it ended abruptly; Trap{CallStackExhausted} before any poll when the entry
    /// does
    ///         not fit above the native stack limit, when the calling thread's stack bounds are unknown, or,
    ///         in Deterministic mode, when an outermost invocation's thread has less than the grant left.
    [[nodiscard]] std::expected<void, runtime::Abrupt> invoke(runtime::Store& store,
                                                              runtime::FuncAddr function,
                                                              std::span<runtime::Value const> args,
                                                              std::span<runtime::Value> results) override;

    /// Prepares an instance of compiled code: builds its InstanceContext and binds its own functions'
    /// dispatch rows to their code and that context.
    /// Precondition (checked): @p store is the one it serves, and keeps dispatch rows; the instance's code is
    /// CompiledModuleCode of this executor's mode.
    /// @param store The store holding the instance.
    /// @param moduleInstance The instance's address.
    void prepareInstance(runtime::Store& store, uint32_t moduleInstance) noexcept override;

    /// How it runs code.
    [[nodiscard]] CompiledExecutorConfig const& config() const noexcept { return _config; }

  private:
    friend struct testing::ExecutorInternals;

    /// One instance as its code runs against it.
    struct InstanceBinding
    {
        std::shared_ptr<CompiledModuleCode const> code;   ///< Its code.
        std::vector<runtime::MemoryView const*> memories; ///< Every memory's view, by memory index.
        std::vector<runtime::TableView const*> tables;    ///< Every table's view, by table index.
        std::vector<runtime::RawValue*> globals;          ///< Every global's cell, by global index.
        compiler::InstanceContext context {};             ///< What r14 points at.
    };

    /// The store it serves, as generated code sees it.
    struct StoreBinding
    {
        StoreBinding(runtime::Store& store, CompiledExecutor& executor);

        runtime::Store* store;
        runtime::PollGate gate;
        ExecutorRecord record;
        compiler::ExecutionState state {};
        /// Per instance address, its binding; null for an instance it did not prepare.
        std::vector<std::unique_ptr<InstanceBinding>> instances;
        uint32_t invocations = 0; ///< Its invocations of module functions in progress, nested ones too.
    };

    CompiledExecutor(CompiledExecutorConfig config,
                     compiler::machine::FrameRule rule,
                     IStackBounds const& stackBounds) noexcept;

    /// The binding of @p store, made the first time (Require: the store it serves).
    [[nodiscard]] StoreBinding& bindingOf(runtime::Store& store);

    /// invoke() through @p trampoline, or the function's module's own when null.
    [[nodiscard]] std::expected<void, runtime::Abrupt> enter(runtime::Store& store,
                                                             runtime::FuncAddr function,
                                                             std::span<runtime::Value const> args,
                                                             std::span<runtime::Value> results,
                                                             compiler::EntryTrampoline trampoline);

    CompiledExecutorConfig _config;
    compiler::machine::FrameRule _rule;
    IStackBounds const* _stackBounds;
    std::unique_ptr<StoreBinding> _binding;
};

} // namespace isoline::execution
