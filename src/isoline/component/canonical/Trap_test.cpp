// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/canonical/GuestMemory.hpp>
#include <isoline/component/canonical/Trap.hpp>

using isoline::component::ComponentLimit;
using isoline::component::canonical::carrierTrap;
using isoline::component::canonical::checkRange;
using isoline::component::canonical::fromCore;
using isoline::component::canonical::outOfBounds;
using isoline::component::canonical::PendingTrap;
using isoline::component::canonical::placeholderCount;
using isoline::component::canonical::Trap;
using isoline::component::canonical::TrapCode;
using isoline::component::canonical::TrapCodeTable;
using isoline::component::canonical::trapOf;

namespace
{

/// The trap @p result failed with.
template <typename T>
[[nodiscard]] Trap trapIn(std::expected<T, Trap> const& result)
{
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

} // namespace

TEST_CASE("Trap.messagesCarryValues")
{
    CHECK(Trap { .code = TrapCode::UnknownHandle, .arguments = { "5" } }.message()
          == "unknown handle index 5");
    CHECK(trapOf(TrapCode::UnknownHandle, 5U)
          == Trap { .code = TrapCode::UnknownHandle, .arguments = { "5" } });
    CHECK(trapOf(TrapCode::WrongHandleType, 1, "R1", "R2").message()
          == "handle index 1 used with the wrong type, expected R1 but found R2");

    // Every row formats: each placeholder takes an argument, and nothing of the format is left over.
    for (auto const& row: TrapCodeTable)
    {
        auto trap = Trap { .code = row.code };
        trap.arguments.assign(placeholderCount(row.format), "<value>");
        auto const message = trap.message();
        CAPTURE(row.format, message);
        CHECK_FALSE(message.empty());
        CHECK_FALSE(message.contains("{}"));
        CHECK(std::ranges::count(message, '<') == static_cast<std::ptrdiff_t>(placeholderCount(row.format)));
    }

    // The caps read as their LimitTable rows do.
    for (auto const& [code, limit]: { std::pair { TrapCode::LiveValueBytes, ComponentLimit::LiveValueBytes },
                                      std::pair { TrapCode::Handles, ComponentLimit::Handles },
                                      std::pair { TrapCode::ResourceTypes, ComponentLimit::ResourceTypes } })
        CHECK(trapOf(code, 7).message() == isoline::component::limitMessage(limit, 7));

    // One wording per trap: the component-model tests' as a prefix, the values after it.
    CHECK(trapOf(TrapCode::InvalidDiscriminant, 2, 2).message()
          == "invalid variant discriminant 2 of 2 cases");
    CHECK(trapOf(TrapCode::InvalidChar, "0xd800").message() == "invalid `char` bit pattern 0xd800");
    CHECK(trapOf(TrapCode::UnalignedPointer, 4, 8).message().starts_with("unaligned pointer"));
    CHECK(outOfBounds(4092, 8, 4096).message()
          == "out of bounds of memory: 8 bytes at 4092 pass its end at 4096");
}

TEST_CASE("Trap.fromCore")
{
    // An interrupt is always the component's Interrupted (R13); every other core trap is Core, carrying
    // the core's code and its spec message.
    CHECK(fromCore(isoline::api::Trap { isoline::api::TrapCode::Interrupted })
          == trapOf(TrapCode::Interrupted));
    auto const unreachable = fromCore(isoline::api::Trap { isoline::api::TrapCode::Unreachable });
    CHECK(unreachable.code == TrapCode::Core);
    CHECK(unreachable.core == isoline::api::TrapCode::Unreachable);
    CHECK(unreachable.message() == "unreachable");
    auto const element = fromCore(isoline::api::Trap { isoline::api::TrapCode::UndefinedElement, 5 });
    CHECK(element.message() == "undefined element 5");
}

TEST_CASE("Trap.checkRange")
{
    CHECK(checkRange(8, 16, 8, 24).has_value());
    CHECK(checkRange(0, 0, 1, 0).has_value());
    CHECK(trapIn(checkRange(4, 8, 8, 4096)) == trapOf(TrapCode::UnalignedPointer, 4, 8));
    CHECK(trapIn(checkRange(4088, 16, 8, 4096)) == outOfBounds(4088, 16, 4096));
    // Alignment is checked first, as definitions.py does.
    CHECK(trapIn(checkRange(4093, 16, 2, 4096)).code == TrapCode::UnalignedPointer);
    // 64-bit arithmetic: a range that would wrap 32 bits is out of bounds, never at a low address.
    CHECK(trapIn(checkRange(0xffff'fff0U, 0x20, 1, uint64_t { 1 } << 32U)).code == TrapCode::OutOfBounds);
}

TEST_CASE("Trap.pendingCrossesCoreFrames")
{
    // A host function raises a component trap: it crosses the core frames as HostTrap, the core call
    // fails with it, and the carrier hands the component trap back exactly once.
    auto pending = PendingTrap {};
    auto imports = isoline::api::Imports {};
    imports.define(
        "env",
        "raise",
        isoline::api::FuncType {},
        isoline::api::makeHostFunction(
            [&](isoline::api::HostContext&,
                std::span<isoline::api::Value const>,
                std::span<isoline::api::Value>) noexcept -> std::expected<void, isoline::api::Abrupt> {
                pending.raise(Trap { .code = TrapCode::UnknownHandle, .arguments = { "5" } });
                return std::unexpected { isoline::api::Abrupt { carrierTrap() } };
            }));
    auto builder = isoline::binary::testing::ModuleBuilder {};
    builder.type(isoline::api::FuncType {})
        .importFunction("env", "raise", 0)
        .function(0, {}, isoline::binary::testing::call(0))
        .exportFunction("run", 1);
    auto const engine = isoline::api::Engine { isoline::api::EngineConfig {} };
    auto const module = engine.load(builder.build());
    REQUIRE(module.has_value());
    auto allocator = isoline::api::HeapMemoryAllocator {};
    auto store = isoline::api::Store { engine, allocator };
    auto const instance = store.instantiate(*module, imports);
    REQUIRE(instance.has_value());
    auto const run = instance->function("run");
    REQUIRE(run.has_value());

    CHECK_FALSE(pending.isPending());
    auto const called = instance->call(*run, {}, {});
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error().failure() == isoline::api::CallFailure::Trap);
    CHECK(called.error().detail == isoline::api::CallError::Detail { carrierTrap() });
    CHECK(pending.isPending());
    CHECK(pending.take() == Trap { .code = TrapCode::UnknownHandle, .arguments = { "5" } });
    CHECK_FALSE(pending.take().has_value());
    CHECK_FALSE(pending.isPending());
}

TEST_CASE("Trap.firstRaisedWins")
{
    // A trap raised while the first unwinds (a host swallowing it, then failing again) does not replace
    // the first: the first trap is what ended the call.
    auto pending = PendingTrap {};
    pending.raise(trapOf(TrapCode::UnknownHandle, 5));
    pending.raise(trapOf(TrapCode::CannotEnter));
    CHECK(pending.take() == trapOf(TrapCode::UnknownHandle, 5));
    pending.raise(trapOf(TrapCode::CannotEnter));
    CHECK(pending.take() == trapOf(TrapCode::CannotEnter));
}
