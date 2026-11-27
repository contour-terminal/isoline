// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <memory_resource>
#include <span>

#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/ModuleCode.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::fastinterpreter
{

/// The fewest slots an invocation's first slot chunk holds: what each nested invocation costs at least
/// (docs/fast-interpreter.md, "Calls, frames and the slot stack"). A frame that needs more gets a chunk of
/// its own size.
inline constexpr auto FirstChunkSlots = uint64_t { 256 };

/// The most slots a slot chunk grows to by doubling the one before it: each later chunk holds at least twice
/// its predecessor's slots, up to this, and at least its first frame's.
inline constexpr auto MaximumGrownChunkSlots = uint64_t { 65'536 };

/// The bytes a value the stack budget charges (StoreLimits::maxStackValues) takes in a frame's slots at the
/// most: two slots, for a vector or a reference; a number takes one.
inline constexpr auto SlotBytesPerValue = uint64_t { 2 * sizeof(Slot) };

/// Runs code lowered once, at load (lower()), through one dispatch: the fast interpreter.
///
/// It honours the embedding contract as the reference interpreter does, at the same points: it polls at
/// every PollSiteTable site the reference polls at, through a runtime::PollGate whose countdown it commits
/// to the store on every way out of running code; it enters every frame through one routine, which polls at
/// FunctionEntry, checks StoreLimits::maxCallDepth (one count across every invocation in progress) and
/// charges the frame's static cost (Store::chargeFrame()), which it gives back however the frame ends.
///
/// Calls never recurse on the host stack: a call pushes a frame record of the invocation and the same
/// dispatch runs on in the callee, a return pops it. A call within the module goes by code index; an
/// imported, indirect or referenced callee goes through the store, a table element or a reference checked
/// (and trapping) before the FunctionEntry poll. A host callee runs through runtime::callHostFunction(), the
/// one host-frame routine of every engine, the countdown committed before and reloaded after; a host that
/// invokes the guest again nests an invocation of its own, within the same depth count and step budget. A
/// tail call ends its caller's frame, its charge given back, before the callee enters.
///
/// A frame's slots live in a slot chunk of the invocation, so a frame of any size runs: a callee's frame
/// begins at its arguments where it fits in its caller's chunk, and starts the next chunk otherwise, its
/// arguments copied there once; a tail callee left alone in its chunk takes that chunk. Chunks grow from
/// FirstChunkSlots by doubling up to MaximumGrownChunkSlots, or to the frame's size. They come from an
/// injected memory resource, only once the frame's entry checks passed; per invocation they hold at most
/// 2 × A + min(2 × A, MaximumGrownChunkSlots) + FirstChunkSlots slots, A its active frames' slots. Its
/// declared locals start as their type's default: zero, or the null of their heap, which a concrete heap
/// names by the running instance's TypeId, never by an index baked into the shared code. Running out of host
/// memory for a frame's slots terminates, as running out of it does in runtime::invokeChecked().
///
/// An instance is not thread-safe: its depth counter belongs to one thread at a time. Threads that execute
/// concurrently each use an interpreter of their own.
class FastInterpreter final: public runtime::ICodeExecutor
{
  public:
    /// An interpreter running code lowered for @p dispatch, its slot chunks from the global heap.
    /// Precondition (checked): @p dispatch is available (availableDispatches()).
    /// @param dispatch The dispatch.
    explicit FastInterpreter(Dispatch dispatch);

    /// An interpreter running code lowered for @p dispatch, its slot chunks from @p slotMemory.
    /// Precondition (checked): @p dispatch is available (availableDispatches()).
    /// @param dispatch The dispatch.
    /// @param slotMemory What every invocation's slot chunks come from; must outlive the interpreter.
    FastInterpreter(Dispatch dispatch, std::pmr::memory_resource& slotMemory);

    /// The fast engine, whose instances hold their module's LoweredModule.
    /// @return ExecutionEngine::Fast.
    [[nodiscard]] runtime::ExecutionEngine engine() const noexcept override;

    /// The dispatch it runs code lowered for.
    /// @return The dispatch.
    [[nodiscard]] Dispatch dispatch() const noexcept;

    /// Invokes a function, as the spec's invocation does (4.5.5).
    ///
    /// A host function runs through runtime::callHostFunction() after the FunctionEntry poll, the embedder
    /// its caller (runtime::HostContext::callerInstance() is nullopt).
    ///
    /// Precondition (checked): an invocation is open on @p store (runtime::InvocationScope); @p function is a
    /// host function, or a module function whose instance holds a LoweredModule lowered for this
    /// interpreter's dispatch, as is every module function it reaches; @p args are of its parameter types and
    /// @p results has room for exactly its results. runtime::invokeChecked() checks the last two first.
    /// @param store The store holding it.
    /// @param function Its address.
    /// @param args Its arguments.
    /// @param results Receives its results.
    /// @return Nothing, or the trap it ended with.
    [[nodiscard]] std::expected<void, runtime::Abrupt> invoke(runtime::Store& store,
                                                              runtime::FuncAddr function,
                                                              std::span<runtime::Value const> args,
                                                              std::span<runtime::Value> results) override;

  private:
    Dispatch _dispatch;
    std::pmr::memory_resource* _slotMemory;

    /// The Wasm frames live across every invocation in progress: a host function that invokes Wasm again
    /// nests within the depth of the call that reached it.
    uint32_t _depth = 0;
};

} // namespace isoline::fastinterpreter
