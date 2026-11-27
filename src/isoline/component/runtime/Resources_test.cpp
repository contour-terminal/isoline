// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <variant>
#include <vector>

#include <isoline/component/Limits.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/Builtins.hpp>
#include <isoline/component/runtime/HandleTable.hpp>
#include <isoline/component/runtime/Resources.hpp>
#include <isoline/component/runtime/testing/ResourceDoubles.hpp>
#include <isoline/component/types/TypeArena.hpp>

using isoline::component::ComponentLimits;
using isoline::component::canonical::ResourceTypeIndex;
using isoline::component::canonical::ResourceValue;
using isoline::component::canonical::Trap;
using isoline::component::canonical::TrapCode;
using isoline::component::canonical::trapOf;
using isoline::component::runtime::BorrowScope;
using isoline::component::runtime::GuestImplementor;
using isoline::component::runtime::HandleEntry;
using isoline::component::runtime::Lender;
using isoline::component::runtime::MayLeave;
using isoline::component::runtime::Ownership;
using isoline::component::runtime::ResourceBindings;
using isoline::component::runtime::resourceDrop;
using isoline::component::runtime::ResourceHandle;
using isoline::component::runtime::resourceNew;
using isoline::component::runtime::ResourceOperations;
using isoline::component::runtime::resourceRep;
using isoline::component::runtime::ResourceStore;
using isoline::component::runtime::ResourceTypeTable;
using isoline::component::runtime::Subtask;
using isoline::component::runtime::Task;
using isoline::component::runtime::TaskId;
using isoline::component::runtime::testing::CoreStore;
using isoline::component::runtime::testing::DestructorCall;
using isoline::component::runtime::testing::FakeBuiltinHost;
using isoline::component::runtime::testing::guestImplementor;
using isoline::component::runtime::testing::hostImplementor;
using isoline::component::runtime::testing::RecordingDestructor;
using isoline::component::runtime::testing::trapIn;
using isoline::component::runtime::testing::valueIn;
using isoline::component::types::ResourceTypeId;

namespace
{

/// The first resource of a side's arena, and the second.
constexpr auto ArenaR = ResourceTypeId { 0 };
constexpr auto ArenaS = ResourceTypeId { 1 };

/// Bindings of the arena's first resource to @p first and, if given, its second to @p second.
[[nodiscard]] ResourceBindings bindingsOf(ResourceTypeIndex first,
                                          std::optional<ResourceTypeIndex> second = {})
{
    auto bindings = ResourceBindings {};
    bindings.bind(ArenaR, first);
    if (second)
        bindings.bind(ArenaS, *second);
    return bindings;
}

/// A fresh runtime resource type of @p host's store.
[[nodiscard]] ResourceTypeIndex createType(FakeBuiltinHost& host,
                                           isoline::component::runtime::ResourceImplementor implementor)
{
    return valueIn(host.resources().types().create(std::move(implementor)));
}

/// The handle at @p index of @p instance's table.
[[nodiscard]] ResourceHandle handleAt(FakeBuiltinHost& host, uint32_t instance, uint32_t index)
{
    auto const entry = host.resources().instances()[instance].handles.get(index);
    REQUIRE(entry.has_value());
    return std::get<ResourceHandle>(entry->get());
}

/// Whether @p instance's table has a handle at @p index.
[[nodiscard]] bool holds(FakeBuiltinHost& host, uint32_t instance, uint32_t index)
{
    return host.resources().instances()[instance].handles.get(index).has_value();
}

} // namespace

TEST_CASE("ResourceTypeTable.generativeAndCapped")
{
    auto types = ResourceTypeTable { 2 };
    // One definition executed twice mints two types, never equal.
    auto const first = valueIn(types.create(guestImplementor(1)));
    auto const second = valueIn(types.create(guestImplementor(2)));
    CHECK(first != second);
    CHECK(first.value != 0);
    CHECK(types.contains(first));
    CHECK(types.contains(second));
    CHECK_FALSE(types.contains(ResourceTypeIndex {}));
    CHECK_FALSE(types.contains(ResourceTypeIndex { 3 }));
    CHECK(std::get<GuestImplementor>(types[second]).componentInstance == 2);

    auto const refused = trapIn(types.create(guestImplementor(3)));
    CHECK(refused == trapOf(TrapCode::ResourceTypes, 2));
    CHECK(refused.message() == "resource types count exceeds limit of 2");
    CHECK(types.size() == 2);
}

TEST_CASE("Handles.everyMisuseTraps")
{
    auto host = FakeBuiltinHost {};
    auto& resources = host.resources();
    auto const a = resources.instances().add();
    auto const b = resources.instances().add();
    auto const r1 = createType(host, guestImplementor(a));
    auto const r2 = createType(host, guestImplementor(a));
    auto recorder = std::make_shared<RecordingDestructor>();
    auto const hosted = createType(host, hostImplementor("h", recorder));
    auto const bindA = bindingsOf(r1, r2);
    auto const bindB = bindingsOf(r1, r2);

    SECTION("unknown indices: 0, past the end, the largest, freed, dropped twice")
    {
        CHECK(trapIn(resourceDrop(host, a, r1, 0)).message() == "unknown handle index 0");
        CHECK(trapIn(resourceRep(host, a, r1, 5)).message() == "unknown handle index 5");
        CHECK(trapIn(resourceDrop(host, a, r1, 5)).message() == "unknown handle index 5");
        CHECK(trapIn(resourceDrop(host, a, r1, 4294967295U)).message() == "unknown handle index 4294967295");
        auto const index = valueIn(resourceNew(host, a, r1, 100));
        CHECK(index == 1);
        REQUIRE(resourceDrop(host, a, r1, index).has_value());
        CHECK(trapIn(resourceDrop(host, a, r1, index)).message() == "unknown handle index 1");
        CHECK(trapIn(resourceRep(host, a, r1, index)).message() == "unknown handle index 1");
        // A handle index of one instance is unknown in another.
        REQUIRE(resourceNew(host, a, r1, 100).has_value());
        CHECK(trapIn(resourceDrop(host, b, r1, 1)).message() == "unknown handle index 1");
    }

    SECTION("a reused slot holds another type: handle-table.wast's drop-R1-as-R2")
    {
        auto const index = valueIn(resourceNew(host, a, r1, 100));
        REQUIRE(resourceDrop(host, a, r1, index).has_value());
        CHECK(valueIn(resourceNew(host, a, r2, 200)) == index);
        auto const trap = trapIn(resourceDrop(host, a, r1, index));
        CHECK(trap.code == TrapCode::WrongHandleType);
        CHECK(trap.message()
              == "handle index 1 used with the wrong type, expected guest-defined resource but found a "
                 "different guest-defined resource");
        CHECK(trapIn(resourceRep(host, a, r1, index)) == trap);
        // The trap changed nothing.
        CHECK(valueIn(resourceRep(host, a, r2, index)) == 200);
        // rep of a foreign type: a host type's built-in on a guest handle.
        CHECK(trapIn(resourceRep(host, a, hosted, index)).message()
              == "handle index 1 used with the wrong type, expected host-defined resource but found "
                 "guest-defined resource");
    }

    SECTION("an own handle lent to a call in progress")
    {
        auto const index = valueIn(resourceNew(host, a, r1, 100));
        auto subtask = Subtask {};
        auto caller = ResourceOperations { resources, a, bindA, BorrowScope { &subtask } };
        CHECK(valueIn(caller.liftBorrow(ArenaR, index))
              == ResourceValue { .type = r1, .rep = 100, .ownership = Ownership::Borrow });
        CHECK(handleAt(host, a, index).lendCount == 1);

        auto const dropped = trapIn(resourceDrop(host, a, r1, index));
        CHECK(dropped.code == TrapCode::LendOutstanding);
        CHECK(dropped.message() == "cannot remove owned resource while borrowed");
        // lift_own of a lent own: the same trap, the handle kept.
        auto lifter = ResourceOperations { resources, a, bindA, BorrowScope {} };
        CHECK(trapIn(lifter.liftOwn(ArenaR, index)) == dropped);
        CHECK(handleAt(host, a, index).lendCount == 1);
        CHECK(host.destructorCalls.empty());

        subtask.deliverResolve(resources.instances());
        CHECK(subtask.state == Subtask::State::Returned);
        CHECK(handleAt(host, a, index).lendCount == 0);
        REQUIRE(resourceDrop(host, a, r1, index).has_value());
    }

    SECTION("a borrow handle: dropped twice, held at return, lifted as an own")
    {
        auto const task = resources.tasks().begin();
        auto callee = ResourceOperations { resources, b, bindB, BorrowScope { task } };
        auto const lent = ResourceValue { .type = r1, .rep = 7, .ownership = Ownership::Borrow };
        auto const index = valueIn(callee.lowerBorrow(ArenaR, lent));
        CHECK(resources.tasks().find(task)->borrowCount == 1);
        CHECK(handleAt(host, b, index).borrowScope == task);

        auto lifter = ResourceOperations { resources, b, bindB, BorrowScope {} };
        auto const notOwn = trapIn(lifter.liftOwn(ArenaR, index));
        CHECK(notOwn == trapOf(TrapCode::OwnExpected, index));
        CHECK(notOwn.message() == "handle index 1 is a borrow where an own is expected");
        CHECK(holds(host, b, index));

        auto const returned = trapIn(resources.tasks().resolve(task));
        CHECK(returned.code == TrapCode::BorrowOutstanding);
        CHECK(returned.message() == "borrow handles still remain at the end of the call");
        CHECK(resources.tasks().find(task)->state == Task::State::Initial);

        REQUIRE(resourceDrop(host, b, r1, index).has_value());
        CHECK(resources.tasks().find(task)->borrowCount == 0);
        CHECK(trapIn(resourceDrop(host, b, r1, index)).message() == "unknown handle index 1");
        CHECK(resources.tasks().resolve(task).has_value());
        resources.tasks().end(task);
        // Dropping a borrow runs no destructor.
        CHECK(host.destructorCalls.empty());
    }

    SECTION("may_leave cleared: resource.new and resource.drop")
    {
        auto const index = valueIn(resourceNew(host, a, r1, 100));
        resources.instances()[a].mayLeave = MayLeave::No;
        CHECK(trapIn(resourceNew(host, a, r1, 1)).message() == "cannot leave component instance");
        CHECK(trapIn(resourceDrop(host, a, r1, index)).code == TrapCode::CannotLeave);
        // resource.rep does not leave.
        CHECK(valueIn(resourceRep(host, a, r1, index)) == 100);
        resources.instances()[a].mayLeave = MayLeave::Yes;
        REQUIRE(resourceDrop(host, a, r1, index).has_value());
    }

    SECTION("a resource lowered as another type")
    {
        auto const task = resources.tasks().begin();
        auto lowering = ResourceOperations { resources, b, bindB, BorrowScope { task } };
        auto const foreign = ResourceValue { .type = r2, .rep = 9, .ownership = Ownership::Own };
        auto const trap = trapIn(lowering.lowerOwn(ArenaR, foreign));
        CHECK(trap == trapOf(TrapCode::WrongResource, 9));
        CHECK(trap.message() == "resource 9 lowered as another resource type");
        CHECK(trapIn(lowering.lowerBorrow(ArenaR, foreign)) == trap);
        // An index the store never minted is foreign too.
        auto const unknown = ResourceValue { .type = ResourceTypeIndex { 99 }, .rep = 9 };
        CHECK(trapIn(lowering.lowerOwn(ArenaR, unknown)) == trap);
        CHECK(resources.instances()[b].handles.length() == 1);
        resources.tasks().end(task);
    }
}

TEST_CASE("Handles.borrowOutlivingItsCall")
{
    // A callee returns holding a borrow: the return traps, the call ends (the store locks down), and a
    // drop of that borrow afterwards (guest code still unwinding) finds no task to lend it to.
    auto host = FakeBuiltinHost {};
    auto& resources = host.resources();
    auto const a = resources.instances().add();
    auto const r = createType(host, guestImplementor(a));
    auto const callee = resources.instances().add();
    auto const bindings = bindingsOf(r);

    auto const task = resources.tasks().begin();
    auto ops = ResourceOperations { resources, callee, bindings, BorrowScope { task } };
    auto const index = valueIn(ops.lowerBorrow(ArenaR, ResourceValue { .type = r, .rep = 3 }));
    CHECK(trapIn(resources.tasks().resolve(task)).code == TrapCode::BorrowOutstanding);
    resources.tasks().end(task);
    CHECK(resources.tasks().find(task) == nullptr);

    // A later task never takes the ended one's id.
    auto const next = resources.tasks().begin();
    CHECK(next != task);
    CHECK(trapIn(resourceDrop(host, callee, r, index)) == trapOf(TrapCode::BorrowOutstanding));
    CHECK(holds(host, callee, index));
    resources.tasks().end(next);
}

TEST_CASE("Handles.lenderSurvivesTableGrowth")
{
    // [method]r.clone(self: borrow<r>) -> own<r>, r implemented by the callee: lifting the argument
    // lends the caller's handle 1; meanwhile a re-entrant call adds 10'000 handles to the caller's
    // table, and lowering the result grows it past its first chunks. deliverResolve finds the lender by
    // its place, not by an address the growth could have moved.
    auto host = FakeBuiltinHost {};
    auto& resources = host.resources();
    auto const caller = resources.instances().add();
    auto const callee = resources.instances().add();
    auto const r = createType(host, guestImplementor(callee));
    auto const callerBindings = bindingsOf(r);
    auto const calleeBindings = bindingsOf(r);

    // The callee made the resource and handed its own to the caller.
    auto const lentIndex = valueIn(resourceNew(host, caller, r, 1000));
    auto const otherIndex = valueIn(resourceNew(host, caller, r, 1001));

    auto subtask = Subtask {};
    auto callerSide = ResourceOperations { resources, caller, callerBindings, BorrowScope { &subtask } };
    auto const self = valueIn(callerSide.liftBorrow(ArenaR, lentIndex));
    subtask.state = Subtask::State::Started;

    auto const task = resources.tasks().begin();
    auto calleeSide = ResourceOperations { resources, callee, calleeBindings, BorrowScope { task } };
    // Into the implementor, the borrow is the representation.
    CHECK(valueIn(calleeSide.lowerBorrow(ArenaR, self)) == 1000);

    // Re-entrance: the callee calls back into the caller, which makes 10'000 handles.
    for (auto const rep: std::views::iota(uint32_t { 0 }, uint32_t { 10'000 }))
        REQUIRE(resourceNew(host, caller, r, rep).has_value());

    // The callee returns a clone.
    auto const clone = valueIn(resourceNew(host, callee, r, 2000));
    auto resultSide = ResourceOperations { resources, callee, calleeBindings, BorrowScope {} };
    auto const result = valueIn(resultSide.liftOwn(ArenaR, clone));
    REQUIRE(resources.tasks().resolve(task).has_value());
    resources.tasks().end(task);
    auto const resultIndex = valueIn(callerSide.lowerOwn(ArenaR, result));
    CHECK(resultIndex == 10'003);

    subtask.deliverResolve(resources.instances());
    CHECK(handleAt(host, caller, lentIndex).lendCount == 0);
    CHECK(handleAt(host, caller, otherIndex).lendCount == 0);
    CHECK(handleAt(host, caller, resultIndex).rep == 2000);
    // Every lend is released: the budget holds the slots only.
    CHECK(resources.budget().used() == resources.instances()[caller].handles.length() - 1 + 1);
    REQUIRE(resourceDrop(host, caller, r, lentIndex).has_value());
    CHECK(host.destructorCalls.empty());
}

TEST_CASE("Handles.twoInstancesOfOneChildDiffer")
{
    // One child component defining R, instantiated twice: two runtime types. The parent holds an own of
    // the first instance's R and passes it where the second's is expected.
    auto core = CoreStore {};
    auto host = FakeBuiltinHost {};
    auto& resources = host.resources();
    auto const parent = resources.instances().add();
    auto const child1 = resources.instances().add();
    auto const child2 = resources.instances().add();
    auto const r1 = createType(host, guestImplementor(child1, core.destructor()));
    auto const r2 = createType(host, guestImplementor(child2, core.destructor()));
    CHECK(r1 != r2);
    // Both children see R as the same resource of their (shared) arena, bound differently.
    auto const child1Bindings = bindingsOf(r1);
    auto const child2Bindings = bindingsOf(r2);
    auto const parentBindings = bindingsOf(r1, r2);

    auto const made = valueIn(resourceNew(host, child1, r1, 77));
    auto child1Results = ResourceOperations { resources, child1, child1Bindings, BorrowScope {} };
    auto const own = valueIn(child1Results.liftOwn(ArenaR, made));
    auto parentSide = ResourceOperations { resources, parent, parentBindings, BorrowScope {} };
    auto const held = valueIn(parentSide.lowerOwn(ArenaR, own));

    // Lifted where the second instance's R is expected.
    auto const trap = trapIn(parentSide.liftOwn(ArenaS, held));
    CHECK(trap.code == TrapCode::WrongHandleType);
    CHECK(trap.message()
          == "handle index 1 used with the wrong type, expected guest-defined resource but found a different "
             "guest-defined resource");
    CHECK(holds(host, parent, held));
    // Lowered into the second instance as its R.
    auto const task = resources.tasks().begin();
    auto child2Side = ResourceOperations { resources, child2, child2Bindings, BorrowScope { task } };
    CHECK(trapIn(child2Side.lowerOwn(ArenaR, own)).code == TrapCode::WrongResource);
    CHECK(trapIn(child2Side.lowerBorrow(ArenaR, own)).code == TrapCode::WrongResource);
    resources.tasks().end(task);

    // Dropped as what it is: the first instance's destructor runs, never the second's.
    CHECK(trapIn(resourceDrop(host, parent, r2, held)).code == TrapCode::WrongHandleType);
    REQUIRE(resourceDrop(host, parent, r1, held).has_value());
    CHECK(host.destructorCalls
          == std::vector<DestructorCall> {
              DestructorCall { .implementor = child1, .caller = parent, .rep = 77 } });
}

TEST_CASE("Handles.ownTransferMovesHandle")
{
    auto host = FakeBuiltinHost {};
    auto& resources = host.resources();
    auto const caller = resources.instances().add();
    auto const callee = resources.instances().add();
    auto const r = createType(host, guestImplementor(caller));
    auto const bindings = bindingsOf(r);
    REQUIRE(resourceNew(host, caller, r, 10).has_value());
    auto const moved = valueIn(resourceNew(host, caller, r, 20));

    auto subtask = Subtask {};
    auto callerSide = ResourceOperations { resources, caller, bindings, BorrowScope { &subtask } };
    auto const value = valueIn(callerSide.liftOwn(ArenaR, moved));
    CHECK(value == ResourceValue { .type = r, .rep = 20, .ownership = Ownership::Own });
    CHECK_FALSE(holds(host, caller, moved));
    CHECK(subtask.lenders.empty());

    auto const task = resources.tasks().begin();
    auto calleeSide = ResourceOperations { resources, callee, bindings, BorrowScope { task } };
    auto const index = valueIn(calleeSide.lowerOwn(ArenaR, value));
    CHECK(index == 1);
    CHECK(handleAt(host, callee, index)
          == ResourceHandle {
              .type = r, .rep = 20, .ownership = Ownership::Own, .borrowScope = {}, .lendCount = 0 });
    // An own lowered counts no borrow.
    CHECK(resources.tasks().find(task)->borrowCount == 0);
    resources.tasks().end(task);
    // The caller's freed slot is the next one it hands out.
    CHECK(valueIn(resourceNew(host, caller, r, 30)) == moved);
}

TEST_CASE("Handles.borrowIntoImplementorIsRep")
{
    auto host = FakeBuiltinHost {};
    auto& resources = host.resources();
    auto const implementor = resources.instances().add();
    auto const other = resources.instances().add();
    auto const r = createType(host, guestImplementor(implementor));
    auto const bindings = bindingsOf(r);
    auto const borrowed = ResourceValue { .type = r, .rep = 4242, .ownership = Ownership::Borrow };

    auto const task = resources.tasks().begin();
    auto intoImplementor = ResourceOperations { resources, implementor, bindings, BorrowScope { task } };
    CHECK(valueIn(intoImplementor.lowerBorrow(ArenaR, borrowed)) == 4242);
    CHECK(resources.instances()[implementor].handles.length() == 1);
    CHECK(resources.tasks().find(task)->borrowCount == 0);

    // Into any other instance, a borrow handle lent to the task.
    auto intoOther = ResourceOperations { resources, other, bindings, BorrowScope { task } };
    auto const index = valueIn(intoOther.lowerBorrow(ArenaR, borrowed));
    CHECK(index == 1);
    CHECK(handleAt(host, other, index)
          == ResourceHandle {
              .type = r, .rep = 4242, .ownership = Ownership::Borrow, .borrowScope = task, .lendCount = 0 });
    CHECK(resources.tasks().find(task)->borrowCount == 1);
    // The representation is no handle index: resource.rep of 4242 is unknown.
    CHECK(trapIn(resourceRep(host, other, r, 4242)).message() == "unknown handle index 4242");
    REQUIRE(resourceDrop(host, other, r, index).has_value());
    CHECK(resources.tasks().resolve(task).has_value());
    resources.tasks().end(task);
}

TEST_CASE("Handles.hostDestructorRunsOnDrop")
{
    auto recorder = std::make_shared<RecordingDestructor>();
    {
        auto host = FakeBuiltinHost {};
        auto& resources = host.resources();
        auto const guest = resources.instances().add();
        auto const hosted = createType(host, hostImplementor("h", recorder));
        auto const guestType = createType(host, guestImplementor(guest));
        auto const bindings = bindingsOf(hosted, guestType);

        auto const task = resources.tasks().begin();
        auto side = ResourceOperations { resources, guest, bindings, BorrowScope { task } };
        auto const dropped = valueIn(side.lowerOwn(ArenaR, ResourceValue { .type = hosted, .rep = 5 }));
        REQUIRE(resourceDrop(host, guest, hosted, dropped).has_value());
        CHECK(recorder->destroyed == std::vector<uint32_t> { 5 });

        auto const borrow = valueIn(side.lowerBorrow(
            ArenaR, ResourceValue { .type = hosted, .rep = 6, .ownership = Ownership::Borrow }));
        REQUIRE(resourceDrop(host, guest, hosted, borrow).has_value());
        CHECK(recorder->destroyed == std::vector<uint32_t> { 5 });
        REQUIRE(resources.tasks().resolve(task).has_value());
        resources.tasks().end(task);

        // Held at teardown: an own of the host type, a borrow of it, and a guest type's own.
        auto const kept = resources.tasks().begin();
        auto keeper = ResourceOperations { resources, guest, bindings, BorrowScope { kept } };
        REQUIRE(keeper.lowerOwn(ArenaR, ResourceValue { .type = hosted, .rep = 9 }).has_value());
        REQUIRE(keeper
                    .lowerBorrow(ArenaR,
                                 ResourceValue { .type = hosted, .rep = 10, .ownership = Ownership::Borrow })
                    .has_value());
        REQUIRE(resourceNew(host, guest, guestType, 11).has_value());
        // A teardown runs no call: the task the borrow was lent to has ended (trapped).
        resources.tasks().end(kept);
        resources.destroyHostResources();
        CHECK(recorder->destroyed == std::vector<uint32_t> { 5, 9 });
        // Destroying the store destroys nothing twice, and runs no guest destructor.
    }
    CHECK(recorder->destroyed == std::vector<uint32_t> { 5, 9 });

    {
        auto store = ResourceStore { ComponentLimits {} };
        auto const instance = store.instances().add();
        auto const hosted = valueIn(store.types().create(hostImplementor("h", recorder)));
        REQUIRE(store.instances()[instance]
                    .handles.add(ResourceHandle { .type = hosted, .rep = 12, .ownership = Ownership::Own })
                    .has_value());
    }
    CHECK(recorder->destroyed == std::vector<uint32_t> { 5, 9, 12 });
}

TEST_CASE("Handles.destructorReentersTheInstance")
{
    // resource.drop removes the handle before the destructor runs: a destructor that re-enters the
    // instance finds the slot free, may take it back, and may grow the table, all safely.
    auto core = CoreStore {};
    auto host = FakeBuiltinHost {};
    auto& resources = host.resources();
    auto const instance = resources.instances().add();
    auto const r = createType(host, guestImplementor(instance, core.destructor()));
    auto const dropped = valueIn(resourceNew(host, instance, r, 1));
    REQUIRE(resourceNew(host, instance, r, 2).has_value());

    auto reentered = std::vector<uint32_t> {};
    host.onDestructor = [&](uint32_t rep) -> std::expected<void, Trap> {
        if (rep != 1)
            return {};
        // The dropped slot is free, and the next new takes it.
        CHECK_FALSE(holds(host, instance, dropped));
        reentered.push_back(valueIn(resourceNew(host, instance, r, 3)));
        for (auto const more: std::views::iota(uint32_t { 100 }, uint32_t { 1100 }))
            reentered.push_back(valueIn(resourceNew(host, instance, r, more)));
        // Dropping the reused index drops the new handle, nested.
        return resourceDrop(host, instance, r, dropped);
    };
    REQUIRE(resourceDrop(host, instance, r, dropped).has_value());
    CHECK(reentered.front() == dropped);
    CHECK(reentered.size() == 1001);
    CHECK(host.destructorCalls
          == std::vector<DestructorCall> {
              DestructorCall { .implementor = instance, .caller = instance, .rep = 1 },
              DestructorCall { .implementor = instance, .caller = instance, .rep = 3 } });
    CHECK_FALSE(holds(host, instance, dropped));
    CHECK(handleAt(host, instance, 2).rep == 2);
    CHECK(handleAt(host, instance, 1002).rep == 1099);

    SECTION("a destructor's trap is the drop's, the handle gone")
    {
        host.onDestructor = [](uint32_t) -> std::expected<void, Trap> {
            return std::unexpected { trapOf(TrapCode::CannotEnter) };
        };
        CHECK(trapIn(resourceDrop(host, instance, r, 2)).code == TrapCode::CannotEnter);
        CHECK_FALSE(holds(host, instance, 2));
    }
}

TEST_CASE("Handles.lendsChargeTheBudget")
{
    // Each lend is charged to the store's handle budget until the call returns, so a call lending one
    // handle many times holds no more than the budget lets it, and no lendCount overflows.
    auto limits = ComponentLimits {};
    limits.maxHandles = 3;
    auto host = FakeBuiltinHost { limits };
    auto& resources = host.resources();
    auto const instance = resources.instances().add();
    auto const r = createType(host, guestImplementor(instance));
    auto const bindings = bindingsOf(r);
    auto const index = valueIn(resourceNew(host, instance, r, 1));

    auto subtask = Subtask {};
    auto side = ResourceOperations { resources, instance, bindings, BorrowScope { &subtask } };
    REQUIRE(side.liftBorrow(ArenaR, index).has_value());
    REQUIRE(side.liftBorrow(ArenaR, index).has_value());
    CHECK(resources.budget().used() == 3);
    CHECK(trapIn(side.liftBorrow(ArenaR, index)) == trapOf(TrapCode::Handles, 3));
    CHECK(handleAt(host, instance, index).lendCount == 2);
    CHECK(subtask.lenders == std::vector<Lender> { Lender { instance, index }, Lender { instance, index } });

    subtask.deliverResolve(resources.instances());
    CHECK(handleAt(host, instance, index).lendCount == 0);
    CHECK(resources.budget().used() == 1);
}

namespace
{

/// An operation of the differential run, as make-handle-vectors.py names it.
enum class Op : uint8_t
{
    New,
    Drop,
    Rep,
    LiftOwn,
    LiftBorrow,
    Resolve,
    LowerOwn,
    LowerBorrow,
    Return,
};

/// How definitions.py's operation ended: returned a value, or trapped with the TrapCode of the trap_if
/// that fired (make-handle-vectors.py's TRAP_CODES). Core never ends one of these operations, so it
/// stands for "returned". One member per code the vectors name.
struct Outcome
{
    static constexpr auto Value = TrapCode::Core;
    static constexpr auto UnknownHandle = TrapCode::UnknownHandle;
    static constexpr auto WrongHandleType = TrapCode::WrongHandleType;
    static constexpr auto LendOutstanding = TrapCode::LendOutstanding;
    static constexpr auto OwnExpected = TrapCode::OwnExpected;
    static constexpr auto BorrowOutstanding = TrapCode::BorrowOutstanding;
};

/// No destructor ran.
constexpr auto NoRep = int64_t { -1 };

/// One step of the run: the operation, its resource type (1 or 2), its argument, and what
/// definitions.py did: returned a value or trapped (and which trap), and which representation a
/// destructor got.
struct HandleVector
{
    Op op;
    uint32_t type;
    uint32_t argument;
    TrapCode outcome;
    uint32_t value;
    int64_t destroyed;
};

constexpr auto HandleVectors = std::to_array<HandleVector>({
#include <isoline/component/runtime/testing/HandleVectors.inc>
});

} // namespace

TEST_CASE("Handles.matchDefinitions")
{
    // The same operations as make-handle-vectors.py ran on definitions.py's Table, ResourceHandle,
    // Task and Subtask: every index, representation, trap and destructor call agrees. Type 1 is
    // implemented elsewhere (here: the host), type 2 by the instance itself.
    auto core = CoreStore {};
    auto host = FakeBuiltinHost {};
    auto& resources = host.resources();
    auto const instance = resources.instances().add();
    auto recorder = std::make_shared<RecordingDestructor>();
    auto const elsewhere = createType(host, hostImplementor("elsewhere", recorder));
    auto const itself = createType(host, guestImplementor(instance, core.destructor()));
    auto const bindings = bindingsOf(elsewhere, itself);
    auto subtask = Subtask {};
    auto task = resources.tasks().begin();

    for (auto const [step, vector]: core::views::enumerate(HandleVectors))
    {
        CAPTURE(step, static_cast<int>(vector.op), vector.type, vector.argument);
        auto const type = vector.type == 1 ? elsewhere : itself;
        auto const resource = vector.type == 1 ? ArenaR : ArenaS;
        recorder->destroyed.clear();
        host.destructorCalls.clear();
        auto lifting = ResourceOperations { resources, instance, bindings, BorrowScope { &subtask } };
        auto lowering = ResourceOperations { resources, instance, bindings, BorrowScope { task } };
        auto const value = ResourceValue { .type = type, .rep = vector.argument };
        auto const repOf = [](std::expected<ResourceValue, Trap> const& lifted) {
            return lifted.transform([](ResourceValue const& handle) { return handle.rep; });
        };

        auto outcome = std::expected<uint32_t, Trap> { 0U };
        switch (vector.op)
        {
            case Op::New: outcome = resourceNew(host, instance, type, vector.argument); break;
            case Op::Drop:
                outcome = resourceDrop(host, instance, type, vector.argument).transform([] { return 0U; });
                break;
            case Op::Rep: outcome = resourceRep(host, instance, type, vector.argument); break;
            case Op::LiftOwn: outcome = repOf(lifting.liftOwn(resource, vector.argument)); break;
            case Op::LiftBorrow: outcome = repOf(lifting.liftBorrow(resource, vector.argument)); break;
            case Op::Resolve:
                subtask.deliverResolve(resources.instances());
                subtask = Subtask {};
                break;
            case Op::LowerOwn: outcome = lowering.lowerOwn(resource, value); break;
            case Op::LowerBorrow:
                outcome = lowering.lowerBorrow(
                    resource,
                    ResourceValue { .type = type, .rep = vector.argument, .ownership = Ownership::Borrow });
                break;
            case Op::Return:
                outcome = resources.tasks().resolve(task).transform([] { return 0U; });
                if (outcome)
                {
                    resources.tasks().end(task);
                    task = resources.tasks().begin();
                }
                break;
        }

        REQUIRE(outcome.has_value() == (vector.outcome == Outcome::Value));
        if (outcome)
            CHECK(*outcome == vector.value);
        else
            CHECK(outcome.error().code == vector.outcome);
        auto destroyed = std::vector<int64_t> {};
        for (auto const rep: recorder->destroyed)
            destroyed.push_back(rep);
        for (auto const& call: host.destructorCalls)
            destroyed.push_back(call.rep);
        CHECK(destroyed
              == (vector.destroyed == NoRep ? std::vector<int64_t> {}
                                            : std::vector<int64_t> { vector.destroyed }));
    }
    resources.tasks().end(task);
}
