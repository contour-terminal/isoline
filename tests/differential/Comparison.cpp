// SPDX-License-Identifier: Apache-2.0
#include "Comparison.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "Observation.hpp"
#include <isoline/binary/Types.hpp>
#include <isoline/fastinterpreter/LittleEndian.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::differential
{

namespace
{
    /// FNV-1a's 64-bit offset basis and prime.
    constexpr auto DigestBasis = uint64_t { 0xcbf2'9ce4'8422'2325 };
    constexpr auto DigestPrime = uint64_t { 0x0000'0100'0000'01b3 };

    /// The name of a reference's kind, as the text format spells its instruction.
    [[nodiscard]] std::string_view kindName(runtime::RefKind kind) noexcept
    {
        switch (kind)
        {
            case runtime::RefKind::Null: return "ref.null";
            case runtime::RefKind::Func: return "ref.func";
            case runtime::RefKind::Extern: return "ref.extern";
            case runtime::RefKind::Exn: return "ref.exn";
        }
        return "ref.?";
    }

    [[nodiscard]] std::string describe(runtime::Ref const& reference)
    {
        return std::format("{} heap {}:{} @{}",
                           kindName(reference.kind),
                           std::to_underlying(reference.heap.kind),
                           reference.heap.typeIndex,
                           reference.address);
    }

    [[nodiscard]] std::string describe(runtime::ValueMismatch const& mismatch)
    {
        return std::format("mismatch {} at {}", std::to_underlying(mismatch.kind), mismatch.index);
    }

    /// Items as text, comma-separated in brackets.
    template <typename Items, typename Show>
    [[nodiscard]] std::string listOf(Items const& items, Show const& show)
    {
        auto text = std::string { "[" };
        auto separator = std::string_view {};
        for (auto const& item: items)
        {
            text += separator;
            text += show(item);
            separator = ", ";
        }
        return text + "]";
    }

    /// The text of a field's value in one call: the call's export, then the value.
    [[nodiscard]] std::string inCall(Observation const& observation, std::string const& value)
    {
        return std::format("{}: {}", observation.exportName, value);
    }
} // namespace

uint64_t memoryDigest(std::span<std::byte const> bytes) noexcept
{
    // Eight bytes a step, each word assembled little-endian so that the digest is the same on every host: a
    // memory of 64 MiB is digested after every call.
    auto digest = DigestBasis;
    auto const words = bytes.size() / sizeof(uint64_t);
    for (auto const index: std::views::iota(std::size_t { 0 }, words))
    {
        auto const word =
            fastinterpreter::loadLittleEndian<uint64_t>(bytes.data() + (index * sizeof(uint64_t)));
        digest = (digest ^ word) * DigestPrime;
        digest ^= digest >> 29U;
    }
    for (auto const byte: bytes.subspan(words * sizeof(uint64_t)))
        digest = (digest ^ std::to_integer<uint64_t>(byte)) * DigestPrime;
    return (digest ^ uint64_t { bytes.size() }) * DigestPrime;
}

std::string describe(runtime::Value const& value)
{
    return std::visit(
        []<typename T>(T const& alternative) -> std::string {
            if constexpr (std::is_same_v<T, runtime::I32>)
                return std::format("i32:{:#010x}", alternative.bits);
            else if constexpr (std::is_same_v<T, runtime::I64>)
                return std::format("i64:{:#018x}", alternative.bits);
            else if constexpr (std::is_same_v<T, runtime::F32>)
                return std::format("f32:{:#010x}", alternative.bits);
            else if constexpr (std::is_same_v<T, runtime::F64>)
                return std::format("f64:{:#018x}", alternative.bits);
            else if constexpr (std::is_same_v<T, runtime::V128>)
                return std::format("v128:{:#018x}:{:#018x}", alternative.lanes[1], alternative.lanes[0]);
            else
                return describe(alternative);
        },
        value);
}

std::string describe(std::span<runtime::Value const> values)
{
    return listOf(values, [](runtime::Value const& value) { return describe(value); });
}

std::string describe(runtime::Abrupt const& abrupt)
{
    return std::visit(
        []<typename T>(T const& alternative) -> std::string {
            if constexpr (std::is_same_v<T, runtime::Trap>)
            {
                if (alternative.subject)
                    return std::format(
                        "trap '{}' subject {}", runtime::specMessage(alternative.code), *alternative.subject);
                return std::format("trap '{}'", runtime::specMessage(alternative.code));
            }
            else if constexpr (std::is_same_v<T, runtime::Thrown>)
                return std::format("thrown exception @{}", alternative.exception);
            else
            {
                static_assert(std::is_same_v<T, runtime::HostFailure>,
                              "every Abrupt alternative is described");
                return std::format("host failure {} code {}{}",
                                   std::to_underlying(alternative.kind),
                                   alternative.code,
                                   alternative.mismatch ? " " + describe(*alternative.mismatch)
                                                        : std::string {});
            }
        },
        abrupt);
}

std::string describe(CallOutcome const& outcome)
{
    if (auto const* results = std::get_if<std::vector<runtime::Value>>(&outcome))
        return "results " + describe(std::span<runtime::Value const> { *results });
    return describe(std::get<runtime::Abrupt>(outcome));
}

std::string describe(runtime::InstantiationError const& error)
{
    return std::visit(
        []<typename T>(T const& alternative) -> std::string {
            if constexpr (std::is_same_v<T, runtime::LinkError>)
                return std::format("link error '{}' at import {}",
                                   runtime::specMessage(alternative.code),
                                   alternative.importIndex);
            else if constexpr (std::is_same_v<T, runtime::LimitExceeded>)
                return runtime::describe(alternative);
            else if constexpr (std::is_same_v<T, runtime::AllocationFailure>)
                return std::format("allocation of extern kind {} refused: {}",
                                   std::to_underlying(alternative.kind),
                                   runtime::describe(alternative.reason));
            else
                return describe(runtime::Abrupt { alternative });
        },
        error);
}

std::string describe(runtime::GrowthRecord const& record)
{
    auto const reason = [](std::optional<runtime::Refusal> const& refusal) {
        return refusal ? std::string { runtime::describe(*refusal) } : std::string { "none" };
    };
    return std::format("memory {}, table {}", reason(record.memory), reason(record.table));
}

std::string describePolls(std::span<uint64_t const> polls)
{
    auto text = std::string {};
    auto separator = std::string_view {};
    for (auto const& row: runtime::PollSiteTable)
    {
        text += std::format("{}{} {}", separator, row.name, polls[std::to_underlying(row.site)]);
        separator = ", ";
    }
    return text;
}

std::string describe(std::optional<ThrownException> const& thrown)
{
    if (!thrown)
        return "none";
    return std::format(
        "tag @{} payload {}", thrown->tag, describe(std::span<runtime::Value const> { thrown->payload }));
}

std::string describe(std::span<std::vector<runtime::Ref> const> tables)
{
    // Each run of equal elements once, with its length: a table of thousands of nulls is one item.
    return listOf(tables, [](std::vector<runtime::Ref> const& table) {
        auto runs = std::vector<std::pair<runtime::Ref, std::size_t>> {};
        for (auto const& element: table)
        {
            if (!runs.empty() && runs.back().first == element)
                ++runs.back().second;
            else
                runs.emplace_back(element, 1);
        }
        return listOf(runs, [](std::pair<runtime::Ref, std::size_t> const& run) {
            return run.second == 1 ? describe(run.first)
                                   : std::format("{} x{}", describe(run.first), run.second);
        });
    });
}

std::string describe(std::span<uint64_t const> items)
{
    return listOf(items, [](uint64_t item) { return std::format("{:#018x}", item); });
}

std::string describe(std::span<std::string const> items)
{
    return listOf(items, [](std::string const& item) { return item; });
}

std::optional<FieldDifference> compare(ModuleObservation const& expected, ModuleObservation const& actual)
{
    if (expected.instantiation != actual.instantiation)
        return FieldDifference { .field = "instantiation",
                                 .expected = expected.instantiation,
                                 .actual = actual.instantiation };
    auto const compareCall = [](Observation const& expectedCall,
                                Observation const& actualCall) -> std::optional<FieldDifference> {
        for (auto const& field: ObservationFields)
        {
            if (!field.agree(expectedCall, actualCall))
                return FieldDifference { .field = std::string { field.name },
                                         .expected = inCall(expectedCall, field.show(expectedCall)),
                                         .actual = inCall(actualCall, field.show(actualCall)) };
        }
        return std::nullopt;
    };
    if (auto difference = compareCall(expected.instantiated, actual.instantiated))
        return difference;
    for (auto const index:
         std::views::iota(std::size_t { 0 }, std::min(expected.calls.size(), actual.calls.size())))
    {
        if (auto difference = compareCall(expected.calls[index], actual.calls[index]))
            return difference;
    }
    if (expected.calls.size() != actual.calls.size())
        return FieldDifference { .field = "calls",
                                 .expected = std::to_string(expected.calls.size()),
                                 .actual = std::to_string(actual.calls.size()) };
    return std::nullopt;
}

std::string describe(Observation const& observation)
{
    auto text = std::string {};
    for (auto const& field: ObservationFields)
        text += std::format("  {}: {}\n", field.name, field.show(observation));
    return text;
}

std::string describe(ModuleObservation const& observation)
{
    auto text = std::format("instantiation: {}\n",
                            observation.instantiation.empty() ? "ok" : observation.instantiation);
    text += describe(observation.instantiated);
    for (auto const& call: observation.calls)
        text += std::format("call {}\n{}", call.exportName, describe(call));
    return text;
}

} // namespace isoline::differential
