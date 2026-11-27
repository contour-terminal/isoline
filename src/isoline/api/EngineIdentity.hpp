// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include <isoline/Features.hpp>

namespace isoline::api
{

/// Which engine runs a module's code.
///
/// Every engine honours the whole embedding contract (limits, interrupts, poll sites, the frame
/// cost), so the choice changes speed, not behaviour.
enum class ExecutionEngine : uint8_t
{
    /// The reference interpreter: clarity and spec fidelity over speed. It is the oracle of
    /// differential fuzzing, never a production path.
    Reference,
};

/// How an engine computes what the WebAssembly specification leaves nondeterministic.
enum class ExecutionMode : uint8_t
{
    /// Whatever is fastest within the specification.
    Production,
    /// The same result on every engine and every host: canonical NaNs, the deterministic
    /// relaxed-SIMD profile, and the step limit counted alike everywhere.
    Deterministic,
};

/// What a loaded module's code depends on: the engine that runs it, its mode, and the features it was
/// decoded and validated with. Nothing else enters it: limits decide whether a module loads, not what
/// its code is.
///
/// An api::Module carries the identity of the engine that loaded it, and only a store of an engine
/// of the same identity may instantiate it. The identity is compared by value: two engines that agree
/// on it may share modules.
struct EngineIdentity
{
    ExecutionEngine engine = ExecutionEngine::Reference; ///< The engine that runs the code.
    ExecutionMode mode = ExecutionMode::Production;      ///< How it computes what the spec leaves open.
    FeatureSet features; ///< The proposals the module was decoded and validated with.

    /// Two identities are equal when engine, mode and features are.
    bool operator==(EngineIdentity const&) const = default;
};

} // namespace isoline::api
