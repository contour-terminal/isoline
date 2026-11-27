// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/CompileLimits.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Printer.hpp>
#include <isoline/compiler/intermediate/Verifier.hpp>
#include <isoline/compiler/translation/Translator.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/validate/ValidatedModule.hpp>

/// What the translation tests share: modules translated and verified, and searches of their text.
namespace isoline::compiler::translation::testing
{

using binary::FuncType;
using binary::ValueType;
using binary::testing::Bytes;
using binary::testing::ModuleBuilder;
using intermediate::Function;
using intermediate::Operation;
using intermediate::print;
using intermediate::verify;
using runtime::ExecutionMode;
using validate::ValidatedModule;

/// Whether a test's compile is cancelled before it starts.
enum class Cancellation : uint8_t
{
    Running,   ///< Not cancelled.
    Cancelled, ///< Cancelled.
};

/// How a test translates.
struct Options
{
    ExecutionMode mode = ExecutionMode::Production;    ///< The mode.
    CompileLimits limits {};                           ///< The caps.
    uint32_t codeIndex = 0;                            ///< The function.
    Cancellation cancellation = Cancellation::Running; ///< Whether it is cancelled.
};

/// One function's translation, with the arena and module it lives on.
struct Translated
{
    std::pmr::monotonic_buffer_resource memory {};
    std::shared_ptr<ValidatedModule const> module {};
    std::optional<Function> function {};
    std::optional<CompileError> error {};

    /// The function's text; fails the test unless it translated.
    [[nodiscard]] std::string text() const
    {
        REQUIRE(function.has_value());
        return print(*function);
    }
};

/// Translates function @p options.codeIndex of @p builder's module, which must validate; a translated
/// function must verify.
inline std::unique_ptr<Translated> translateOf(ModuleBuilder const& builder, Options const& options = {})
{
    auto result = std::make_unique<Translated>();
    auto loaded = ValidatedModule::load(builder.build(), isoline::FeatureSet::all(), binary::DecodeLimits {});
    REQUIRE(loaded.has_value());
    result->module = std::move(*loaded);
    auto const index = ModuleIndex { result->module->module(), result->memory };
    auto cancellation = CompileCancellation {};
    if (options.cancellation == Cancellation::Cancelled)
        cancellation.cancel();
    auto translated = translate(TranslationInput { .module = *result->module,
                                                   .index = index,
                                                   .codeIndex = options.codeIndex,
                                                   .mode = options.mode,
                                                   .limits = options.limits,
                                                   .cancellation = cancellation },
                                result->memory);
    if (!translated)
    {
        result->error = translated.error();
        return result;
    }
    auto const verified = verify(*translated);
    INFO((verified ? std::string {} : describe(verified.error())) << "\n" << print(*translated));
    REQUIRE(verified.has_value());
    result->function.emplace(std::move(*translated));
    return result;
}

/// The refusal of @p translated; fails the test if it translated.
inline CompileError refusalOf(Translated const& translated)
{
    REQUIRE(translated.error.has_value());
    return *translated.error;
}

/// A module of the function type @p type and one function of it with @p locals and @p body.
inline ModuleBuilder oneFunction(FuncType type, std::vector<ValueType> locals, Bytes body)
{
    auto builder = ModuleBuilder {};
    builder.type(std::move(type)).function(0, std::move(locals), std::move(body));
    return builder;
}

/// The lines of @p text.
inline std::vector<std::string_view> linesOf(std::string const& text)
{
    auto lines = std::vector<std::string_view> {};
    for (auto const line: std::views::split(text, '\n'))
        lines.emplace_back(line.begin(), line.end());
    return lines;
}

/// The index of the first line of @p lines at or after @p from that contains @p needle; none if none.
inline std::optional<std::size_t> lineWith(std::vector<std::string_view> const& lines,
                                           std::string_view needle,
                                           std::size_t from = 0)
{
    for (auto const index: std::views::iota(from, lines.size()))
        if (lines[index].contains(needle))
            return index;
    return std::nullopt;
}

/// How often @p needle occurs in @p text.
inline std::size_t countOf(std::string const& text, std::string_view needle)
{
    auto count = std::size_t { 0 };
    auto position = text.find(needle);
    while (position != std::string::npos)
    {
        ++count;
        position = text.find(needle, position + 1);
    }
    return count;
}

/// The instructions of @p function that are @p operation, by index.
inline std::vector<uint32_t> instructionsOf(Function const& function, Operation operation)
{
    auto found = std::vector<uint32_t> {};
    for (auto const index: std::views::iota(uint32_t { 0 }, function.instructionCount()))
        if (function.instruction(index).operation == operation)
            found.push_back(index);
    return found;
}

} // namespace isoline::compiler::translation::testing
