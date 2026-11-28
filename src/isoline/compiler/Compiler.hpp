// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <memory_resource>
#include <span>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/CompiledCode.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/Target.hpp>
#include <isoline/compiler/optimization/PassTable.hpp>
#include <isoline/compiler/translation/Translator.hpp>
#include <isoline/runtime/CountingArena.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::compiler
{

/// The compile memory a function reserves per decoded body unit (binary::bodyUnits(): each unit takes at
/// least one byte of the body) before any of its work: its bound, charged on the compile's arena so that a
/// module past the budget is refused before its peak (published; Task 18 measures it against hostile bodies).
inline constexpr uint64_t CompileBytesPerBodyByte = 256;

/// The compile memory a function reserves beyond its per-unit share: what any function takes, however small
/// its body (published).
inline constexpr uint64_t CompileFunctionOverheadBytes = uint64_t { 64 } * 1024;

/// The bound a function of @p units decoded body units reserves: CompileBytesPerBodyByte × units +
/// CompileFunctionOverheadBytes, saturated at the largest uint64_t.
/// @param units The function's body units.
/// @return The bytes.
[[nodiscard]] uint64_t compileReservationOf(uint64_t units) noexcept;

/// The cap on a module's code: max(artifactBytesFloor, artifactBytesPerModuleByte × @p sourceBytes), and
/// never past what a 32-bit code offset addresses (CompiledCode's offsets).
/// @param limits The caps.
/// @param sourceBytes The module's size (validate::ValidatedModule::sourceBytes()).
/// @return The bytes.
[[nodiscard]] uint64_t artifactCapOf(CompileLimits const& limits, uint64_t sourceBytes) noexcept;

/// What one function of a module is selected with: everything a compile shares across its functions.
struct FunctionInput
{
    validate::ValidatedModule const& module;      ///< The module.
    translation::ModuleIndex const& index;        ///< Its index, built once per module.
    CompileConfig const& config;                  ///< How it is compiled.
    machine::ITarget const& target;               ///< What for.
    CompileCancellation const& cancellation;      ///< Polled throughout.
    std::span<uint64_t const> memoryMinimumBytes; ///< optimization::memoryMinimumBytesOf()'s.
    optimization::Verification verification;      ///< When the passes verify.
    uint32_t importedFunctions = 0;               ///< The functions the module imports.
    uint32_t codeIndex = 0;                       ///< The function, by its code-section index.
};

/// Takes one function through translation, the passes and instruction selection: what Compiler::compile()
/// runs for it before register allocation, and what a test runs to check an allocation against the function
/// as selected (registerallocation::testing::checkAssignment()).
/// @param input The function and its module's shared state.
/// @param memory What every temporary allocates from.
/// @return The selected machine function; or the first compile error.
[[nodiscard]] std::expected<machine::MachineFunction, CompileError> selectFunction(
    FunctionInput const& input, std::pmr::memory_resource& memory);

/// Compiles validated modules into CompiledCode for one target and configuration.
///
/// A compile runs on the calling thread, inside a runtime::FloatEnvironmentScope (constant folding uses host
/// float arithmetic, so the host's rounding mode or flush-to-zero never reaches the code, I9). It refuses a
/// module that uses a deferred feature before any function (checkModule()), then takes each function in code
/// order: cancellation polled, its body size checked (FunctionTooLarge), its bound reserved on the arena
/// (compileReservationOf()), translated, optimized (verified after each pass in a debug build, once at the
/// end in a release one), selected, allocated and encoded; the artifact's cap is checked after each function,
/// so a module whose code is too large is refused at the first function past it, not after the last.
///
/// Every allocation of a compile goes through the injected arena, so its budget bounds the whole compile's
/// peak: one function's temporaries plus the code built so far. A refusal of the arena is a CompileError
/// (BudgetExceeded or HostMemoryExhausted) naming the function it happened in; nothing of the compile stays
/// charged on the arena afterwards, however it ended.
class Compiler
{
  public:
    /// A compiler for @p config's target, which @p target generates code for.
    /// Precondition (checked): @p target's description is @p config's.
    /// @param config How every module is compiled.
    /// @param target The code generator; must outlive the compiler.
    /// @param arena What every allocation of a compile draws on; must outlive the compiler.
    /// @param cancellation Polled throughout a compile; must outlive the compiler.
    Compiler(CompileConfig config,
             machine::ITarget const& target,
             runtime::ICountingArena& arena,
             CompileCancellation const& cancellation);

    /// Compiles @p module.
    /// @param module The module.
    /// @return Its code; or the first compile error.
    [[nodiscard]] std::expected<CompiledCode, CompileError> compile(
        validate::ValidatedModule const& module) const;

    /// How it compiles.
    [[nodiscard]] CompileConfig const& config() const noexcept { return _config; }

  private:
    CompileConfig _config;
    machine::ITarget const* _target;
    runtime::ICountingArena* _arena;
    CompileCancellation const* _cancellation;
};

} // namespace isoline::compiler
