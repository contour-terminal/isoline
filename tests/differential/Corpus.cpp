// SPDX-License-Identifier: Apache-2.0
#include "Corpus.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "Arguments.hpp"
#include "Comparison.hpp"
#include "DifferentialRunner.hpp"
#include "Observation.hpp"
#include "ProcessRunner.hpp"
#include "WasmtimeOracle.hpp"
#include <isoline/binary/Types.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

namespace isoline::differential
{

namespace
{
    /// The bits of the seed's digest that choose whether a seed's run is interrupted.
    constexpr auto InterruptedSeeds = uint64_t { 2 };

    /// The traps of Isoline's own limits, which Wasmtime's limits do not match: a call that ends on one
    /// is not compared with the oracle.
    constexpr auto OwnLimitTraps = std::to_array<runtime::TrapCode>({
        runtime::TrapCode::StepLimitExceeded,
        runtime::TrapCode::CallStackExhausted,
        runtime::TrapCode::ExceptionLimitExceeded,
        runtime::TrapCode::Interrupted,
    });

    struct LaneName
    {
        Lane lane;
        std::string_view name;
    };
    constexpr auto LaneNames = std::to_array<LaneName>({
        { .lane = Lane::Deterministic, .name = "deterministic" },
        { .lane = Lane::Production, .name = "production" },
    });

    [[nodiscard]] bool endsOnOwnLimit(CallOutcome const& outcome,
                                      runtime::GrowthRecord const& growth) noexcept
    {
        if (growth.memory || growth.table)
            return true;
        auto const* abrupt = std::get_if<runtime::Abrupt>(&outcome);
        if (abrupt == nullptr)
            return false;
        auto const* trap = std::get_if<runtime::Trap>(abrupt);
        return trap != nullptr && std::ranges::find(OwnLimitTraps, trap->code) != OwnLimitTraps.end();
    }

    /// Whether a fresh instance ran the call, or the store's limits refused the instantiation.
    enum class FreshInstance : uint8_t
    {
        Ran,
        RefusedForLimits,
    };

    /// Isoline's outcome of one export call on a fresh instance of the reference interpreter.
    struct FreshCall
    {
        CallOutcome outcome;
        runtime::GrowthRecord growth;
        std::vector<runtime::Value> args;
        FreshInstance instance = FreshInstance::Ran; ///< Whether the call ran.
    };

    [[nodiscard]] FreshCall callFresh(std::shared_ptr<validate::ValidatedModule const> const& validated,
                                      std::string_view name,
                                      binary::FuncType const& type,
                                      std::span<std::byte const> seed,
                                      uint64_t ordinal)
    {
        auto allocator = runtime::HeapMemoryAllocator {};
        auto store = runtime::Store { FuzzLimits, runtime::StoreServices { .allocator = allocator } };
        auto executor = interpreter::ReferenceInterpreter {};
        auto call = FreshCall {};
        auto const instantiated = runtime::instantiateReporting(store, executor, validated, nullptr, {});
        if (!instantiated.instance)
        {
            auto const& error = instantiated.instance.error();
            if (auto const* trap = std::get_if<runtime::Trap>(&error))
                call.outcome = runtime::Abrupt { *trap };
            else if (auto const* thrown = std::get_if<runtime::Thrown>(&error))
                call.outcome = runtime::Abrupt { *thrown };
            else
                call.instance = FreshInstance::RefusedForLimits;
            call.growth = instantiated.refusedGrowth;
            return call;
        }
        auto const& instance = store.moduleInstance(*instantiated.instance);
        auto const exported = std::ranges::find(
            instance.exports, name, [](auto const& entry) { return std::string_view { entry.first }; });
        auto context = ArgumentContext {};
        if (!instance.memories.empty())
            context.memoryBytes = store.memory(instance.memories.front()).bytes().size();
        auto stream = ArgumentStream { seed, ordinal };
        call.args = argumentsFor(stream, type.params, context);
        auto results = std::vector<runtime::Value>(type.results.size());
        auto const invoked =
            runtime::invokeChecked(store, executor, exported->second.address, call.args, results);
        if (invoked.outcome)
            call.outcome = std::move(results);
        else
            call.outcome = invoked.outcome.error();
        call.growth = invoked.refusedGrowth;
        return call;
    }
} // namespace

std::array<std::byte, 8> seedBytes(uint64_t seed) noexcept
{
    auto bytes = std::array<std::byte, 8> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, bytes.size()))
        bytes[index] = static_cast<std::byte>(seed >> (8 * index));
    return bytes;
}

InterruptionPlan interruptionFor(std::span<std::byte const> seed) noexcept
{
    auto stream = ArgumentStream { seed, ~uint64_t { 0 } };
    if (stream.next() % InterruptedSeeds == 0)
        return {};
    return InterruptionPlan { .interruptAtPoll = 1 + (stream.next() % MaxInterruptPoll) };
}

std::optional<Lane> laneNamed(std::string_view name) noexcept
{
    auto const row = std::ranges::find(LaneNames, name, &LaneName::name);
    if (row == LaneNames.end())
        return std::nullopt;
    return row->lane;
}

std::string_view describe(Applicability applicability) noexcept
{
    switch (applicability)
    {
        case Applicability::Compared: return "compared";
        case Applicability::Invalid: return "it does not decode or validate";
        case Applicability::UnsupportedImport:
            return "an import's type is one the recording host cannot satisfy";
    }
    return "?";
}

std::string_view describe(ObservationMode mode) noexcept
{
    return mode == ObservationMode::Observed ? "observed" : "unobserved";
}

std::string_view describe(OracleVerdict verdict) noexcept
{
    switch (verdict)
    {
        case OracleVerdict::Agrees: return "agrees";
        case OracleVerdict::Disagrees: return "disagrees";
        case OracleVerdict::NotCompared: return "not compared";
    }
    return "?";
}

std::expected<std::vector<std::byte>, std::string> readModule(std::filesystem::path const& path)
{
    auto const text = readWholeFile(path);
    if (!text)
        return std::unexpected { text.error() };
    if (path.extension() == ".wat")
    {
        auto compiled = text::compileText(*text, text::TextLimits {});
        if (!compiled)
            return std::unexpected { std::format("{}: does not compile", path.string()) };
        return std::move(*compiled);
    }
    auto bytes = std::vector<std::byte>(text->size());
    std::ranges::transform(
        *text, bytes.begin(), [](char character) { return static_cast<std::byte>(character); });
    return bytes;
}

std::vector<std::pair<std::filesystem::path, uint64_t>> corpusModules(std::filesystem::path const& directory)
{
    auto modules = std::vector<std::pair<std::filesystem::path, uint64_t>> {};
    auto error = std::error_code {};
    for (auto const& entry: std::filesystem::directory_iterator { directory, error })
    {
        if (entry.path().extension() != ".wasm")
            continue;
        auto const stem = entry.path().stem().string();
        auto seed = uint64_t {};
        auto const [end, parsed] = std::from_chars(stem.data(), stem.data() + stem.size(), seed);
        if (parsed == std::errc {} && end == stem.data() + stem.size())
            modules.emplace_back(entry.path(), seed);
    }
    std::ranges::sort(modules, {}, &std::pair<std::filesystem::path, uint64_t>::second);
    return modules;
}

std::vector<OracleComparison> compareWithOracle(WasmtimeOracle const& oracle,
                                                std::span<std::byte const> module,
                                                std::span<std::byte const> seed)
{
    auto comparisons = std::vector<OracleComparison> {};
    auto const validated = loadForComparison(module);
    if (!validated || !(*validated)->module().imports.empty())
        return comparisons;
    auto const& decoded = (*validated)->module();
    auto ordinal = uint64_t { 0 };
    for (auto const& entry: decoded.exports)
    {
        if (entry.kind != binary::ExternKind::Func)
            continue;
        auto const& type = decoded.types[decoded.functions[entry.index]];
        if (!isOracleEligible(decoded, type))
            continue;
        auto& comparison = comparisons.emplace_back(OracleComparison {
            .exportName = entry.name, .verdict = OracleVerdict::NotCompared, .detail = {} });
        auto const call = callFresh(*validated, entry.name, type, seed, ordinal++);
        if (call.instance == FreshInstance::RefusedForLimits || endsOnOwnLimit(call.outcome, call.growth))
        {
            comparison.detail = std::format("Isoline ended on its own limit: {}", describe(call.outcome));
            continue;
        }
        auto const answer = oracle.call(module, entry.name, call.args);
        if (!answer)
        {
            comparison.detail = std::format(
                "no answer ({}): {}", std::to_underlying(answer.error().kind), answer.error().detail);
            continue;
        }
        comparison.verdict =
            agreesWithOracle(*answer, call.outcome) ? OracleVerdict::Agrees : OracleVerdict::Disagrees;
        auto const oracleText = std::visit(
            []<typename T>(T const& alternative) -> std::string {
                if constexpr (std::is_same_v<T, std::vector<runtime::Value>>)
                    return "results " + describe(std::span<runtime::Value const> { alternative });
                else if constexpr (std::is_same_v<T, OracleTrap>)
                    return std::format("trap '{}'", alternative.message);
                else
                    return std::string { "thrown exception" };
            },
            *answer);
        comparison.detail = std::format("arguments {}: Isoline {}, Wasmtime {}",
                                        describe(std::span<runtime::Value const> { call.args }),
                                        describe(call.outcome),
                                        oracleText);
    }
    return comparisons;
}

} // namespace isoline::differential
