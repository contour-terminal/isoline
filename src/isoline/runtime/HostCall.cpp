// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/HostCall.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>

namespace isoline::runtime
{

std::expected<void, Abrupt> callHostFunction(Store& store,
                                             ICodeExecutor& executor,
                                             FunctionInstance const& function,
                                             std::optional<uint32_t> caller,
                                             std::span<Value const> args,
                                             std::span<Value> results) noexcept
{
    Require(function.host != nullptr);
    auto const& type = store.type(function.typeId);
    Require(args.size() == type.params.size());
    Require(results.size() == type.results.size());
    auto const cost = hostFrameCost(type);
    if (auto const charged = store.chargeFrame(cost); !charged)
        return std::unexpected { Abrupt { charged.error() } };
    for (auto const& arg: args)
        store.markEscaped(arg);
    std::ranges::transform(type.results, results.begin(), defaultValue);
    auto const done = [&] {
        auto const scope = HostCallScope { store };
        auto context = HostContext { store, executor, caller };
        return checkHostOutcome(store, type, function.host->call(context, args, results), results);
    }();
    store.releaseFrame(cost);
    if (auto const polled = store.poll(PollSite::HostReturn); !polled)
        return std::unexpected { Abrupt { polled.error() } };
    return done;
}

} // namespace isoline::runtime
