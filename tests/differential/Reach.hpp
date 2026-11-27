// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "Observation.hpp"
#include <isoline/runtime/Trap.hpp>

namespace isoline::differential
{

/// The hard edges a set of observations reached, counted: the traps by code, the refused grows, the polls
/// that only the edges cause (bulk granules, unwinding, sweeps), the exceptions thrown out of a call, and
/// what the recording host did (failures, re-entries, the nesting limit, writes). What a test asserts its
/// inputs reach, and what the corpus replay prints.
struct Reach
{
    std::array<uint64_t, runtime::TrapTable.size()> traps {}; ///< The calls that trapped, by TrapCode.
    uint64_t calls = 0;                                       ///< The calls, the instantiations included.
    uint64_t refusedGrowth = 0;    ///< The calls with a refused memory.grow or table.grow.
    uint64_t thrown = 0;           ///< The calls an exception escaped.
    uint64_t hostFailures = 0;     ///< The calls a host failure ended.
    uint64_t bulkGranules = 0;     ///< The BulkGranule polls.
    uint64_t unwinds = 0;          ///< The Unwind polls.
    uint64_t sweeps = 0;           ///< The Sweep polls.
    uint64_t reentries = 0;        ///< The host's re-entries into the guest.
    uint64_t nestingExhausted = 0; ///< The re-entries refused at the store's nested-invocation limit.
    uint64_t hostWrites = 0;       ///< The host's writes into the caller's memory.

    /// Counts what @p observation reached.
    /// @param observation One call's (or an instantiation's) observation.
    void add(Observation const& observation);

    /// Counts what the instantiation and every call of @p observation reached.
    /// @param observation A module's observation.
    void add(ModuleObservation const& observation);
};

/// What a Reach counted, as text: one `name count` per field, the traps by their spec message.
/// @param reach The counts.
/// @return The text.
[[nodiscard]] std::string describe(Reach const& reach);

} // namespace isoline::differential
