// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::runtime
{

/// Runs one host frame, after the FunctionEntry poll its caller took: the one path from an executor
/// into IHostFunction::call(), so that every engine charges, escapes, scopes, checks and polls a host
/// call alike.
///
/// In order, it charges the frame's static cost (hostFrameCost()) to the store, escapes every argument
/// (Store::markEscaped(): an exnref handed to a host may be kept past its frame), fills @p results with
/// the default of each result type, calls the host function under a HostCallScope, checks what it ended
/// with (checkHostOutcome()), gives the charge back, and polls at PollSite::HostReturn whatever the host
/// returned: an interrupt there replaces the outcome, so a host that absorbed a nested Interrupted cannot
/// extend the call. A Thrown it returns is an exception the store holds, which the executor throws on in
/// the Wasm caller; unwinding it is the executor's.
///
/// Precondition (checked): @p function is a host function; @p args and @p results are exactly as wide as
/// its params and results.
/// @param store The store it runs in.
/// @param executor What executes the guest code the host may invoke through its HostContext.
/// @param function The host function.
/// @param caller The instance whose code called it; nullopt when the embedder did.
/// @param args Its arguments, of its params' types.
/// @param results Receives its results.
/// @return Nothing, @p results written; or how it ended abruptly: Trap{CallStackExhausted} when its frame
///         does not fit (nothing else done), the trap of the HostReturn poll, or the checked outcome of the
///         host function.
[[nodiscard]] std::expected<void, Abrupt> callHostFunction(Store& store,
                                                           ICodeExecutor& executor,
                                                           FunctionInstance const& function,
                                                           std::optional<uint32_t> caller,
                                                           std::span<Value const> args,
                                                           std::span<Value> results) noexcept;

} // namespace isoline::runtime
