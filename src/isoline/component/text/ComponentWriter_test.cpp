// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/binary/Encoder.hpp>
#include <isoline/component/text/ComponentWriter.hpp>
#include <isoline/component/text/Forms.hpp>
#include <isoline/text/ModuleWriter.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using isoline::component::ComponentLimits;
using isoline::component::binary::ComponentPool;
using isoline::component::binary::decodeComponent;
using isoline::component::text::ComponentWriter;
using isoline::component::text::CoreModuleMalformed;
using isoline::component::text::everyComponentFeature;
using isoline::component::text::Forms;
using isoline::text::compileText;
using isoline::text::TextLimits;
using isoline::text::WriteError;
using isoline::text::WriterOptions;

namespace
{

std::vector<std::byte> fixtureBytes(std::string_view name)
{
    auto file = std::ifstream { std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "component" / "text"
                                    / (std::string { name } + ".wasm"),
                                std::ios::binary };
    REQUIRE(file.is_open());
    auto bytes = std::vector<std::byte> {};
    std::ranges::transform(std::istreambuf_iterator<char> { file },
                           std::istreambuf_iterator<char> {},
                           std::back_inserter(bytes),
                           [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

ComponentPool poolOf(std::span<std::byte const> bytes)
{
    auto pool = decodeComponent(bytes, everyComponentFeature(), ComponentLimits {});
    REQUIRE(pool.has_value());
    return std::move(*pool);
}

std::string textOf(std::span<std::byte const> bytes, WriterOptions options = {})
{
    auto text = ComponentWriter { options, isoline::FeatureSet::all() }.write(poolOf(bytes));
    REQUIRE(text.has_value());
    return std::move(*text);
}

std::vector<std::byte> compiled(std::string_view text)
{
    auto bytes = compileText(text, TextLimits {}, Forms);
    INFO(text);
    auto const message = bytes ? std::string {}
                               : std::format("{} at {}:{}",
                                             isoline::text::specMessage(bytes.error()),
                                             bytes.error().position.line,
                                             bytes.error().position.column);
    INFO(message);
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

} // namespace

TEST_CASE("ComponentWriter.roundTripsTheCorpus")
{
    // Reading what the writer writes gives the component back, canonically, core modules unnamed.
    for (auto const name: { "names", "sugar", "types", "canon", "more" })
    {
        INFO(name);
        auto const bytes = fixtureBytes(name);
        auto const text = textOf(bytes);
        INFO(text);
        auto modules = isoline::component::text::P10CoreModules {};
        auto const canonical = isoline::component::binary::canonicalizeComponent(
            bytes, everyComponentFeature(), ComponentLimits {}, modules);
        REQUIRE(canonical.has_value());
        CHECK(compiled(text) == *canonical);
        // And what it writes of that is the same text.
        CHECK(textOf(compiled(text)) == text);
    }
}

TEST_CASE("ComponentWriter.coreNameSectionThatDoesNotDecode")
{
    // A core module's name section that names nothing it can decode is the module writer's to keep raw.
    auto const bytes = compiled(R"wat((component (core module (@custom "name" "\ff"))))wat");
    auto const text = textOf(bytes);
    INFO(text);
    CHECK(text.contains(R"wat((@custom "name" (before first) "\ff"))wat"));
    CHECK(compiled(text) == bytes);
}

TEST_CASE("ComponentWriter.layout")
{
    auto const text = textOf(compiled(R"wat((component
        (core module (func (export "f")))
        (type (instance (export "g" (func))))
        (import "i" (instance (type 0)))
        (alias export 0 "g" (func))
        (component (type (list u8)))
        (export "h" (func 0) (func (type 1)))))wat"));
    CHECK(text == R"wat((component
  (core module (;0;)
    (type (func))
    (func (type 0)
    )
    (export "f" (func 0))
  )
  (type (;0;) (instance
    (type (;0;) (func))
    (export "g" (func (;0;) (type 0)))
  ))
  (import "i" (instance (;0;) (type 0)))
  (alias export 0 "g" (func (;0;)))
  (component (;0;)
    (type (;0;) (list u8))
  )
  (export (;1;) "h" (func 0) (func (type 1)))
)
)wat");
}

TEST_CASE("ComponentWriter.refusals")
{
    auto const bytes = compiled("(component (core module (func)) (type u8))");
    // Past maxTextBytes, in a component's line or a core module's.
    auto const whole = textOf(bytes);
    auto const tooLarge =
        ComponentWriter { WriterOptions { .maxTextBytes = whole.size() - 1 }, isoline::FeatureSet::all() }
            .write(poolOf(bytes));
    REQUIRE_FALSE(tooLarge.has_value());
    CHECK(std::get<WriteError>(tooLarge.error()) == WriteError::TextTooLarge);
    auto const tiny =
        ComponentWriter { WriterOptions { .maxTextBytes = 20 }, isoline::FeatureSet::all() }.write(
            poolOf(bytes));
    REQUIRE_FALSE(tiny.has_value());
    CHECK(std::get<WriteError>(tiny.error()) == WriteError::TextTooLarge);
    // Within a module type's lines too.
    auto const moduleType = compiled("(component (core type (module (type (func)) (type (func)))))");
    auto const typed =
        ComponentWriter { WriterOptions { .maxTextBytes = 24 }, isoline::FeatureSet::all() }.write(
            poolOf(moduleType));
    REQUIRE_FALSE(typed.has_value());
    CHECK(std::get<WriteError>(typed.error()) == WriteError::TextTooLarge);
}

TEST_CASE("ComponentWriter.coreModuleMalformed")
{
    // The second core module (ordinal 1) holds a section of an unknown id.
    auto pool = poolOf(compiled("(component (core module) (component (core module)))"));
    auto bytes = std::vector<std::byte>(pool.bytes->begin(), pool.bytes->end());
    auto& inner =
        std::get<isoline::component::binary::CoreModuleDefinition>(pool.components[1].definitions[0]);
    auto const offset = bytes.size();
    auto const broken = std::to_array<std::byte>({ std::byte { 0x00 },
                                                   std::byte { 0x61 },
                                                   std::byte { 0x73 },
                                                   std::byte { 0x6d },
                                                   std::byte { 0x01 },
                                                   std::byte { 0x00 },
                                                   std::byte { 0x00 },
                                                   std::byte { 0x00 },
                                                   std::byte { 0x7f },
                                                   std::byte { 0x00 } });
    bytes.insert(bytes.end(), broken.begin(), broken.end());
    inner = { .offset = offset, .size = broken.size() };
    pool.bytes = std::make_shared<std::vector<std::byte> const>(std::move(bytes));
    auto const text = ComponentWriter { {}, isoline::FeatureSet::all() }.write(pool);
    REQUIRE_FALSE(text.has_value());
    REQUIRE(std::holds_alternative<CoreModuleMalformed>(text.error()));
    CHECK(std::get<CoreModuleMalformed>(text.error()).ordinal == 1);
}
