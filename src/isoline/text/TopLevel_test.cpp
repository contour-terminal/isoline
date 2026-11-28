// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <initializer_list>
#include <ranges>
#include <string>
#include <vector>

#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using isoline::text::compileText;
using isoline::text::CoreForms;
using isoline::text::NodeId;
using isoline::text::parseSExpressions;
using isoline::text::refuseComponent;
using isoline::text::specMessage;
using isoline::text::TextErrorCode;
using isoline::text::TextLimits;

namespace
{

/// The bytes @p source compiles to, which it must.
std::vector<std::byte> compiled(std::string_view source)
{
    auto bytes = compileText(source, TextLimits {}, CoreForms);
    INFO(source);
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

} // namespace

TEST_CASE("TopLevel.componentRefused")
{
    auto const component = compileText("(component)", TextLimits {}, CoreForms);
    REQUIRE_FALSE(component.has_value());
    CHECK(component.error().code == TextErrorCode::UnsupportedComponent);
    CHECK(specMessage(component.error()) == "component text needs the component model");
    // Without a forms table the text is read with CoreForms, as a core-only consumer calls it.
    auto const byDefault = compileText("(component)", TextLimits {});
    REQUIRE_FALSE(byDefault.has_value());
    CHECK(byDefault.error().code == TextErrorCode::UnsupportedComponent);
    auto const module = compileText("(module)", TextLimits {});
    REQUIRE(module.has_value());
    CHECK(*module == *compileText("(module)", TextLimits {}, CoreForms));

    auto const tree = parseSExpressions("(component)", TextLimits {});
    REQUIRE(tree.has_value());
    auto const refused = refuseComponent(*tree, NodeId { 0 }, TextLimits {});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == TextErrorCode::UnsupportedComponent);
}

TEST_CASE("TopLevel.bareFieldsCompile")
{
    CHECK(compiled("(func) (memory 1)") == compiled("(module (func) (memory 1))"));
    CHECK(compiled(";; nothing but a comment") == compiled("(module)"));
    CHECK(compiled("") == compiled("(module)"));
    // Annotations before the form do not hide it.
    CHECK(compiled("(@unknown) (module (func))") == compiled("(module (func))"));
    // A quote's text may be the whole module form, as custom-page-sizes.wast:113's is.
    CHECK(compiled("(module (memory (pagesize 1) (data)))") == compiled("(memory (pagesize 1) (data))"));
}

TEST_CASE("TopLevel.aFormIsTheWholeText")
{
    auto const second = compileText("(module) (func)", TextLimits {}, CoreForms);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == TextErrorCode::UnexpectedToken);
    CHECK(second.error().position.column == 10);

    auto const trailing = compileText("(module) junk", TextLimits {}, CoreForms);
    REQUIRE_FALSE(trailing.has_value());
    CHECK(specMessage(trailing.error()) == "unknown operator junk");

    auto const component = compileText("(component) (module)", TextLimits {}, CoreForms);
    REQUIRE_FALSE(component.has_value());
    CHECK(component.error().code == TextErrorCode::UnexpectedToken);

    // A module field is no form: the text is an inline module, and (module) is no field of one.
    auto const inlineModule = compileText("(func) (module)", TextLimits {}, CoreForms);
    REQUIRE_FALSE(inlineModule.has_value());
    CHECK(inlineModule.error().code == TextErrorCode::UnexpectedToken);
}

TEST_CASE("TopLevel.errorsOfTheText")
{
    auto const unbalanced = compileText("(module", TextLimits {}, CoreForms);
    REQUIRE_FALSE(unbalanced.has_value());
    CHECK(unbalanced.error().code == TextErrorCode::UnexpectedToken);

    auto const inner = compileText("(module (func $f) (func $f))", TextLimits {}, CoreForms);
    REQUIRE_FALSE(inner.has_value());
    CHECK(specMessage(inner.error()) == "duplicate function $f");

    auto const inlineError = compileText("(func $f) (func $f)", TextLimits {}, CoreForms);
    REQUIRE_FALSE(inlineError.has_value());
    CHECK(inlineError.error().code == TextErrorCode::DuplicateIdentifier);
}

TEST_CASE("TopLevel.outputLimit")
{
    // A compact import group's binary repeats its module name per item: past maxModuleBytes it is
    // refused, as a module form and as bare fields alike.
    auto const fields = std::string { R"((import ")" } + std::string(1'000, 'm')
                        + R"(" (item "a" (func)) (item "b" (func)) (item "c" (func))))";
    auto const whole = compileText(fields, TextLimits {}, CoreForms);
    REQUIRE(whole.has_value());
    CHECK(compileText(fields, TextLimits { .maxModuleBytes = whole->size() }, CoreForms).has_value());
    for (auto const& source: { fields, "(module " + fields + ")" })
    {
        auto const refused =
            compileText(source, TextLimits { .maxModuleBytes = whole->size() - 1 }, CoreForms);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == TextErrorCode::OutputTooLarge);
        CHECK(specMessage(refused.error()) == "output too large");
    }

    // A code section is refused as it passes the limit: of two bodies of 1'000 nops under a limit of a
    // quarter of their module, the first is written, past the limit, and the second not begun.
    auto nops = std::string {};
    for ([[maybe_unused]] auto const index: std::views::iota(0, 1'000))
        nops += "nop ";
    auto const bodies = "(func " + nops + ") (func " + nops + ")";
    auto const both = compileText(bodies, TextLimits {}, CoreForms);
    REQUIRE(both.has_value());
    auto const quarter = compileText(bodies, TextLimits { .maxModuleBytes = both->size() / 4 }, CoreForms);
    REQUIRE_FALSE(quarter.has_value());
    CHECK(quarter.error().code == TextErrorCode::OutputTooLarge);
}
