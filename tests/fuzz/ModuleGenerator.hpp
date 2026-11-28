// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <isoline/Features.hpp>

namespace isoline::fuzz
{

/// The caps on what generateModule() builds.
struct GeneratorLimits
{
    uint32_t maxFunctions = 8;          ///< The most functions the module defines; its imports aside.
    uint32_t maxBodyInstructions = 400; ///< The most instructions of a body, operands and ends counted.
    uint32_t maxNesting = 16;           ///< The deepest nesting of blocks, loops, ifs and try_tables.
};

/// The features every generated module validates under: WebAssembly 3.0, and the proposals Isoline runs
/// beyond it whose instructions or types a module states: wide arithmetic and custom page sizes.
inline constexpr auto GeneratorFeatures =
    FeatureSet::wasm3().with(Feature::WideArithmetic).with(Feature::CustomPageSizes);

/// Whom a generated module is for.
enum class GeneratorMode : uint8_t
{
    /// The differential harness: it imports HostImports, which its RecordingHost binds, and its functions
    /// take and return every value type.
    Harness,
    /// The Wasmtime oracle as well: it imports nothing, and its functions take numbers and return numbers or
    /// vectors, which Wasmtime's command line passes (WasmtimeOracle.hpp's eligibility). Reference and
    /// interpreter share their numeric kernels, so only the oracle checks those.
    Oracle,
};

/// A function every generated module of GeneratorMode::Harness imports, as function indices 0 and 1, from the
/// differential harness's RecordingHost: `host` derives its results from its arguments, `reenter` re-enters
/// the guest at every call.
struct HostImport
{
    std::string_view module; ///< The import's module name.
    std::string_view name;   ///< Its field name.
};

/// The host imports, in import order.
inline constexpr auto HostImports = std::to_array<HostImport>({
    { .module = "env", .name = "host" },
    { .module = "env", .name = "reenter" },
});

/// The most instructions one function's body holds under @p limits: its budget, or what closing an empty
/// body takes, when that is more.
/// @param limits The caps.
/// @return The instructions, operands and ends counted.
[[nodiscard]] uint64_t maxFunctionInstructions(GeneratorLimits const& limits) noexcept;

/// The most bytes generateModule() returns under @p limits, whatever the entropy: the fixed sections, and per
/// function its locals and maxFunctionInstructions() instructions of the longest encoding one may take.
/// @param limits The caps.
/// @return The bytes.
[[nodiscard]] uint64_t maxModuleBytes(GeneratorLimits const& limits) noexcept;

/// Builds a module that validates under GeneratorFeatures, driven by the fuzzer's bytes.
///
/// Instructions come from OpcodeTable's OpcodeSignature rows, the memory accesses of MemoryAccessTable, and a
/// table of special rows (control, calls, memory, tables, references, exceptions); operands an instruction
/// does not find on the stack are made by constants and reads of locals and globals, and every block is
/// closed with exactly its results. A loop tests and decrements its counter at its head, so every branch to
/// it, from a br, br_if, br_table, br_on_null or catch clause, is bounded. A table of program shapes adds
/// functions that reach the hard edges no random body reaches: an unwind past UnwindPollItems labels, and
/// more exceptions than the store holds, which sweeps. The module imports per @p mode, and defines two
/// tables, three memories (the second 64-bit, the third of 1-byte pages), a global of each value type, two
/// tags, an active and a passive segment of each kind, and exports every function it defines as `f<index>`.
/// The same entropy builds the same module; when the entropy runs out, every open block is closed and the
/// module ends there, so an empty entropy builds a module of one empty function. Termination is not
/// guaranteed: loops count down, but calls may recurse, so the runner's maxSteps bounds it.
/// @param entropy The fuzzer's bytes.
/// @param limits The caps.
/// @param mode Whom the module is for.
/// @return The module's bytes, at most maxModuleBytes(@p limits).
[[nodiscard]] std::vector<std::byte> generateModule(std::span<std::byte const> entropy,
                                                    GeneratorLimits const& limits,
                                                    GeneratorMode mode);

/// generateModule() for the differential harness (GeneratorMode::Harness).
/// @param entropy The fuzzer's bytes.
/// @param limits The caps.
/// @return The module's bytes.
[[nodiscard]] std::vector<std::byte> generateModule(std::span<std::byte const> entropy,
                                                    GeneratorLimits const& limits);

} // namespace isoline::fuzz
