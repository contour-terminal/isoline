// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "Observation.hpp"
#include "ProcessRunner.hpp"
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::differential
{

/// Where the oracle reads a module from: a file, which this seam writes (R15: test tools touch the file
/// system, the library does not).
class IModuleFiles
{
  public:
    virtual ~IModuleFiles() = default;

    /// A file holding @p module's bytes.
    /// @param module The module.
    /// @return The file's path, or why it could not be written.
    [[nodiscard]] virtual std::expected<std::filesystem::path, std::string> write(
        std::span<std::byte const> module) = 0;
};

/// Module files in a directory, each named after a digest of its bytes and written once: a later write of
/// the same module reuses the file.
class DirectoryModuleFiles final: public IModuleFiles
{
  public:
    /// Files in @p directory, which is created when it does not exist.
    /// @param directory The directory.
    explicit DirectoryModuleFiles(std::filesystem::path directory);

    /// @copydoc IModuleFiles::write
    [[nodiscard]] std::expected<std::filesystem::path, std::string> write(
        std::span<std::byte const> module) override;

  private:
    std::filesystem::path _directory;
    uint64_t _writes = 0; ///< The files written so far, which names each one's staging file.
};

/// A trap Wasmtime reported: its message, and the TrapCodes that agree with it (TrapMapping.hpp).
struct OracleTrap
{
    std::string message;                  ///< Wasmtime's message, after `wasm trap: `.
    std::vector<runtime::TrapCode> codes; ///< The codes that agree.
    [[nodiscard]] bool operator==(OracleTrap const&) const = default;
};

/// An exception Wasmtime reported escaping the call (`thrown Wasm exception`); it shows neither its tag nor
/// its payload.
struct OracleThrown
{
    [[nodiscard]] bool operator==(OracleThrown const&) const = default;
};

/// What Wasmtime made of a call: its results (a NaN as the canonical NaN, Wasmtime printing none's bits),
/// a trap, or an exception.
using OracleResult = std::variant<std::vector<runtime::Value>, OracleTrap, OracleThrown>;

/// Why the oracle has no answer.
enum class OracleErrorKind : uint8_t
{
    Ineligible,      ///< The module imports, or the export takes or returns a vector or a reference.
    Unrepresentable, ///< An argument is a NaN other than the canonical one, which Wasmtime's command line
                     ///< cannot pass.
    ModuleFile,      ///< The module's file could not be written.
    Process,         ///< Wasmtime could not be run.
    UnmappedTrap,    ///< Wasmtime trapped with a message no TrapMappingRows row has.
    Unparsed,        ///< Its output is neither results, nor a trap, nor an exception.
    Exhausted,       ///< Wasmtime's own timeout ended the call: its answer would not compare.
};

/// Why the oracle has no answer, and what it saw.
struct OracleError
{
    OracleErrorKind kind = OracleErrorKind::Process; ///< Why.
    std::string detail;                              ///< What it saw: the message, the output.
};

/// The flags the oracle runs Wasmtime with: the cut's features (scripts/tools.lock), the deterministic
/// relaxed-SIMD profile, canonical NaNs, and its timeout.
/// @param timeout How long Wasmtime may run one call.
/// @return The `run` arguments before `--invoke`.
[[nodiscard]] std::vector<std::string> wasmtimeFlags(std::chrono::seconds timeout);

/// Whether the oracle can run export @p type of @p module: the module imports nothing, and the export takes
/// i32, i64, f32 and f64 only (Wasmtime's command line passes no other) and returns numbers or vectors.
/// @param module The decoded module.
/// @param type The export's function type.
/// @return Whether it is eligible.
[[nodiscard]] bool isOracleEligible(binary::Module const& module, binary::FuncType const& type) noexcept;

/// Whether Isoline's outcome of a call agrees with the oracle's: equal results, where a NaN agrees with any
/// NaN (Wasmtime prints none's bits); a trap whose code is one the oracle's message maps to; or an exception
/// thrown out of the call.
/// @param oracle The oracle's result.
/// @param outcome Isoline's outcome.
/// @return Whether they agree.
[[nodiscard]] bool agreesWithOracle(OracleResult const& oracle, CallOutcome const& outcome) noexcept;

/// Wasmtime as an oracle: it runs one export of an import-free module whose exports take and return numbers,
/// with `wasmtime run <flags> --invoke <name> <module> <args>`, and maps its output to results, a trap
/// (through TrapMappingRows) or an exception. It compares results and traps only: Wasmtime's limits are not
/// Isoline's, so steps, polls and the state a call leaves are the differential runner's to compare.
class WasmtimeOracle
{
  public:
    /// How long Wasmtime may run one call, by default.
    static constexpr auto DefaultTimeout = std::chrono::seconds { 10 };

    /// An oracle running @p wasmtime through @p runner, on files @p files writes.
    /// @param wasmtime The pinned Wasmtime (scripts/fetch-tools.py --tool wasmtime).
    /// @param runner Runs it; must outlive the oracle.
    /// @param files Writes the modules it reads; must outlive the oracle.
    /// @param timeout How long it may run one call.
    WasmtimeOracle(std::filesystem::path wasmtime,
                   IProcessRunner& runner,
                   IModuleFiles& files,
                   std::chrono::seconds timeout = DefaultTimeout);

    /// Calls export @p name of @p module with @p args in a fresh instance.
    /// @param module The module's bytes.
    /// @param name The export.
    /// @param args The arguments.
    /// @return What Wasmtime made of the call, or why it has no answer.
    [[nodiscard]] std::expected<OracleResult, OracleError> call(std::span<std::byte const> module,
                                                                std::string_view name,
                                                                std::span<runtime::Value const> args) const;

  private:
    std::filesystem::path _wasmtime;
    IProcessRunner* _runner;
    IModuleFiles* _files;
    std::chrono::seconds _timeout;
};

} // namespace isoline::differential
