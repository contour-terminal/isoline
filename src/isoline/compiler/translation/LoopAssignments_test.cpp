// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/translation/LoopAssignments.hpp>
#include <isoline/runtime/CountingArena.hpp>
#include <isoline/validate/ValidatedModule.hpp>

using isoline::FeatureSet;
using isoline::binary::FuncType;
using isoline::binary::I32;
using isoline::binary::testing::Bytes;
using isoline::binary::testing::code;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::localSet;
using isoline::binary::testing::loop;
using isoline::binary::testing::ModuleBuilder;
using isoline::compiler::CompileErrorCode;
using isoline::compiler::translation::assignedLocalsPerLoop;
using isoline::validate::ValidatedModule;

namespace
{

/// The module of one function of no parameters, @p localCount i32 locals and the body @p body, validated.
std::shared_ptr<ValidatedModule const> moduleOf(uint32_t localCount, Bytes const& body)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).function(0, std::vector(localCount, I32), body);
    auto module = ValidatedModule::load(builder.build(), FeatureSet::all(), isoline::binary::DecodeLimits {});
    REQUIRE(module.has_value());
    return std::move(*module);
}

/// local.set @p local of a constant.
Bytes setLocal(uint32_t local)
{
    return code({ i32Const(0), localSet(local) });
}

/// @p body inside @p depth nested loops, built in linear time.
Bytes nestedLoops(uint32_t depth, Bytes const& body)
{
    auto out = Bytes {};
    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, depth))
        out.insert(out.end(), { 0x03, 0x40 });
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), depth, 0x0B);
    return out;
}

} // namespace

TEST_CASE("LoopAssignments.innermostMergesIntoParent")
{
    // loop { set 1; loop { set 2; set 2 }; block { set 0 } }: the outer loop holds what its children
    // assign, each local once, in ascending order; the block is a construct of its own.
    auto const module = moduleOf(3,
                                 loop(code({ setLocal(1),
                                             loop(code({ setLocal(2), setLocal(2) })),
                                             isoline::binary::testing::block(setLocal(0)) })));
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto const assignments = assignedLocalsPerLoop(module->module().code[0].body, 1'000, memory);
    REQUIRE(assignments.has_value());
    REQUIRE(assignments->constructCount() == 3);
    CHECK(std::ranges::equal(assignments->locals(0), std::vector<uint32_t> { 0, 1, 2 }));
    CHECK(std::ranges::equal(assignments->locals(1), std::vector<uint32_t> { 2 }));
    CHECK(std::ranges::equal(assignments->locals(2), std::vector<uint32_t> { 0 }));
    CHECK(assignments->bodyIndex(0) == 0);
    CHECK(assignments->firstAtOrAfter(1) == 1);
    CHECK(assignments->firstAtOrAfter(assignments->bodyIndex(2) + 1) == 3);
}

TEST_CASE("LoopAssignments.refusesPastTheTotalCap", "[.hostile]")
{
    // 100 nested loops around 50 distinct assignments join 100 × 50 = 5'000 sets: a cap of 5'000 takes
    // them, one of 1'000 refuses at its 1'001st joining. Run capped (tests/CMakeLists.txt), on an arena
    // that refuses past 4 MiB, so that a broken cap fails small.
    auto body = Bytes {};
    for (auto const local: std::views::iota(uint32_t { 0 }, uint32_t { 50 }))
        body = code({ body, setLocal(local) });
    auto const module = moduleOf(50, nestedLoops(100, body));
    auto arena = isoline::runtime::CountingArena { uint64_t { 4 } << 20, *std::pmr::new_delete_resource() };
    auto const refused = assignedLocalsPerLoop(module->module().code[0].body, 1'000, arena);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == CompileErrorCode::TooManyBlockParameters);
    CHECK(refused.error().requested == 1'001);
    CHECK(refused.error().limit == 1'000);
    auto const taken = assignedLocalsPerLoop(module->module().code[0].body, 5'000, arena);
    REQUIRE(taken.has_value());
    CHECK(taken->locals(0).size() == 50);
    CHECK(taken->locals(99).size() == 50);
    CHECK_FALSE(assignedLocalsPerLoop(module->module().code[0].body, 4'999, arena).has_value());
}

TEST_CASE("LoopAssignments.deepNestingIsIterative")
{
    // 100'000 nested loops around one assignment: one link per open loop at a time, no recursion
    // (ASan, the default stack).
    auto const module = moduleOf(1, nestedLoops(100'000, setLocal(0)));
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto const assignments = assignedLocalsPerLoop(module->module().code[0].body, 4'000'000, memory);
    REQUIRE(assignments.has_value());
    CHECK(assignments->constructCount() == 100'000);
    CHECK(std::ranges::equal(assignments->locals(0), std::vector<uint32_t> { 0 }));
    CHECK(std::ranges::equal(assignments->locals(99'999), std::vector<uint32_t> { 0 }));
}
