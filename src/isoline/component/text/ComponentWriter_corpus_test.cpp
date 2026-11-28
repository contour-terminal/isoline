// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/binary/Encoder.hpp>
#include <isoline/component/text/ComponentWriter.hpp>
#include <isoline/component/text/Forms.hpp>
#include <isoline/component/validate/Validator.hpp>
#include <isoline/component/validate/testing/CoreModuleValidator.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using isoline::component::ComponentLimits;
using isoline::component::text::everyComponentFeature;

namespace
{

/// The directories of the imported component suites (tests/component-files.txt), under the source tree.
constexpr auto CorpusDirectories = std::to_array<std::string_view>({
    "tests/component-model",
    "tests/component-wasm-tools",
    "tests/component-wasmtime",
});

/// The gated features Isoline does not refuse (Implementation::Decoded or Implemented): a component of a
/// refused one (🪙 values, which the text format has no grammar for) is no component Isoline reads.
[[nodiscard]] isoline::component::ComponentFeatureSet readFeatures() noexcept
{
    auto features = isoline::component::ComponentFeatureSet::defaults();
    for (auto const& row: isoline::component::ComponentFeatureTable)
        if (row.p11 != isoline::component::Implementation::Refused)
            features = features.with(row.feature);
    return features;
}

/// The bytes of the file at @p path.
[[nodiscard]] std::vector<std::byte> bytesOf(std::filesystem::path const& path)
{
    auto file = std::ifstream { path, std::ios::binary };
    REQUIRE(file.is_open());
    auto bytes = std::vector<std::byte> {};
    std::ranges::transform(std::istreambuf_iterator<char> { file },
                           std::istreambuf_iterator<char> {},
                           std::back_inserter(bytes),
                           [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

/// Whether validation refuses @p pool for a gated form Isoline refuses (FeatureDisabled): one the text
/// format has no grammar for, as 🪙's value sort, that the decoder reads all the same.
[[nodiscard]] bool usesARefusedGate(isoline::component::binary::ComponentPool pool)
{
    auto coreModules = isoline::component::validate::testing::CoreModuleValidator {};
    auto const validated = isoline::component::validate::validateComponent(
        std::move(pool), readFeatures(), ComponentLimits {}, coreModules);
    return !validated
           && validated.error().code == isoline::component::validate::ValidationErrorCode::FeatureDisabled;
}

/// The canonical form of a component's bytes, its core modules canonical as P10 writes them.
[[nodiscard]] auto canonicalOf(std::span<std::byte const> bytes)
{
    auto modules = isoline::component::text::P10CoreModules {};
    return isoline::component::binary::canonicalizeComponent(
        bytes, everyComponentFeature(), ComponentLimits {}, modules);
}

} // namespace

TEST_CASE("ComponentWriter.roundTripsCorpus")
{
    // Every committed component of the suites that decodes under the gated features Isoline does not
    // refuse (the oracle's binaries, components given as bytes among them) means the same component once
    // written as text and compiled again: canonicalize(x) == canonicalize(compile(write(decode(x)))).
    auto components = std::size_t { 0 };
    for (auto const directory: CorpusDirectories)
    {
        auto files = std::vector<std::filesystem::path> {};
        for (auto const& entry: std::filesystem::recursive_directory_iterator {
                 std::filesystem::path { ISOLINE_SOURCE_DIR } / directory })
            if (entry.is_regular_file() && entry.path().extension() == ".wasm")
                files.push_back(entry.path());
        std::ranges::sort(files);
        for (auto const& path: files)
        {
            auto const bytes = bytesOf(path);
            if (isoline::component::binary::layerOf(bytes)
                != isoline::component::binary::LayerKind::Component)
                continue;
            auto const pool =
                isoline::component::binary::decodeComponent(bytes, readFeatures(), ComponentLimits {});
            auto const canonical = canonicalOf(bytes);
            if (!pool || !canonical || usesARefusedGate(*pool))
                continue;
            ++components;
            INFO(path.string());
            auto const text =
                isoline::component::text::ComponentWriter { {}, isoline::FeatureSet::all() }.write(*pool);
            REQUIRE(text.has_value());
            INFO(*text);
            auto const compiled = isoline::text::compileText(
                *text, isoline::text::TextLimits {}, isoline::component::text::Forms);
            if (!compiled)
                UNSCOPED_INFO(std::format("{} at {}:{}",
                                          isoline::text::specMessage(compiled.error()),
                                          compiled.error().position.line,
                                          compiled.error().position.column));
            REQUIRE(compiled.has_value());
            auto const again = canonicalOf(*compiled);
            REQUIRE(again.has_value());
            CHECK(*again == *canonical);
        }
    }
    // The corpus is there: the suites hold well over a thousand components.
    CHECK(components > 1'000);
}
