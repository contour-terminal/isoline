// SPDX-License-Identifier: Apache-2.0
#include "GuestRun.hpp"

#include <core/Overloaded.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/runtime/Trap.hpp>

namespace isoline::benchmarks
{

namespace
{

    /// The name of each step a run can fail at, in GuestRunError's order.
    constexpr auto GuestRunErrorNames =
        std::to_array<std::string_view>({ "load", "instantiate", "export", "call" });
    static_assert(GuestRunErrorNames.size() == static_cast<std::size_t>(GuestRunError::CallFailed) + 1);

    /// What each kind of load error is, in api::LoadError's order.
    constexpr auto LoadErrorNames =
        std::to_array<std::string_view>({ "the module is too large",
                                          "out of host memory",
                                          "the module is malformed",
                                          "the module is invalid",
                                          "lowering it is past a compile limit" });
    static_assert(LoadErrorNames.size() == std::variant_size_v<api::LoadError>);

    /// What each way a call can fail is, in api::CallFailure's order.
    constexpr auto CallFailureNames = std::to_array<std::string_view>({ "trap",
                                                                        "interrupted",
                                                                        "host failure",
                                                                        "uncaught exception",
                                                                        "argument mismatch",
                                                                        "foreign handle",
                                                                        "store deleted" });
    static_assert(CallFailureNames.size() == static_cast<std::size_t>(api::CallFailure::StoreDeleted) + 1);

    /// A run that failed at @p error for @p detail.
    std::unexpected<GuestRunFailure> failure(GuestRunError error, std::string detail)
    {
        return std::unexpected { GuestRunFailure { .error = error, .detail = std::move(detail) } };
    }

    /// How @p error reads in a report: its failure, and its trap's message where it trapped.
    std::string describe(api::CallError const& error)
    {
        auto description = std::string { CallFailureNames[static_cast<std::size_t>(error.failure())] };
        if (auto const* const trapped = std::get_if<runtime::Trap>(&error.detail))
            description += std::format(": {}", runtime::specMessage(*trapped));
        return description;
    }

    /// Splits @p text into its lines, without their line ends.
    std::vector<std::string_view> linesOf(std::string_view text)
    {
        auto lines = std::vector<std::string_view> {};
        for (auto const line: text | std::views::split('\n'))
        {
            auto view = std::string_view { line.begin(), line.end() };
            if (view.ends_with('\r'))
                view.remove_suffix(1);
            lines.push_back(view);
        }
        return lines;
    }

    /// @p text without its leading and trailing blanks.
    std::string_view trimmed(std::string_view text)
    {
        auto const first = text.find_first_not_of(" \t");
        if (first == std::string_view::npos)
            return {};
        auto const last = text.find_last_not_of(" \t");
        return text.substr(first, last - first + 1);
    }

    /// Whether @p text is a SHA-256 in lowercase hex.
    bool isSha256(std::string_view text)
    {
        return text.size() == 64 && std::ranges::all_of(text, [](char c) {
                   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
               });
    }

    /// The suffix that makes a guest's hash row its features row.
    constexpr auto FeaturesSuffix = std::string_view { ".features" };

} // namespace

std::string_view nameOf(GuestRunError error) noexcept
{
    return GuestRunErrorNames[static_cast<std::size_t>(error)];
}

std::expected<GuestRunResult, GuestRunFailure> runGuest(api::EngineConfig const& config,
                                                        GuestRunRequest const& request,
                                                        core::IClock const& clock)
{
    auto const engine = api::Engine { config };
    auto allocator = api::HeapMemoryAllocator {};

    auto const loadStart = clock.now();
    auto const module = engine.load(request.module);
    if (!module)
        return failure(GuestRunError::LoadFailed, std::string { LoadErrorNames[module.error().index()] });
    auto store = api::Store { engine, allocator };
    auto const instance = store.instantiate(*module, api::Imports {});
    if (!instance)
        return failure(GuestRunError::InstantiateFailed,
                       std::format("instantiation failed (error kind {})", instance.error().index()));
    auto const load = clock.now() - loadStart;

    auto const run = instance->function("run");
    if (!run)
        return failure(GuestRunError::NoRunExport, "the guest exports no function named run");

    auto const args = std::array { api::Value { api::I32 { request.iterations } } };
    auto results = std::array { api::Value { api::I64 {} } };
    auto const invokeStart = clock.now();
    for ([[maybe_unused]] auto const invocation: std::views::iota(uint32_t { 0 }, request.invocations))
    {
        auto const called = store.call(*run, args, results);
        if (!called)
            return failure(GuestRunError::CallFailed, describe(called.error()));
    }
    auto const invoke = clock.now() - invokeStart;

    auto const* const checksum = std::get_if<api::I64>(results.data());
    if (checksum == nullptr)
        return failure(GuestRunError::CallFailed, "run() did not return an i64");
    return GuestRunResult { .checksum = checksum->bits, .load = load, .invoke = invoke };
}

std::expected<std::vector<GuestRow>, GuestLockError> parseGuestLock(std::string_view text)
{
    // Each guest with the line of its hash row, which a missing features row is reported at.
    auto hashRows = std::vector<std::pair<GuestRow, std::size_t>> {};
    auto features = std::vector<std::pair<std::string, std::string>> {};
    auto lineNumber = std::size_t { 0 };
    for (auto const line: linesOf(text))
    {
        ++lineNumber;
        auto const content = trimmed(line.substr(0, line.find('#')));
        if (content.empty())
            continue;
        auto const equals = content.find('=');
        if (equals == std::string_view::npos)
            return std::unexpected { GuestLockError { .line = lineNumber,
                                                      .reason = "not a `key = value` row" } };
        auto const key = trimmed(content.substr(0, equals));
        auto const value = trimmed(content.substr(equals + 1));
        if (key.ends_with(FeaturesSuffix))
        {
            features.emplace_back(std::string { key.substr(0, key.size() - FeaturesSuffix.size()) },
                                  std::string { value });
            continue;
        }
        if (!key.ends_with(".wasm"))
            return std::unexpected { GuestLockError { .line = lineNumber,
                                                      .reason = "the key names no .wasm guest" } };
        if (!isSha256(value))
            return std::unexpected { GuestLockError {
                .line = lineNumber, .reason = "the value is not a SHA-256 in lowercase hex" } };
        hashRows.emplace_back(
            GuestRow { .file = std::string { key }, .sha256 = std::string { value }, .features = {} },
            lineNumber);
    }
    auto rows = std::vector<GuestRow> {};
    for (auto& [row, rowLine]: hashRows)
    {
        auto const found = std::ranges::find(features, row.file, &std::pair<std::string, std::string>::first);
        if (found == features.end())
            return std::unexpected { GuestLockError {
                .line = rowLine, .reason = std::format("{} has no features row", row.file) } };
        row.features = found->second;
        rows.push_back(std::move(row));
    }
    return rows;
}

bool usesSimd(GuestRow const& row) noexcept
{
    for (auto const name: std::string_view { row.features } | std::views::split(','))
    {
        if (trimmed(std::string_view { name.begin(), name.end() }) == "simd")
            return true;
    }
    return false;
}

} // namespace isoline::benchmarks
