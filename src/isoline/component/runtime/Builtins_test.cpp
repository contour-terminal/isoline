// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/runtime/Builtins.hpp>
#include <isoline/component/runtime/testing/ResourceDoubles.hpp>

using isoline::component::ComponentFeature;
using isoline::component::binary::CanonBuiltin;
using isoline::component::binary::CanonBuiltinTable;
using isoline::component::canonical::carrierTrap;
using isoline::component::canonical::TrapCode;
using isoline::component::canonical::trapOf;
using isoline::component::runtime::BuiltinBinding;
using isoline::component::runtime::builtinHandlerRow;
using isoline::component::runtime::BuiltinHandlerTable;
using isoline::component::runtime::BuiltinSupport;
using isoline::component::runtime::MayLeave;
using isoline::component::runtime::testing::CoreStore;
using isoline::component::runtime::testing::FakeBuiltinHost;
using isoline::component::runtime::testing::guestImplementor;
using isoline::component::runtime::testing::valueIn;

TEST_CASE("Builtins.tableCoversEveryCanonRow")
{
    // One row per CanonBuiltinTable row but lift and lower; Unimplemented exactly the 🔀, 🧵, 📝 and ➡️
    // rows, which are the gated ones.
    constexpr auto P12Gates = std::to_array<ComponentFeature>({ ComponentFeature::Async,
                                                                ComponentFeature::Threading,
                                                                ComponentFeature::SharedThreading,
                                                                ComponentFeature::ErrorContext,
                                                                ComponentFeature::Forward });
    auto covered = std::size_t { 0 };
    for (auto const& canon: CanonBuiltinTable)
    {
        CAPTURE(canon.text);
        if (canon.builtin == CanonBuiltin::Lift || canon.builtin == CanonBuiltin::Lower)
            continue;
        auto const& row = builtinHandlerRow(canon.builtin);
        CHECK(row.builtin == canon.builtin);
        auto const p12 = canon.gate.has_value() && std::ranges::find(P12Gates, *canon.gate) != P12Gates.end();
        CHECK(p12 == canon.gate.has_value());
        CHECK((row.support == BuiltinSupport::Unimplemented) == p12);
        CHECK((row.factory != nullptr) == !p12);
        ++covered;
    }
    CHECK(covered == BuiltinHandlerTable.size());
    CHECK(covered == CanonBuiltinTable.size() - 2);
    CHECK(builtinHandlerRow(CanonBuiltin::ResourceNew).support == BuiltinSupport::Implemented);
    CHECK(builtinHandlerRow(CanonBuiltin::ResourceDrop).support == BuiltinSupport::Implemented);
    CHECK(builtinHandlerRow(CanonBuiltin::ResourceRep).support == BuiltinSupport::Implemented);
}

TEST_CASE("Builtins.resourceFunctionsRunInACoreStore")
{
    // The core functions instantiation defines: (i32) -> (i32) for new and rep, (i32) -> () for drop. A
    // trap waits in the carrier while the core call fails with carrierTrap().
    auto core = CoreStore {};
    auto host = FakeBuiltinHost {};
    auto const instance = host.resources().instances().add();
    auto const type = valueIn(host.resources().types().create(guestImplementor(instance)));
    auto const binding = BuiltinBinding { .host = host, .instance = instance, .type = type };

    auto defineRow = [&](CanonBuiltin builtin) {
        auto const made = builtinHandlerRow(builtin).factory(binding);
        auto defined = core.store.define(made.type, made.function);
        REQUIRE(defined.has_value());
        return *defined;
    };
    auto const create = defineRow(CanonBuiltin::ResourceNew);
    auto const drop = defineRow(CanonBuiltin::ResourceDrop);
    auto const rep = defineRow(CanonBuiltin::ResourceRep);
    auto call = [&](isoline::api::Function const& function, uint32_t argument, std::size_t resultCount) {
        auto const args = std::array<isoline::api::Value, 1> { isoline::api::I32 { .bits = argument } };
        auto results = std::vector<isoline::api::Value>(resultCount);
        auto const called = core.store.call(function, args, results);
        return called.transform([&] { return results; });
    };
    auto const i32 = [](uint32_t bits) {
        return std::vector<isoline::api::Value> { isoline::api::I32 { .bits = bits } };
    };

    auto const made = call(create, 100, 1);
    REQUIRE(made.has_value());
    CHECK(*made == i32(1));
    auto const read = call(rep, 1, 1);
    REQUIRE(read.has_value());
    CHECK(*read == i32(100));
    CHECK(call(drop, 1, 0).has_value());
    CHECK_FALSE(host.pendingTrap().isPending());

    auto const failed = call(drop, 1, 0);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().detail == isoline::api::CallError::Detail { carrierTrap() });
    CHECK(host.pendingTrap().take() == trapOf(TrapCode::UnknownHandle, 1U));

    host.resources().instances()[instance].mayLeave = MayLeave::No;
    REQUIRE_FALSE(call(create, 5, 1).has_value());
    CHECK(host.pendingTrap().take() == trapOf(TrapCode::CannotLeave));
    REQUIRE_FALSE(call(rep, 0, 1).has_value());
    CHECK(host.pendingTrap().take() == trapOf(TrapCode::UnknownHandle, 0U));
}
