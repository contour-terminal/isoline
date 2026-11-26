// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <isoline/Features.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/ModuleCode.hpp>

namespace isoline::api
{

/// Which engine runs a module's code: the runtime's own vocabulary (runtime/ModuleCode.hpp), which the
/// executors and the instances share.
using runtime::ExecutionEngine;

/// How an engine computes what the WebAssembly specification leaves nondeterministic
/// (runtime/ExecutionMode.hpp).
using runtime::ExecutionMode;

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
