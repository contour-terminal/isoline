// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory_resource>
#include <ranges>
#include <string>
#include <vector>

#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/intermediate/Verifier.hpp>
#include <isoline/compiler/optimization/Rewrite.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>

using isoline::compiler::intermediate::BlockId;
using isoline::compiler::intermediate::Builder;
using isoline::compiler::intermediate::describe;
using isoline::compiler::intermediate::Function;
using isoline::compiler::intermediate::Type;
using isoline::compiler::intermediate::ValueId;
using isoline::compiler::intermediate::verify;
using isoline::compiler::optimization::dropParameters;
using isoline::compiler::optimization::incomingEdgesOf;
using isoline::compiler::optimization::ParameterFate;
using isoline::compiler::optimization::parameterPlacesOf;
using isoline::compiler::optimization::removeUnreachableBlocks;
using isoline::compiler::optimization::reversePostorderOf;
using isoline::compiler::optimization::Substitution;

namespace
{

/// b0(x) branches to b1(x, x) or b3(); b1(p, q) jumps to b2(q); b2(r) returns r; b3 returns x; b4, which no
/// block reaches, jumps to b2.
struct Diamond
{
    std::pmr::monotonic_buffer_resource memory { std::pmr::new_delete_resource() };
    Function function { memory };
    Builder builder { function };
    BlockId entry = builder.createBlock();
    BlockId left = builder.createBlock();
    BlockId join = builder.createBlock();
    BlockId right = builder.createBlock();
    BlockId unreached = builder.createBlock();
    ValueId x = builder.addParameter(entry, Type::I32);
    ValueId p = builder.addParameter(left, Type::I32);
    ValueId q = builder.addParameter(left, Type::I32);
    ValueId r = builder.addParameter(join, Type::I32);

    Diamond()
    {
        builder.switchTo(entry);
        builder.branch(x, left, std::array { x, x }, right, {});
        builder.switchTo(left);
        builder.jump(join, std::array { q });
        builder.switchTo(join);
        builder.ret(std::array { r });
        builder.switchTo(right);
        builder.ret(std::array { x });
        builder.switchTo(unreached);
        builder.jump(join, std::array { x });
    }
};

} // namespace

TEST_CASE("Rewrite.substitutionResolvesThroughChains")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto substitution = Substitution { 4, memory };
    CHECK(substitution.isEmpty());
    substitution.replace(ValueId { 0 }, ValueId { 1 });
    substitution.replace(ValueId { 1 }, ValueId { 2 });
    CHECK_FALSE(substitution.isEmpty());
    CHECK(substitution.resolve(ValueId { 0 }) == ValueId { 2 });
    CHECK(substitution.resolve(ValueId { 1 }) == ValueId { 2 });
    CHECK(substitution.resolve(ValueId { 3 }) == ValueId { 3 });
}

TEST_CASE("Contract.Rewrite.substitutionRefusesACycle", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto substitution = Substitution { 2, memory };
    substitution.replace(ValueId { 0 }, ValueId { 1 });
    substitution.replace(ValueId { 1 }, ValueId { 0 });
    FAIL("SURVIVED: a substitution cycle");
}

TEST_CASE("Contract.Rewrite.substitutionReplacesOnce", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto substitution = Substitution { 3, memory };
    substitution.replace(ValueId { 0 }, ValueId { 1 });
    substitution.replace(ValueId { 0 }, ValueId { 2 });
    FAIL("SURVIVED: a value replaced twice");
}

TEST_CASE("Rewrite.walksAndMapsTheFunction")
{
    auto diamond = Diamond {};
    auto& function = diamond.function;
    auto& memory = diamond.memory;
    auto const order = reversePostorderOf(function, memory);
    // The entry first; every block before its successors; the unreachable block absent.
    REQUIRE(order.size() == 4);
    CHECK(order.front() == diamond.entry);
    CHECK(std::ranges::find(order, diamond.left) < std::ranges::find(order, diamond.join));
    CHECK(std::ranges::find(order, diamond.unreached) == order.end());

    auto const places = parameterPlacesOf(function, memory);
    CHECK(places.block[std::to_underlying(diamond.q)] == std::to_underlying(diamond.left));
    CHECK(places.position[std::to_underlying(diamond.q)] == 1);
    CHECK(places.block[std::to_underlying(diamond.r)] == std::to_underlying(diamond.join));

    // join has two edges in, from left and from the unreachable block; the entry none.
    auto const incoming = incomingEdgesOf(function, memory);
    CHECK(incoming.into(diamond.join).size() == 2);
    CHECK(incoming.into(diamond.entry).empty());
    CHECK(incoming.into(diamond.left).size() == 1);
}

TEST_CASE("Rewrite.removesUnreachableBlocksAndDropsParameters")
{
    auto diamond = Diamond {};
    auto& function = diamond.function;
    CHECK(removeUnreachableBlocks(function, diamond.memory) == 1);
    CHECK(function.blockCount() == 4);
    CHECK(removeUnreachableBlocks(function, diamond.memory) == 0);
    // Drop p, which nothing uses: left takes q alone, and the entry's branch passes it x once.
    auto fates = std::vector<ParameterFate>(function.valueCount(), ParameterFate::Kept);
    fates[std::to_underlying(diamond.p)] = ParameterFate::Dropped;
    dropParameters(function, fates);
    CHECK(function.block(diamond.left).parameters.count == 1);
    auto const targets = function.targets(function.instruction(*function.terminator(diamond.entry)));
    REQUIRE(targets.size() == 2);
    CHECK(targets[0].arguments.count == 1);
    CHECK(targets[1].arguments.count == 0);
    // The returns keep their values: they are no arguments.
    CHECK(function.instruction(*function.terminator(diamond.join)).operands.count == 1);
    auto const verified = verify(function);
    INFO((verified ? std::string {} : describe(verified.error())));
    CHECK(verified.has_value());
}

TEST_CASE("Contract.Rewrite.dropsNoParameterOfTheEntry", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto diamond = Diamond {};
    auto fates = std::vector<ParameterFate>(diamond.function.valueCount(), ParameterFate::Kept);
    fates[std::to_underlying(diamond.x)] = ParameterFate::Dropped;
    dropParameters(diamond.function, fates);
    FAIL("SURVIVED: a parameter of the function dropped");
}
