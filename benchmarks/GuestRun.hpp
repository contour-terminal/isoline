// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/api/Engine.hpp>

/// The benchmarks' half that is not a composition root: one guest run on an Isoline engine, timed by an
/// injected clock, and the guest lock that pins what is run. BenchmarkRunner's main reads the real clock and
/// the files; the tests hand in a manual clock and the committed guests.
namespace isoline::benchmarks
{

/// What to run: a guest's bytes, the iterations its run() repeats its kernel, and how often run() is called.
struct GuestRunRequest
{
    std::span<std::byte const> module; ///< The guest's bytes: it imports nothing and exports run(i32) -> i64.
    uint32_t iterations = 0;           ///< run()'s argument; 0 does the guest's setup alone.
    /// How many times run() is called on the one instance: 1 for a benchmark, many for the host→guest call
    /// round trip (with iterations 0, each call is the entry and the setup).
    uint32_t invocations = 1;
};

/// What a run computed and how long its phases took by the injected clock.
struct GuestRunResult
{
    uint64_t checksum = 0;          ///< The last run()'s result, as its bits.
    core::SteadyDuration load {};   ///< Loading (decoding, validation, lowering) and instantiation.
    core::SteadyDuration invoke {}; ///< Every run() call together.
};

/// Why a guest did not run to its checksum.
enum class GuestRunError : uint8_t
{
    LoadFailed,        ///< The engine refused the module.
    InstantiateFailed, ///< Instantiating it failed.
    NoRunExport,       ///< It exports no function named run.
    CallFailed,        ///< run() trapped, or did not take an i32 and return an i64.
};

/// A failed run: the step that failed, and what the engine said.
struct GuestRunFailure
{
    GuestRunError error = GuestRunError::LoadFailed; ///< The step.
    std::string detail;                              ///< The engine's description of the failure.
};

/// The name of @p error, as reports print it.
/// @param error The error.
/// @return Its name: load, instantiate, export or call.
[[nodiscard]] std::string_view nameOf(GuestRunError error) noexcept;

/// Loads, instantiates and runs a guest on an engine of @p config.
/// @param config The engine: its execution engine, dispatch and features.
/// @param request The guest and how to call it.
/// @param clock Times the load and the calls.
/// @return The checksum and the phases' times, or the step that failed.
[[nodiscard]] std::expected<GuestRunResult, GuestRunFailure> runGuest(api::EngineConfig const& config,
                                                                      GuestRunRequest const& request,
                                                                      core::IClock const& clock);

/// One guest of benchmarks/guests.lock: its file, its pinned SHA-256, and the features it was built with.
struct GuestRow
{
    std::string file;     ///< Its file name in benchmarks/guests, e.g. shootout-fib2.wasm.
    std::string sha256;   ///< The SHA-256 of its bytes, in lowercase hex.
    std::string features; ///< The wasm-tools feature list it validates under, as build-guests.sh wrote it.
};

/// Why a guest lock was refused.
struct GuestLockError
{
    std::size_t line = 0; ///< The 1-based line at fault.
    std::string reason;   ///< What is wrong with it.
};

/// Parses benchmarks/guests.lock: `<guest>.wasm = <sha256>` and `<guest>.wasm.features = <list>` rows;
/// '#' starts a comment.
/// @param text The lock's contents.
/// @return Its guests in the order of their hash rows, each with its features, or the first line at fault:
/// one
///         that is no row, a hash that is not 64 hex digits, or a guest without both rows.
[[nodiscard]] std::expected<std::vector<GuestRow>, GuestLockError> parseGuestLock(std::string_view text);

/// Whether a guest was built with SIMD: its feature list names simd, and Contour's WAMR runs no SIMD.
/// @param row The guest.
/// @return Whether its features include simd.
[[nodiscard]] bool usesSimd(GuestRow const& row) noexcept;

} // namespace isoline::benchmarks
