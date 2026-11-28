// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace isoline::runtime
{

/// Which engine runs a module's code.
///
/// Every engine honours the whole embedding contract (limits, interrupts, poll sites, the frame
/// cost), so the choice changes speed, not behaviour.
enum class ExecutionEngine : uint8_t
{
    /// The reference interpreter: clarity and spec fidelity over speed. It is the oracle of
    /// differential fuzzing, never a production path. It runs the validated module itself, so its
    /// instances hold no ModuleCode.
    Reference,
    /// The interpreter, which runs code lowered from the validated module once, at load.
    Interpreter,
};

/// The code an engine made of a validated module, for its executor (runtime::ICodeExecutor) to run.
///
/// Made once per module and shared by every instance of it (ModuleInstance::code), in any store, so it
/// is immutable. Each engine derives its own; an executor runs only code of its own engine, which
/// instantiate() checks.
class ModuleCode
{
  public:
    virtual ~ModuleCode() = default;

    /// The engine whose code this is.
    /// @return The engine.
    [[nodiscard]] virtual ExecutionEngine engine() const noexcept = 0;
};

} // namespace isoline::runtime
