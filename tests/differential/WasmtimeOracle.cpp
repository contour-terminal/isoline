// SPDX-License-Identifier: Apache-2.0
#include "WasmtimeOracle.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "Comparison.hpp"
#include "Observation.hpp"
#include "TrapMapping.hpp"
#include <isoline/Features.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::differential
{

namespace
{
    /// What precedes a trap's message in Wasmtime's error.
    constexpr auto TrapPrefix = std::string_view { "wasm trap: " };
    /// What Wasmtime reports for an exception that escapes the call.
    constexpr auto ThrownMessage = std::string_view { "thrown Wasm exception" };
    /// The status a shell, and a child that could not execute its program, exits with.
    constexpr auto CannotExecute = 127;
    /// Wasmtime's trap when its timeout (an epoch interrupt) ends the call.
    constexpr auto InterruptMessage = std::string_view { "interrupt" };
    /// The canonical NaNs, which Wasmtime's command line spells `nan` and prints as `NaN`.
    constexpr auto CanonicalF32 = uint32_t { 0x7FC0'0000 };
    constexpr auto CanonicalF64 = uint64_t { 0x7FF8'0000'0000'0000 };

    /// The features of the cut (scripts/tools.lock's cut.features) Wasmtime enables only when asked.
    constexpr auto CutFeatures = std::to_array<std::string_view>({
        "exceptions",
        "function-references",
        "tail-call",
        "multi-memory",
        "memory64",
        "extended-const",
        "relaxed-simd",
        "relaxed-simd-deterministic",
        "wide-arithmetic",
        "custom-page-sizes",
        "compact-imports",
    });

    [[nodiscard]] std::string_view trimmed(std::string_view line) noexcept
    {
        auto const begin = line.find_first_not_of(" \t\r");
        if (begin == std::string_view::npos)
            return {};
        return line.substr(begin, line.find_last_not_of(" \t\r") + 1 - begin);
    }

    /// The lines of @p text, each trimmed, the empty ones left out.
    [[nodiscard]] std::vector<std::string_view> linesOf(std::string_view text)
    {
        auto lines = std::vector<std::string_view> {};
        for (auto const line: std::views::split(text, '\n'))
        {
            auto const content = trimmed(std::string_view { line.begin(), line.end() });
            if (!content.empty())
                lines.push_back(content);
        }
        return lines;
    }

    /// The argument Wasmtime's command line takes for @p value, or nothing when it cannot pass it.
    [[nodiscard]] std::optional<std::string> argumentText(runtime::Value const& value)
    {
        return std::visit(
            []<typename T>(T const& alternative) -> std::optional<std::string> {
                if constexpr (std::is_same_v<T, runtime::I32>)
                    return std::format("{}", static_cast<int32_t>(alternative.bits));
                else if constexpr (std::is_same_v<T, runtime::I64>)
                    return std::format("{}", static_cast<int64_t>(alternative.bits));
                else if constexpr (std::is_same_v<T, runtime::F32>)
                {
                    auto const number = std::bit_cast<float>(alternative.bits);
                    if (std::isnan(number))
                        return alternative.bits == CanonicalF32 ? std::optional<std::string> { "nan" }
                                                                : std::nullopt;
                    return std::format("{}", number);
                }
                else if constexpr (std::is_same_v<T, runtime::F64>)
                {
                    auto const number = std::bit_cast<double>(alternative.bits);
                    if (std::isnan(number))
                        return alternative.bits == CanonicalF64 ? std::optional<std::string> { "nan" }
                                                                : std::nullopt;
                    return std::format("{}", number);
                }
                else
                    return std::nullopt;
            },
            value);
    }

    template <typename Integer>
    [[nodiscard]] std::optional<Integer> parseInteger(std::string_view text) noexcept
    {
        auto value = Integer {};
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc {} || end != text.data() + text.size())
            return std::nullopt;
        return value;
    }

    /// A float as Wasmtime prints it (Rust's shortest round-trip decimal, `inf`, `-inf`, `NaN`), parsed
    /// exactly; a NaN as the canonical NaN.
    template <typename Float>
    [[nodiscard]] std::optional<Float> parseFloat(std::string_view text)
    {
        if (text == "NaN")
            return std::numeric_limits<Float>::quiet_NaN();
        if (text == "inf")
            return std::numeric_limits<Float>::infinity();
        if (text == "-inf")
            return -std::numeric_limits<Float>::infinity();
        if (text.empty() || text.find_first_not_of("+-0123456789.eE") != std::string_view::npos)
            return std::nullopt;
        auto const terminated = std::string { text };
        char* end = nullptr;
        auto const value = std::is_same_v<Float, float>
                               ? std::strtof(terminated.c_str(), &end)
                               : static_cast<Float>(std::strtod(terminated.c_str(), &end));
        if (end != terminated.c_str() + terminated.size())
            return std::nullopt;
        return static_cast<Float>(value);
    }

    /// A vector as Wasmtime prints it: its 128 bits as one unsigned decimal.
    [[nodiscard]] std::optional<runtime::V128> parseVector(std::string_view text) noexcept
    {
        if (text.empty())
            return std::nullopt;
        constexpr auto Low32 = uint64_t { 0xFFFF'FFFF };
        auto vector = runtime::V128 {};
        for (auto const character: text)
        {
            if (character < '0' || character > '9')
                return std::nullopt;
            // vector * 10 + digit, through 32-bit limbs so that every carry is seen.
            auto carry = static_cast<uint64_t>(character - '0');
            auto limbs = std::array<uint64_t, 4> { vector.lanes[0] & Low32,
                                                   vector.lanes[0] >> 32U,
                                                   vector.lanes[1] & Low32,
                                                   vector.lanes[1] >> 32U };
            for (auto& limb: limbs)
            {
                auto const product = (limb * 10) + carry;
                limb = product & Low32;
                carry = product >> 32U;
            }
            if (carry != 0)
                return std::nullopt;
            vector.lanes[0] = limbs[0] | (limbs[1] << 32U);
            vector.lanes[1] = limbs[2] | (limbs[3] << 32U);
        }
        return vector;
    }

    [[nodiscard]] std::optional<runtime::Value> parseResult(binary::ValueType const& type,
                                                            std::string_view text)
    {
        switch (type.kind)
        {
            case binary::ValueKind::I32:
                if (auto const value = parseInteger<int32_t>(text))
                    return runtime::I32 { static_cast<uint32_t>(*value) };
                return std::nullopt;
            case binary::ValueKind::I64:
                if (auto const value = parseInteger<int64_t>(text))
                    return runtime::I64 { static_cast<uint64_t>(*value) };
                return std::nullopt;
            case binary::ValueKind::F32:
                if (auto const value = parseFloat<float>(text))
                    return runtime::F32 { std::isnan(*value) ? CanonicalF32
                                                             : std::bit_cast<uint32_t>(*value) };
                return std::nullopt;
            case binary::ValueKind::F64:
                if (auto const value = parseFloat<double>(text))
                    return runtime::F64 { std::isnan(*value) ? CanonicalF64
                                                             : std::bit_cast<uint64_t>(*value) };
                return std::nullopt;
            case binary::ValueKind::V128:
                if (auto const value = parseVector(text))
                    return *value;
                return std::nullopt;
            case binary::ValueKind::Ref: break;
        }
        return std::nullopt;
    }

    /// The function type of export @p name, when it exports a function of that name.
    [[nodiscard]] binary::FuncType const* exportedFunction(binary::Module const& module,
                                                           std::string_view name) noexcept
    {
        for (auto const& entry: module.exports)
        {
            if (entry.kind != binary::ExternKind::Func || entry.name != name)
                continue;
            // Only an import-free module is eligible, so the function index space is the function section.
            if (!module.imports.empty() || entry.index >= module.functions.size())
                return nullptr;
            return &module.types[module.functions[entry.index]];
        }
        return nullptr;
    }

    /// Whether Isoline's value agrees with the oracle's: equal, or both NaNs of the same type.
    [[nodiscard]] bool agrees(runtime::Value const& oracle, runtime::Value const& isoline) noexcept
    {
        if (auto const* expected = std::get_if<runtime::F32>(&oracle))
        {
            auto const* actual = std::get_if<runtime::F32>(&isoline);
            return actual != nullptr
                   && (actual->bits == expected->bits
                       || (std::isnan(std::bit_cast<float>(actual->bits))
                           && std::isnan(std::bit_cast<float>(expected->bits))));
        }
        if (auto const* expected = std::get_if<runtime::F64>(&oracle))
        {
            auto const* actual = std::get_if<runtime::F64>(&isoline);
            return actual != nullptr
                   && (actual->bits == expected->bits
                       || (std::isnan(std::bit_cast<double>(actual->bits))
                           && std::isnan(std::bit_cast<double>(expected->bits))));
        }
        return oracle == isoline;
    }
} // namespace

DirectoryModuleFiles::DirectoryModuleFiles(std::filesystem::path directory):
    _directory { std::move(directory) }
{
}

std::expected<std::filesystem::path, std::string> DirectoryModuleFiles::write(
    std::span<std::byte const> module)
{
    auto error = std::error_code {};
    std::filesystem::create_directories(_directory, error);
    if (error)
        return std::unexpected { std::format("cannot create {}: {}", _directory.string(), error.message()) };
    auto path = _directory / std::format("{:016x}.wasm", memoryDigest(module));
    if (std::filesystem::is_regular_file(path, error)
        && std::filesystem::file_size(path, error) == module.size())
        return path;
    // A staging file of its own, renamed into place, so that a concurrent run never reads half a module.
    auto const staging =
        _directory
        / std::format("{:016x}.{:016x}-{}.part", memoryDigest(module), std::random_device {}(), _writes++);
    {
        auto stream = std::ofstream { staging, std::ios::binary | std::ios::trunc };
        stream.write(reinterpret_cast<char const*>(module.data()),
                     static_cast<std::streamsize>(module.size()));
        if (!stream)
            return std::unexpected { std::format("cannot write {}", staging.string()) };
    }
    std::filesystem::rename(staging, path, error);
    if (error)
        return std::unexpected { std::format("cannot rename {}: {}", staging.string(), error.message()) };
    return path;
}

std::vector<std::string> wasmtimeFlags(std::chrono::seconds timeout)
{
    auto features = std::string {};
    for (auto const feature: CutFeatures)
        features += std::format("{}=y,", feature);
    features += std::format("timeout={}s", timeout.count());
    return { "run", "-W", features, "-C", "cranelift-enable-nan-canonicalization=true" };
}

bool isOracleEligible(binary::Module const& module, binary::FuncType const& type) noexcept
{
    auto const isScalar = [](binary::ValueType const& valueType) {
        return valueType.kind != binary::ValueKind::V128 && valueType.kind != binary::ValueKind::Ref;
    };
    return module.imports.empty() && std::ranges::all_of(type.params, isScalar)
           && std::ranges::none_of(type.results,
                                   [](auto const& result) { return result.kind == binary::ValueKind::Ref; });
}

bool agreesWithOracle(OracleResult const& oracle, CallOutcome const& outcome) noexcept
{
    if (auto const* expected = std::get_if<std::vector<runtime::Value>>(&oracle))
    {
        auto const* actual = std::get_if<std::vector<runtime::Value>>(&outcome);
        return actual != nullptr && std::ranges::equal(*expected, *actual, agrees);
    }
    auto const* abrupt = std::get_if<runtime::Abrupt>(&outcome);
    if (abrupt == nullptr)
        return false;
    if (auto const* trap = std::get_if<OracleTrap>(&oracle))
    {
        auto const* actual = std::get_if<runtime::Trap>(abrupt);
        return actual != nullptr && std::ranges::contains(trap->codes, actual->code);
    }
    return std::holds_alternative<runtime::Thrown>(*abrupt);
}

WasmtimeOracle::WasmtimeOracle(std::filesystem::path wasmtime,
                               IProcessRunner& runner,
                               IModuleFiles& files,
                               std::chrono::seconds timeout):
    _wasmtime { std::move(wasmtime) }, _runner { &runner }, _files { &files }, _timeout { timeout }
{
}

std::expected<OracleResult, OracleError> WasmtimeOracle::call(std::span<std::byte const> module,
                                                              std::string_view name,
                                                              std::span<runtime::Value const> args) const
{
    auto const loaded = validate::ValidatedModule::load(module, FeatureSet::all(), binary::DecodeLimits {});
    if (!loaded)
        return std::unexpected { OracleError { .kind = OracleErrorKind::Ineligible,
                                               .detail = "the module does not load" } };
    auto const* type = exportedFunction((*loaded)->module(), name);
    if (type == nullptr || !isOracleEligible((*loaded)->module(), *type))
        return std::unexpected { OracleError { .kind = OracleErrorKind::Ineligible,
                                               .detail = std::string { name } } };

    // A command-line argument ends at its first NUL, so such a name would invoke another export.
    if (name.contains('\0'))
        return std::unexpected { OracleError { .kind = OracleErrorKind::Unrepresentable,
                                               .detail = "the export's name holds a NUL" } };
    auto arguments = wasmtimeFlags(_timeout);
    auto const file = _files->write(module);
    if (!file)
        return std::unexpected { OracleError { .kind = OracleErrorKind::ModuleFile,
                                               .detail = file.error() } };
    // One argument, so that a name that starts like an option ("-") is still the option's value.
    arguments.insert(arguments.end(), { std::format("--invoke={}", name), file->string() });
    for (auto const& argument: args)
    {
        auto text = argumentText(argument);
        if (!text)
            return std::unexpected { OracleError { .kind = OracleErrorKind::Unrepresentable,
                                                   .detail = describe(argument) } };
        arguments.push_back(std::move(*text));
    }

    auto const ran = _runner->run(_wasmtime, arguments, _timeout + ProcessGrace);
    if (!ran)
        return std::unexpected { OracleError { .kind = OracleErrorKind::Process,
                                               .detail = ran.error().detail } };
    if (ran->exitCode == CannotExecute || (ran->exitCode != 0 && linesOf(ran->standardError).empty()))
        return std::unexpected { OracleError {
            .kind = OracleErrorKind::Process,
            .detail = std::format("exit status {}: {}", ran->exitCode, ran->standardError) } };
    for (auto const line: linesOf(ran->standardError))
    {
        auto const at = line.find(TrapPrefix);
        if (at != std::string_view::npos)
        {
            auto const message = trimmed(line.substr(at + TrapPrefix.size()));
            if (message == InterruptMessage)
                return std::unexpected { OracleError { .kind = OracleErrorKind::Exhausted,
                                                       .detail = std::string { message } } };
            auto const* row = trapMappingOf(message);
            if (row == nullptr)
                return std::unexpected { OracleError { .kind = OracleErrorKind::UnmappedTrap,
                                                       .detail = std::string { message } } };
            auto const codes = row->agreeing();
            return OracleTrap { .message = std::string { message }, .codes = { codes.begin(), codes.end() } };
        }
        if (line.ends_with(ThrownMessage))
            return OracleThrown {};
    }
    if (ran->exitCode != 0)
    {
        auto const isLimit = std::ranges::any_of(WasmtimeLimitRows, [&ran](WasmtimeLimitRow const& row) {
            return ran->standardError.contains(row.message);
        });
        return std::unexpected { OracleError { .kind = isLimit ? OracleErrorKind::Refused
                                                               : OracleErrorKind::Rejected,
                                               .detail = ran->standardError } };
    }
    auto const lines = linesOf(ran->standardOutput);
    if (lines.size() != type->results.size())
        return std::unexpected { OracleError { .kind = OracleErrorKind::Unparsed,
                                               .detail = ran->standardOutput } };
    auto results = std::vector<runtime::Value> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, lines.size()))
    {
        auto const value = parseResult(type->results[index], lines[index]);
        if (!value)
            return std::unexpected { OracleError { .kind = OracleErrorKind::Unparsed,
                                                   .detail = std::string { lines[index] } } };
        results.push_back(*value);
    }
    return results;
}

} // namespace isoline::differential
