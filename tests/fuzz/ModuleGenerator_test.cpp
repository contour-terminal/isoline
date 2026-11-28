// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "ModuleGenerator.hpp"
#include <Comparison.hpp>
#include <Corpus.hpp>
#include <DifferentialRunner.hpp>
#include <Observation.hpp>
#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/interpreter/Code.hpp>
#include <isoline/interpreter/Dispatch.hpp>
#include <isoline/interpreter/Lowering.hpp>
#include <isoline/runtime/CountingArena.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/validate/ValidatedModule.hpp>
#include <isoline/validate/ValidationError.hpp>

using namespace isoline;
using isoline::fuzz::generateModule;
using isoline::fuzz::GeneratorFeatures;
using isoline::fuzz::GeneratorLimits;
using isoline::fuzz::GeneratorMode;
using isoline::fuzz::maxFunctionInstructions;
using isoline::fuzz::maxModuleBytes;

namespace
{

/// How many fixed-seed entropy buffers the cases generate from.
constexpr auto SeedCount = uint32_t { 5'000 };

/// The first of them whose modules are run to see which poll sites they reach: running is slow, and the
/// program shapes reach every site within the first few dozen.
constexpr auto PollSeedCount = uint32_t { 200 };

/// The longest of them.
constexpr auto MaxEntropyBytes = uint64_t { 4'096 };

/// A SplitMix64 stream: the fixed seeds' bytes, the same on every host.
class SplitMix
{
  public:
    explicit SplitMix(uint64_t seed): _state { seed } {}

    [[nodiscard]] uint64_t next() noexcept
    {
        _state += 0x9E37'79B9'7F4A'7C15U;
        auto mixed = _state;
        mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9U;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EBU;
        return mixed ^ (mixed >> 31U);
    }

  private:
    uint64_t _state;
};

/// @p count bytes of @p value.
[[nodiscard]] std::vector<std::byte> filled(std::size_t count, uint8_t value)
{
    return std::vector<std::byte>(count, std::byte { value });
}

/// @p count bytes of the SplitMix stream of @p seed.
[[nodiscard]] std::vector<std::byte> randomBytes(std::size_t count, uint64_t seed)
{
    auto stream = SplitMix { seed };
    auto bytes = std::vector<std::byte> {};
    bytes.reserve(count);
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, count))
        bytes.push_back(static_cast<std::byte>(stream.next() & 0xFFU));
    return bytes;
}

/// Fixed-seed buffer @p index: the empty one, MaxEntropyBytes of 0xFF and of 0x00, then random bytes of a
/// random length up to MaxEntropyBytes.
[[nodiscard]] std::vector<std::byte> entropyOf(uint32_t index)
{
    switch (index)
    {
        case 0: return {};
        case 1: return filled(MaxEntropyBytes, 0xFF);
        case 2: return filled(MaxEntropyBytes, 0x00);
        default: break;
    }
    auto lengths = SplitMix { index };
    return randomBytes(lengths.next() % (MaxEntropyBytes + 1), index);
}

/// Why a module did not load, as text.
[[nodiscard]] std::string describe(validate::ModuleError const& error)
{
    if (auto const* validation = std::get_if<validate::ValidationError>(&error))
        return validate::specMessage(*validation) + " in function " + std::to_string(validation->function)
               + " at instruction " + std::to_string(validation->instruction);
    return binary::specMessage(std::get<binary::DecodeError>(error));
}

/// @p module, loaded under the generator's features; fails the case when it does not load.
[[nodiscard]] std::shared_ptr<validate::ValidatedModule const> loaded(std::span<std::byte const> module,
                                                                      uint32_t index)
{
    auto result = validate::ValidatedModule::load(module, GeneratorFeatures, binary::DecodeLimits {});
    if (!result)
        UNSCOPED_INFO("entropy " << index << ": " << describe(result.error()));
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// The OpcodeTable rows the generator must reach: every instruction its features decode.
[[nodiscard]] std::set<binary::Opcode> rowsInTheCut()
{
    auto rows = std::set<binary::Opcode> {};
    for (auto const& row: binary::OpcodeTable)
        if (GeneratorFeatures.includes(row.required))
            rows.insert(row.opcode);
    return rows;
}

/// Every OperationForm enumerator.
[[nodiscard]] std::set<interpreter::OperationForm> everyOperationForm()
{
    using enum interpreter::OperationForm;
    return { Nullary,     Unary, Binary, BinaryImmediate, Move, Load, Store, Branch, BranchMove,
             BranchTable, Call,  Bulk,   Special };
}

/// The forms of the operations @p module lowers to, under the Switch dispatch.
[[nodiscard]] std::set<interpreter::OperationForm> loweredForms(validate::ValidatedModule const& module)
{
    auto const lowered =
        interpreter::lower(module,
                           interpreter::LoweringConfig { .mode = runtime::ExecutionMode::Deterministic,
                                                         .dispatch = interpreter::Dispatch::Switch },
                           std::make_shared<runtime::CountingArena>(differential::LoweringBudget,
                                                                    *std::pmr::new_delete_resource()));
    REQUIRE(lowered.has_value());
    auto forms = std::set<interpreter::OperationForm> {};
    for (auto const& function: (*lowered)->functions())
    {
        auto const code = std::span<interpreter::Word const> { function.code };
        auto offset = std::size_t { 0 };
        while (offset < code.size())
        {
            auto const operation = interpreter::operationOfHeader(code[offset]);
            forms.insert(interpreter::operationRow(operation).form);
            offset += interpreter::operationExtent(operation, code.subspan(offset));
        }
    }
    return forms;
}

/// The poll sites the reference reached in @p module's observed run.
[[nodiscard]] std::set<runtime::PollSite> reachedPollSites(std::span<std::byte const> module, uint32_t index)
{
    auto participants = std::vector<differential::Participant> {};
    participants.push_back(differential::referenceParticipant());
    participants.push_back(differential::interpreterParticipant(interpreter::Dispatch::Switch));
    auto const runner = differential::DifferentialRunner {
        std::move(participants), differential::Lane::Deterministic, differential::FuzzLimits, {}
    };
    auto const observations = runner.observe(module, differential::seedBytes(index));
    REQUIRE(observations.has_value());
    auto sites = std::set<runtime::PollSite> {};
    auto const collect = [&](differential::Observation const& observation) {
        for (auto const site:
             std::views::iota(uint8_t { 0 }, uint8_t { std::to_underlying(runtime::LastPollSite) + 1 }))
            if (observation.polls[site] != 0)
                sites.insert(static_cast<runtime::PollSite>(site));
    };
    for (auto const& observation: *observations)
    {
        if (observation.mode != differential::ObservationMode::Observed || !observation.observation)
            continue;
        collect(observation.observation->instantiated);
        for (auto const& call: observation.observation->calls)
            collect(call);
    }
    return sites;
}

/// Every PollSite enumerator.
[[nodiscard]] std::set<runtime::PollSite> everyPollSite()
{
    auto sites = std::set<runtime::PollSite> {};
    for (auto const site:
         std::views::iota(uint8_t { 0 }, uint8_t { std::to_underlying(runtime::LastPollSite) + 1 }))
        sites.insert(static_cast<runtime::PollSite>(site));
    return sites;
}

/// A shape of branch to a loop's label: each must be generated, so that the interpreter's loop polls are
/// reached from every kind of transfer.
enum class LoopBranch : uint8_t
{
    BrIf,
    FusedBrIf, ///< A comparison right before the br_if, which the interpreter fuses.
    BrTable,
    BrOnNull,
    Catch,
};

/// Whether @p row is a comparison of two numbers to an i32.
[[nodiscard]] bool isComparison(binary::OpcodeInfo const& row)
{
    using enum binary::OpcodeSignature;
    auto const relation = row.text.substr(row.text.find('.') + 1);
    auto const isBinaryToI32 = row.signature == I32I32ToI32 || row.signature == I64I64ToI32
                               || row.signature == F32F32ToI32 || row.signature == F64F64ToI32;
    return isBinaryToI32
           && std::ranges::any_of(std::to_array<std::string_view>({ "eq", "ne", "lt", "gt", "le", "ge" }),
                                  [&](std::string_view prefix) { return relation.starts_with(prefix); });
}

/// The branches to a loop's label @p function holds, by shape.
[[nodiscard]] std::set<LoopBranch> loopBranches(binary::Function const& function)
{
    using binary::Opcode;
    // The opcode that opened each enclosing block, innermost last; the function's own block first.
    auto blocks = std::vector<Opcode> { Opcode::Block };
    auto const isLoop = [&](uint32_t label) {
        return label < blocks.size() && blocks[blocks.size() - 1 - label] == Opcode::Loop;
    };
    auto found = std::set<LoopBranch> {};
    auto previous = Opcode::Nop;
    for (auto const& instruction: function.body)
    {
        switch (instruction.opcode)
        {
            case Opcode::BrIf:
                if (isLoop(instruction.a))
                    found.insert(isComparison(binary::opcodeInfo(previous)) ? LoopBranch::FusedBrIf
                                                                            : LoopBranch::BrIf);
                break;
            case Opcode::BrTable:
                for (auto const label: binary::labelsOf(function, instruction))
                    if (isLoop(label))
                        found.insert(LoopBranch::BrTable);
                break;
            case Opcode::BrOnNull:
                if (isLoop(instruction.a))
                    found.insert(LoopBranch::BrOnNull);
                break;
            case Opcode::TryTable:
                for (auto const& clause: binary::catchClausesOf(function, instruction))
                    if (isLoop(clause.label))
                        found.insert(LoopBranch::Catch);
                blocks.push_back(Opcode::TryTable);
                break;
            case Opcode::Block:
            case Opcode::Loop:
            case Opcode::If: blocks.push_back(instruction.opcode); break;
            case Opcode::End: blocks.pop_back(); break;
            default: break;
        }
        previous = instruction.opcode;
    }
    return found;
}

} // namespace

TEST_CASE("ModuleGenerator.alwaysValidates")
{
    for (auto const index: std::views::iota(uint32_t { 0 }, SeedCount))
    {
        auto const module = generateModule(entropyOf(index), GeneratorLimits {});
        [[maybe_unused]] auto const validated = loaded(module, index);
    }
}

TEST_CASE("ModuleGenerator.isDeterministic")
{
    constexpr auto Seeds = uint32_t { 200 };
    // By digest: distinct digests are distinct modules.
    auto distinct = std::set<uint64_t> {};
    for (auto const index: std::views::iota(uint32_t { 0 }, Seeds))
    {
        auto const entropy = entropyOf(index);
        auto const first = generateModule(entropy, GeneratorLimits {});
        CHECK(first == generateModule(entropy, GeneratorLimits {}));
        distinct.insert(differential::memoryDigest(first));
    }
    // The entropy drives the module: different buffers build different modules, short ones aside.
    CHECK(distinct.size() >= Seeds * 9 / 10);
}

TEST_CASE("ModuleGenerator.coversEveryOpcodeRow")
{
    auto missingRows = rowsInTheCut();
    auto missingForms = everyOperationForm();
    auto missingSites = everyPollSite();
    for (auto const index: std::views::iota(uint32_t { 0 }, SeedCount))
    {
        if (missingRows.empty() && missingForms.empty() && (missingSites.empty() || index >= PollSeedCount))
            break;
        auto const module = generateModule(entropyOf(index), GeneratorLimits {});
        auto const validated = loaded(module, index);
        for (auto const& function: validated->module().code)
            for (auto const& instruction: function.body)
                missingRows.erase(instruction.opcode);
        for (auto const form: loweredForms(*validated))
            missingForms.erase(form);
        if (!missingSites.empty() && index < PollSeedCount)
            for (auto const site: reachedPollSites(module, index))
                missingSites.erase(site);
    }
    for (auto const opcode: missingRows)
        UNSCOPED_INFO("never generated: " << binary::opcodeInfo(opcode).text);
    for (auto const form: missingForms)
        UNSCOPED_INFO("never lowered to: OperationForm " << int { std::to_underlying(form) });
    for (auto const site: missingSites)
        UNSCOPED_INFO("never polled at: PollSite " << int { std::to_underlying(site) });
    CHECK(missingRows.empty());
    CHECK(missingForms.empty());
    CHECK(missingSites.empty());
}

TEST_CASE("ModuleGenerator.boundedOutput")
{
    constexpr auto EntropyBytes = std::size_t { 64 } << 10;
    auto const limitRows = std::to_array<GeneratorLimits>({
        GeneratorLimits {},
        { .maxFunctions = 0, .maxBodyInstructions = 400, .maxNesting = 16 },
        { .maxFunctions = 1, .maxBodyInstructions = 0, .maxNesting = 0 },
        { .maxFunctions = 2, .maxBodyInstructions = 40, .maxNesting = 2 },
        { .maxFunctions = 3, .maxBodyInstructions = 2'000, .maxNesting = 40 },
    });
    auto const entropies = std::to_array<std::vector<std::byte>>({
        filled(EntropyBytes, 0xFF),
        filled(EntropyBytes, 0x00),
        filled(EntropyBytes, 0x80),
        randomBytes(EntropyBytes, 1),
        randomBytes(EntropyBytes, 2),
        randomBytes(EntropyBytes, 3),
    });
    for (auto const& limits: limitRows)
    {
        auto longestBody = uint64_t { 0 };
        for (auto const& entropy: entropies)
        {
            auto const module = generateModule(entropy, limits);
            CHECK(module.size() <= maxModuleBytes(limits));
            auto const validated = loaded(module, limits.maxBodyInstructions);
            CHECK(validated->module().code.size() <= limits.maxFunctions);
            for (auto const& function: validated->module().code)
            {
                CHECK(function.body.size() <= maxFunctionInstructions(limits));
                longestBody = std::max(longestBody, uint64_t { function.body.size() });
            }
        }
        // The bound is not vacuous: 64 KiB of entropy fills a body to most of its budget.
        if (limits.maxFunctions != 0)
            CHECK(longestBody * 2 >= limits.maxBodyInstructions);
    }
}

TEST_CASE("ModuleGenerator.branchesToLoopsInEveryShape")
{
    auto missing = std::set<LoopBranch> {
        LoopBranch::BrIf, LoopBranch::FusedBrIf, LoopBranch::BrTable, LoopBranch::BrOnNull, LoopBranch::Catch
    };
    for (auto const index: std::views::iota(uint32_t { 0 }, SeedCount))
    {
        if (missing.empty())
            break;
        auto const module = generateModule(entropyOf(index), GeneratorLimits {});
        for (auto const& function: loaded(module, index)->module().code)
            for (auto const shape: loopBranches(function))
                missing.erase(shape);
    }
    for (auto const shape: missing)
        UNSCOPED_INFO("never branched to a loop as LoopBranch " << int { std::to_underlying(shape) });
    CHECK(missing.empty());
}

TEST_CASE("ModuleGenerator.oracleModeImportsNothing")
{
    constexpr auto Seeds = uint32_t { 500 };
    auto numberParams = uint64_t { 0 };
    for (auto const index: std::views::iota(uint32_t { 0 }, Seeds))
    {
        auto const module = generateModule(entropyOf(index), GeneratorLimits {}, GeneratorMode::Oracle);
        auto const validated = loaded(module, index);
        auto const& decoded = validated->module();
        CHECK(decoded.imports.empty());
        // Every export takes numbers and returns numbers or vectors, as the oracle passes them.
        for (auto const& function: decoded.code)
        {
            auto const& type = decoded.types[function.typeIndex];
            for (auto const& param: type.params)
                CHECK(param.kind != binary::ValueKind::V128);
            for (auto const& param: type.params)
                CHECK(param.kind != binary::ValueKind::Ref);
            for (auto const& result: type.results)
                CHECK(result.kind != binary::ValueKind::Ref);
            numberParams += type.params.size();
        }
    }
    CHECK(numberParams > 0);
}
