// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include <isoline/component/binary/Component.hpp>
#include <isoline/component/text/Forms.hpp>
#include <isoline/text/Keywords.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using isoline::component::text::Forms;
using isoline::component::text::P10CoreModules;
using isoline::text::compileText;
using isoline::text::CoreForms;
using isoline::text::TextErrorCode;
using isoline::text::TextLimits;

TEST_CASE("TopLevel.coreFormsStillRefuseComponents")
{
    // isoline_text's own table refuses components, as it links no component model (D13).
    auto const refused = compileText("(component)", TextLimits {}, CoreForms);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TextErrorCode::UnsupportedComponent);
    // The component model's table reads them: the empty component is its preamble alone.
    auto const component = compileText("(component)", TextLimits {}, Forms);
    REQUIRE(component.has_value());
    auto const preamble = std::vector<std::byte> { std::byte { 0x00 }, std::byte { 0x61 }, std::byte { 0x73 },
                                                   std::byte { 0x6d }, std::byte { 0x0d }, std::byte { 0x00 },
                                                   std::byte { 0x01 }, std::byte { 0x00 } };
    CHECK(*component == preamble);
    // Modules read alike through either table.
    auto const throughForms = compileText("(module (func))", TextLimits {}, Forms);
    auto const throughCore = compileText("(module (func))", TextLimits {}, CoreForms);
    REQUIRE(throughForms.has_value());
    REQUIRE(throughCore.has_value());
    CHECK(*throughForms == *throughCore);
}

TEST_CASE("Forms.vocabularyIsKnown")
{
    // Every keyword component text reads is in the text format's vocabulary: a misplaced one is an
    // unexpected token, not an unknown operator.
    for (auto const& row: isoline::component::binary::CanonBuiltinTable)
        CHECK(isoline::text::isKnownKeyword(row.text));
    for (auto const& row: isoline::component::binary::PrimitiveValueTypeTable)
        CHECK(isoline::text::isKnownKeyword(row.text));
    for (auto const& row: isoline::component::binary::DefinedValueTypeTable)
        CHECK(isoline::text::isKnownKeyword(row.keyword));
    for (auto const& row: isoline::component::binary::NameAttributeTable)
        CHECK(isoline::text::isKnownKeyword(row.keyword));
    for (auto const& row: isoline::component::text::SortTexts)
        CHECK(isoline::text::isKnownKeyword(row.keyword));
}

TEST_CASE("Forms.coreModuleCanonicalizer")
{
    auto modules = P10CoreModules {};
    auto const module = compileText(R"wat((module (func (export "f"))))wat", TextLimits {}, CoreForms);
    REQUIRE(module.has_value());
    auto const canonical = modules.canonicalize(*module);
    REQUIRE(canonical.has_value());
    CHECK(*canonical == *module);
    auto const malformed = std::vector<std::byte> { std::byte { 0x00 }, std::byte { 0x61 } };
    auto const refused = modules.canonicalize(malformed);
    REQUIRE_FALSE(refused.has_value());
    CHECK_FALSE(refused.error().empty());
}
