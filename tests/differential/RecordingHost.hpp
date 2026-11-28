// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Store.hpp>

namespace isoline::differential
{

/// Why a module's imports cannot be bound alike in every store: an import whose type names a concrete
/// heap (a store's TypeId, which the harness does not intern), or a non-nullable reference, which no
/// value the host makes up satisfies. Such a module is not compared.
struct UnsupportedImport
{
    uint32_t importIndex = 0; ///< The import, by its index in the import section.
};

/// Why a store refused an import's instance: past a limit, or an allocation refused.
using ImportRefusal = std::variant<runtime::LimitExceeded, runtime::AllocationFailure>;

/// Whether every import of @p module can be bound by a RecordingHost.
/// @param module The decoded module.
/// @return Nothing, or the first import that cannot.
[[nodiscard]] std::expected<void, UnsupportedImport> checkImports(binary::Module const& module);

/// The re-entries the host functions make between two takeTrace()s, at most: a guest that calls a
/// re-entering import k times per entry would otherwise fan out to k^maxNestedInvocations host calls, each
/// costing a guest step or two but microseconds of host work, which under ASan ran a 100'000-step budget
/// past the fuzz smoke's 10-second timeout. Depth-first, a chain of re-entries still reaches the
/// nested-invocation limit (8) long before the budget runs out.
inline constexpr auto MaxReentriesPerCall = uint32_t { 256 };

/// What a RecordingHost's functions share: the trace they append to, and the re-entries they may still
/// make before the next takeTrace() (MaxReentriesPerCall).
struct HostLog
{
    std::vector<std::string> trace;               ///< The calls since the last takeTrace(), in order.
    uint32_t reentriesLeft = MaxReentriesPerCall; ///< The re-entries left; at 0 one is recorded and skipped.
};

/// The host every participant's store binds a module's imports to, the same in each, so that what the
/// guest does through its imports compares.
///
/// A function import is a host function whose results are derived from its arguments and the import's
/// index alone, and which records each call (trace()): `<import>(<arguments>) -> <results>`. One call in 64,
/// chosen by its arguments, fails with HostFailureKind::Embedder instead, so that a host failure unwinds
/// the guest too. One in 16 re-enters the guest first, calling an export of the caller instance with
/// derived arguments and swallowing how it ended (`<import> reenters <export>(<arguments>): <outcome>`); an
/// import named `reenter` does so at every call, so that a guest recursing through it reaches the store's
/// nested-invocation limit, and one named `reenterDeep` calls the export `deep` at every call, so that the
/// call depth runs out across nested invocations. One in 4 writes a derived byte into the caller's memory 0
/// (`<import> writes <byte> at <address>`), so that the guest reads what a host wrote. Memory, table, global
/// and tag imports are fresh instances of the import's type, defined once per store: the memory and table at
/// their minimum size, the table full of nulls, the global a value derived from the import's index.
class RecordingHost
{
  public:
    RecordingHost();

    /// Binds @p module's imports in @p store, in import order. Precondition: checkImports() accepts it.
    /// @param store The store.
    /// @param module The decoded module.
    /// @return The external values, or why the store refused one.
    [[nodiscard]] std::expected<std::vector<runtime::ExternValue>, ImportRefusal> bind(
        runtime::Store& store, binary::Module const& module);

    /// The calls of its host functions since the last takeTrace(), in order; and empties it.
    /// @return The calls.
    [[nodiscard]] std::vector<std::string> takeTrace();

  private:
    /// The trace the host functions append to and their re-entry budget: shared with each of them, which
    /// the store keeps.
    std::shared_ptr<HostLog> _log;
};

} // namespace isoline::differential
