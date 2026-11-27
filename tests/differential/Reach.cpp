// SPDX-License-Identifier: Apache-2.0
#include "Reach.hpp"

#include <format>
#include <string>
#include <utility>
#include <variant>

#include "Observation.hpp"
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Trap.hpp>

namespace isoline::differential
{

void Reach::add(Observation const& observation)
{
    ++calls;
    if (auto const* abrupt = std::get_if<runtime::Abrupt>(&observation.outcome))
    {
        if (auto const* trap = std::get_if<runtime::Trap>(abrupt))
            ++traps[std::to_underlying(trap->code)];
        else if (std::holds_alternative<runtime::Thrown>(*abrupt))
            ++thrown;
        else
            ++hostFailures;
    }
    if (observation.growth.memory || observation.growth.table)
        ++refusedGrowth;
    bulkGranules += observation.polls[std::to_underlying(runtime::PollSite::BulkGranule)];
    unwinds += observation.polls[std::to_underlying(runtime::PollSite::Unwind)];
    sweeps += observation.polls[std::to_underlying(runtime::PollSite::Sweep)];
    // The recording host's trace lines (RecordingHost.hpp): a re-entry names how it ended.
    auto const exhausted =
        std::format("trap '{}'", runtime::specMessage(runtime::TrapCode::CallStackExhausted));
    for (auto const& line: observation.hostTrace)
    {
        if (line.contains(" reenters "))
        {
            ++reentries;
            if (line.ends_with(exhausted))
                ++nestingExhausted;
        }
        else if (line.contains(" writes "))
            ++hostWrites;
    }
}

void Reach::add(ModuleObservation const& observation)
{
    add(observation.instantiated);
    for (auto const& call: observation.calls)
        add(call);
}

std::string describe(Reach const& reach)
{
    auto text = std::format("calls {}, refused growth {}, thrown {}, host failures {}, bulk granules {}, "
                            "unwinds {}, sweeps {}, re-entries {} (nesting exhausted {}), host writes {}",
                            reach.calls,
                            reach.refusedGrowth,
                            reach.thrown,
                            reach.hostFailures,
                            reach.bulkGranules,
                            reach.unwinds,
                            reach.sweeps,
                            reach.reentries,
                            reach.nestingExhausted,
                            reach.hostWrites);
    for (auto const& row: runtime::TrapTable)
        text += std::format(", trap '{}' {}", row.specMessage, reach.traps[std::to_underlying(row.code)]);
    return text;
}

} // namespace isoline::differential
