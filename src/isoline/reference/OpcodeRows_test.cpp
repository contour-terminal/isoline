// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/OpcodeRowModules.hpp>
#include <isoline/reference/ReferenceInterpreter.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidatedModule.hpp>
#include <isoline/validate/Validator.hpp>

// One generated module per row of the opcode table (testing/OpcodeRowModules.hpp): each row's
// instruction must decode to that row, validate with the operand and result types its row names, and
// run. A Special row without a fixture fails the test, so a new instruction lands with its module.
// Running it proves the interpreter has a case or a table row for it: one without ends the process.

using isoline::FeatureSet;
using isoline::binary::DecodeLimits;
using isoline::binary::decodeModule;
using isoline::binary::I32;
using isoline::binary::Instruction;
using isoline::binary::memoryAccessOf;
using isoline::binary::OpcodeSignature;
using isoline::binary::OpcodeTable;
using isoline::binary::testing::fixtureOf;
using isoline::binary::testing::fixtureOfSignature;
using isoline::binary::testing::fixtures;
using isoline::binary::testing::moduleOf;
using isoline::validate::ValidatedModule;
using isoline::validate::validateModule;

TEST_CASE("Opcode.everyRowDecodes")
{
    auto const features = FeatureSet::all();
    auto specialRows = std::size_t { 0 };
    for (auto const& row: OpcodeTable)
    {
        INFO(row.text);
        if (row.signature == OpcodeSignature::Special && memoryAccessOf(row.opcode) == nullptr)
            ++specialRows;
        auto const fixture = fixtureOf(row);
        auto validated = ValidatedModule::load(moduleOf(fixture).build(), features, DecodeLimits {});
        REQUIRE(validated.has_value());
        auto const& module = (*validated)->module();
        REQUIRE(module.code.size() == 2);
        // A fixed-signature row's instruction directly follows the local.get of each operand, so it must
        // decode as that very row; a fixture's is somewhere in its body.
        auto const& body = module.code[1].body;
        if (row.signature != OpcodeSignature::Special)
        {
            CHECK(body.at(fixture.params.size()).opcode == row.opcode);
        }
        else
        {
            // The fixture's own instruction, not the final end every body has.
            auto const own = std::span { body }.first(body.size() - 1);
            CHECK(std::ranges::find(own, row.opcode, &Instruction::opcode) != own.end());
        }

        // Any end of its own will do (results, a trap or an exception): only a missing case aborts.
        auto allocator = isoline::runtime::HeapMemoryAllocator {};
        auto store = isoline::runtime::Store { isoline::runtime::StoreLimits {},
                                               isoline::runtime::StoreServices { .allocator = allocator } };
        auto interpreter = isoline::reference::ReferenceInterpreter {};
        auto const instance = isoline::runtime::instantiate(store, interpreter, std::move(*validated), {});
        REQUIRE(instance.has_value());
        auto const function = store.moduleInstance(*instance).functions.at(1);
        auto args = std::vector<isoline::runtime::Value> {};
        std::ranges::transform(fixture.params, std::back_inserter(args), isoline::runtime::defaultValue);
        auto results = std::vector<isoline::runtime::Value>(fixture.results.size());
        // Its arguments are its params' defaults, so the call always enters and the row runs.
        CHECK(isoline::runtime::invokeChecked(store, interpreter, function, args, results).entry
              == isoline::runtime::Entry::Entered);
    }
    // Every fixture serves a row: none is left over from an instruction since removed.
    CHECK(fixtures().size() == specialRows);
}

TEST_CASE("Opcode.everyFixedSignatureRowIsTypedByItsRow")
{
    // The module of a fixed-signature row stops validating when its function claims one more result
    // than the row pushes: the validator types the instruction by its row, not by its surroundings.
    auto const features = FeatureSet::all();
    for (auto const& row: OpcodeTable)
    {
        if (row.signature == OpcodeSignature::Special)
            continue;
        INFO(row.text);
        auto fixture = fixtureOfSignature(row);
        fixture.results.push_back(I32);
        auto const bytes = moduleOf(fixture).build();
        auto const module = decodeModule(bytes, features);
        REQUIRE(module.has_value());
        CHECK_FALSE(validateModule(*module, features).has_value());
    }
}
