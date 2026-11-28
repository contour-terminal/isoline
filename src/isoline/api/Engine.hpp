// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

#include <isoline/Contracts.hpp>
#include <isoline/Features.hpp>
#include <isoline/api/EngineIdentity.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/runtime/Limits.hpp>

namespace isoline::api
{

/// The limits loading a module enforces: one on its size, and those decoding enforces.
///
/// They decide whether a module loads, not what its code is, so they are no part of EngineIdentity.
struct LoadLimits
{
    /// The most bytes a module may have; a larger one is refused (ModuleTooLarge) before a byte of it
    /// is decoded. 64 MiB by default.
    uint64_t maxModuleBytes = uint64_t { 64 } << 20;

    /// The implementation limits of decoding and validation.
    DecodeLimits decode {};

    /// Two sets of limits are equal when every limit is.
    bool operator==(LoadLimits const&) const = default;
};

/// Everything an Engine is: what it accepts, how it runs code, and the limits of what it loads and of
/// the stores it makes. Fixed at construction.
struct EngineConfig
{
    /// The proposals modules are decoded and validated with. It must be closed over every enabled
    /// feature's prerequisites (FeatureSet::checkPrerequisites()); FeatureSet::withPrerequisites()
    /// closes a set.
    FeatureSet features = FeatureSet::wasm3();

    /// The engine that runs modules' code.
    ExecutionEngine engine = ExecutionEngine::Reference;

    /// How that code computes what the specification leaves nondeterministic.
    ExecutionMode mode = ExecutionMode::Production;

    /// The limits loading a module enforces.
    LoadLimits load {};

    /// The caps of every store made for this engine, unless the store overrides them
    /// (StoreOptions::limits).
    StoreLimits store {};

    /// What a module this configuration loads depends on: the engine, the mode and the features, not
    /// the limits.
    /// @return The identity of an engine of this configuration.
    [[nodiscard]] EngineIdentity identity() const noexcept;
};

/// Why Engine::prepareThread() could not make the calling thread ready to run guest code.
enum class PrepareError : uint8_t
{
    Unavailable, ///< What the engine needs of a thread (a signal stack, a guard) could not be set up.
};

/// The last PrepareError enumerator; it must name whichever enumerator is listed last above. The C API's
/// error rows (c/detail/ErrorRows.hpp) hold one row per enumerator up to it.
inline constexpr auto LastPrepareError = PrepareError::Unavailable;

/// What an engine sets up on one thread to run its code there, implemented by the engines that need
/// any (a compiled engine's signal stack); its destructor releases it. Interpreters have none.
class IThreadResource
{
  public:
    virtual ~IThreadResource() = default;
};

/// What Engine::prepareThread() set up on the calling thread, owned by the embedder: the thread may
/// run the engine's guest code while it lives, and destroying it releases what it holds.
///
/// It is move-only, as what it holds belongs to one thread. Keep it on the thread it prepared, and
/// destroy it there after that thread's last call into guest code. A token of an interpreter holds
/// nothing.
///
/// Tokens nest. A thread may hold any number at once, of one engine and of several: each prepares the
/// thread on its own, and destroying one, in any order, leaves the thread prepared for every token
/// still alive. That is a contract for every engine whose tokens hold something (a compiled engine's
/// signal stack or guard): a resource set up by an earlier token, or shared by several, is released
/// only when the last token needing it goes. Contract.threadPreparationNests holds every engine to it.
class ThreadPreparation
{
  public:
    /// A token that holds nothing: what an engine that needs nothing of a thread returns.
    ThreadPreparation() = default;

    /// A token that owns @p resource, released when the token is destroyed.
    /// @param resource What the engine set up on the thread (checked: not null).
    explicit ThreadPreparation(std::unique_ptr<IThreadResource> resource) noexcept;

    /// Whether the token holds nothing, as an interpreter's does.
    /// @return Whether it owns no thread resource.
    [[nodiscard]] bool empty() const noexcept { return _resource == nullptr; }

  private:
    std::unique_ptr<IThreadResource> _resource; ///< What the engine set up; null for an interpreter's.
};

/// The entry point of the embedding API: it loads modules, under a configuration fixed at construction.
///
/// An engine holds no state but its configuration, so loading is const and may run on any number of
/// threads at once. The modules it loads stay valid after it is gone; each carries the engine's
/// identity, which decides which stores may instantiate it.
class Engine
{
  public:
    /// An engine of @p config.
    /// @param config Its configuration. Its feature set must be closed (checked: an unclosed set is a
    ///               programming error of the embedder; FeatureSet::withPrerequisites() closes one).
    explicit Engine(EngineConfig config) noexcept
        ISOLINE_PRE(config.features.checkPrerequisites().has_value());

    /// The configuration it was constructed with.
    /// @return The configuration.
    [[nodiscard]] EngineConfig const& config() const noexcept { return _config; }

    /// Loads a module: checks its size, then decodes and validates it with the engine's features and
    /// decode limits.
    ///
    /// Imports are described, never resolved: a module loads whatever it imports. Nothing is thrown:
    /// the host running out of memory is a HostMemoryExhausted refusal.
    /// @param bytes The module's bytes, from the magic number on; not referenced after the call.
    /// @return The module, or why it was refused: ModuleTooLarge before a byte is decoded, else the
    ///         first decode or validation error. isImplementationLimit() tells a refusal a raised
    ///         limit would lift, and requiredFeature() one a feature would.
    [[nodiscard]] std::expected<Module, LoadError> load(std::span<std::byte const> bytes) const noexcept;

    /// Makes the calling thread ready to run guest code of this engine. A thread calls it before its
    /// first call into guest code and keeps the token while it runs any.
    ///
    /// Interpreters need nothing of a thread: their token holds nothing and the call cannot fail. A
    /// compiling engine may set up a signal stack or a guard here, which the token owns.
    ///
    /// It may be called again on a thread already prepared, by this engine or another: tokens nest (see
    /// ThreadPreparation), and the thread stays prepared while any of them lives.
    /// @return The token, or why the thread could not be prepared.
    [[nodiscard]] std::expected<ThreadPreparation, PrepareError> prepareThread() const noexcept;

    /// The bytes of inaccessible guard every linear memory must have past its end for this engine's
    /// code, which runtime::IMemoryAllocator must provide.
    ///
    /// Interpreters check every access explicitly, so they need none.
    /// @return The guard size in bytes; 0 for explicit bounds checks.
    [[nodiscard]] uint64_t requiredGuardBytes() const noexcept;

  private:
    EngineConfig _config;
};

} // namespace isoline::api
