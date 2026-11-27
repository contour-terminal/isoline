// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/component/Features.hpp>
#include <isoline/component/validate/Names.hpp>

using isoline::component::ComponentFeature;
using isoline::component::ComponentFeatureSet;
using isoline::component::validate::canonicalLabel;
using isoline::component::validate::ExternName;
using isoline::component::validate::isKebabCase;
using isoline::component::validate::isSemverCompatible;
using isoline::component::validate::NameErrorCode;
using isoline::component::validate::nameErrorMessage;
using isoline::component::validate::NameKind;
using isoline::component::validate::parseExternName;
using isoline::component::validate::parseSemanticVersion;
using isoline::component::validate::SemanticVersion;
using isoline::component::validate::semanticVersionMessage;
using isoline::component::validate::strongUniquenessKey;

namespace
{
/// The name @p text parses to; it must parse.
[[nodiscard]] ExternName parsed(std::string_view text)
{
    auto name = parseExternName(text, ComponentFeatureSet::wasi03());
    if (!name)
        UNSCOPED_INFO(nameErrorMessage(name.error()));
    REQUIRE(name.has_value());
    return *name;
}

/// The message refusing @p text; it must be refused.
[[nodiscard]] std::string refusal(std::string_view text,
                                  ComponentFeatureSet features = ComponentFeatureSet::wasi03())
{
    auto const name = parseExternName(text, features);
    REQUIRE_FALSE(name.has_value());
    return nameErrorMessage(name.error());
}

/// The version @p text parses to; it must parse.
[[nodiscard]] SemanticVersion version(std::string_view text)
{
    auto const parsedVersion = parseSemanticVersion(text);
    REQUIRE(parsedVersion.has_value());
    return *parsedVersion;
}

/// The message of the `semver` crate refusing @p text; it must be refused.
[[nodiscard]] std::string versionRefusal(std::string_view text)
{
    auto const parsedVersion = parseSemanticVersion(text);
    REQUIRE_FALSE(parsedVersion.has_value());
    return semanticVersionMessage(parsedVersion.error());
}
} // namespace

TEST_CASE("Names.grammar")
{
    auto const plain = parsed("a-b");
    CHECK(plain.kind == NameKind::Plain);
    CHECK(plain.text == "a-b");

    auto const method = parsed("[method]r.f");
    CHECK(method.kind == NameKind::Method);
    CHECK(method.resource == "r");
    CHECK(method.function == "f");
    auto const staticFunction = parsed("[static]r-1.make-it");
    CHECK(staticFunction.kind == NameKind::Static);
    CHECK(staticFunction.resource == "r-1");
    CHECK(staticFunction.function == "make-it");
    auto const constructor = parsed("[constructor]r");
    CHECK(constructor.kind == NameKind::Constructor);
    CHECK(constructor.resource == "r");

    auto const interface = parsed("wasi:cli/run@0.3.2");
    CHECK(interface.kind == NameKind::Interface);
    CHECK(interface.package == "wasi:cli");
    CHECK(interface.interface == "run");
    REQUIRE(interface.version.has_value());
    CHECK(*interface.version == SemanticVersion { .major = 0, .minor = 3, .patch = 2 });
    CHECK_FALSE(parsed("wasi:cli/run").version.has_value());
    CHECK(parsed("a:b/c@1.0.0+build").version == SemanticVersion { .major = 1 });
    CHECK(parsed("a:b/c@1.0.0-rc.1").version == SemanticVersion { .major = 1, .prerelease = "rc.1" });

    // KebabStr::new's own vectors (wasmparser 0.263.0 src/validator/names.rs:1115-1130).
    for (auto const* label: { "a", "a-B", "a0", "a-0", "a0-000-3d4a-54FF", "A", "ABC-def" })
    {
        INFO(label);
        CHECK(isKebabCase(label));
    }
    for (auto const* label: { "", "aB", "a-", "-", "\xC2\xB6", "0", "0-a", "a-b--c", "a0-000-3d4A-54Ff" })
    {
        INFO(label);
        CHECK_FALSE(isKebabCase(label));
    }
    CHECK(canonicalLabel("a-B-c") == "abc");

    // Each refusal in wasm-tools 1.263.0's words, measured: `wasm-tools validate` of
    // (component (import "<name>" (instance))) reports them after its context line.
    struct Vector
    {
        std::string_view name;
        std::string_view message;
    };
    for (auto const& [name, message]: std::vector<Vector> {
             { .name = "aB", .message = "`aB` is not in kebab case" },
             { .name = "1", .message = "`1` is not in kebab case" },
             { .name = "a/b", .message = "`a/b` is not in kebab case" },
             { .name = "[method]a", .message = "failed to find `.` character" },
             { .name = "[method]a.b.c", .message = "`b.c` is not in kebab case" },
             { .name = "[constructor]", .message = "`` is not in kebab case" },
             { .name = "[foo]a", .message = "`[foo]a` is not in kebab case" },
             { .name = "a:B/c", .message = "character `B` is not lowercase in package name/namespace" },
             { .name = "A:b/c", .message = "character `A` is not lowercase in package name/namespace" },
             { .name = "a:b", .message = "expected `/` after package name" },
             { .name = "a:b:c/d", .message = "expected `/` after package name" },
             { .name = "a/b:c", .message = "expected `:` at `/b:c`" },
             { .name = ":b/c", .message = "`` is not in kebab case" },
             { .name = "a:b/c/d", .message = "trailing characters found: `/d`" },
             { .name = "a:b/c!", .message = "trailing characters found: `!`" },
             { .name = "a:b/c@0.3",
               .message =
                   "invalid interface version: unexpected end of input while parsing minor version number" },
             { .name = "a:b/c@",
               .message = "invalid interface version: empty string, expected a semver version" },
         })
    {
        INFO(name);
        CHECK(refusal(name) == message);
    }

    // 📡 is refused in P11 whatever the set holds, so `[get]` and `[set]` are, naming it.
    auto const accessor =
        parseExternName("[get]a", ComponentFeatureSet::defaults().with(ComponentFeature::GettersSetters));
    REQUIRE_FALSE(accessor.has_value());
    CHECK(accessor.error().code == NameErrorCode::AccessorsDisabled);
    CHECK(accessor.error().disabled == ComponentFeature::GettersSetters);
    CHECK(nameErrorMessage(accessor.error())
          == "`[get]` and `[set]` names require the component model accessors feature");
    CHECK(refusal("[method][set]r.f")
          == "`[get]` and `[set]` names require the component model accessors feature");
    // 🪺 nested names fail the grammar as without the feature (above), whatever the set holds.
    CHECK(refusal("a:b/c/d", ComponentFeatureSet::defaults().with(ComponentFeature::NestedNamespaces))
          == "trailing characters found: `/d`");
}

TEST_CASE("Names.semverParse")
{
    CHECK(version("0.3.2") == SemanticVersion { .major = 0, .minor = 3, .patch = 2 });
    CHECK(version("1.2.3-alpha.1+sha.5114f85")
          == SemanticVersion { .major = 1, .minor = 2, .patch = 3, .prerelease = "alpha.1" });
    CHECK(version("18446744073709551615.0.0").major == UINT64_MAX);

    // The `semver` crate's messages, as wasm-tools 1.263.0 reports them after "invalid interface
    // version: " (measured with `wasm-tools validate`).
    struct Vector
    {
        std::string_view version;
        std::string_view message;
    };
    for (auto const& [text, message]: std::vector<Vector> {
             { .version = "", .message = "empty string, expected a semver version" },
             { .version = "0.3", .message = "unexpected end of input while parsing minor version number" },
             { .version = "1", .message = "unexpected end of input while parsing major version number" },
             { .version = "1.0.", .message = "unexpected end of input while parsing patch version number" },
             { .version = "01.0.0", .message = "invalid leading zero in major version number" },
             { .version = "1.00.0", .message = "invalid leading zero in minor version number" },
             { .version = "1.q.0", .message = "unexpected character 'q' while parsing minor version number" },
             { .version = "1,0.0", .message = "unexpected character ',' after major version number" },
             { .version = "1.0.0x", .message = "unexpected character 'x' after patch version number" },
             { .version = "1.0.0-", .message = "empty identifier segment in pre-release identifier" },
             { .version = "1.0.0-a..b", .message = "empty identifier segment in pre-release identifier" },
             { .version = "1.0.0-01", .message = "invalid leading zero in pre-release identifier" },
             { .version = "1.0.0+", .message = "empty identifier segment in build metadata" },
             { .version = "1.0.0-a+b!", .message = "unexpected character '!' after build metadata" },
             { .version = "1.0.0-a!", .message = "unexpected character '!' after pre-release identifier" },
             { .version = "1.0.0'", .message = R"(unexpected character '\'' after patch version number)" },
             { .version = "99999999999999999999.0.0",
               .message = "value of major version number exceeds u64::MAX" },
             { .version = "18446744073709551616.0.0",
               .message = "value of major version number exceeds u64::MAX" },
         })
    {
        INFO(text);
        CHECK(versionRefusal(text) == message);
    }
    // A pre-release segment of digits may be "0"; build metadata may lead with zeros.
    CHECK(version("1.0.0-0").prerelease == "0");
    CHECK(version("1.0.0+001").prerelease.empty());
}

TEST_CASE("Names.semver")
{
    auto const compatible = [](std::string_view required, std::string_view provided) {
        return isSemverCompatible(version(required), version(provided));
    };
    CHECK(compatible("0.3.0", "0.3.2"));
    CHECK(compatible("0.3.2", "0.3.0"));
    CHECK_FALSE(compatible("0.3.0", "0.4.0"));
    CHECK_FALSE(compatible("0.0.1", "0.0.2"));
    CHECK(compatible("0.0.1", "0.0.1"));
    CHECK(compatible("1.0.0", "1.9.0"));
    CHECK(compatible("1.2.3", "1.0.7"));
    CHECK_FALSE(compatible("1.0.0", "2.0.0"));
    CHECK_FALSE(compatible("0.0.0", "0.1.0"));
    // A pre-release matches only itself, so do its neighbours only themselves.
    CHECK(compatible("1.0.0-rc.1", "1.0.0-rc.1"));
    CHECK_FALSE(compatible("1.0.0-rc.1", "1.0.0-rc.2"));
    CHECK_FALSE(compatible("1.0.0-rc.1", "1.0.0"));
    CHECK_FALSE(compatible("1.0.0", "1.1.0-rc.1"));
    // Build metadata is dropped when parsed, and never decides.
    CHECK(compatible("1.0.0+a", "1.0.0+b"));
    CHECK(compatible("0.0.1+a", "0.0.1"));
}

TEST_CASE("Names.strongUniqueness")
{
    // The uniqueness vectors of wasmparser 0.263.0 (src/validator/names.rs:1156-1265), the accessor
    // forms aside (📡 is refused): a name conflicts exactly with those of an equal key.
    auto const key = [](std::string_view text) {
        return strongUniquenessKey(parsed(text));
    };
    CHECK(key("a") == key("a"));
    CHECK(key("a") != key("b"));
    CHECK(key("[constructor]a") != key("[constructor]b"));
    CHECK(key("[method]a.b") == key("[static]a.b"));
    CHECK(key("[method]b.b") != key("[static]a.b"));

    auto seen = std::set<std::string> {};
    for (auto const* name: { "a", "[constructor]a", "[method]a.b", "[static]b.b" })
    {
        INFO(name);
        CHECK(seen.insert(key(name)).second);
    }
    CHECK_FALSE(seen.insert(key("[static]a.b")).second);

    seen.clear();
    for (auto const* name: { "foo",
                             "foo-bar",
                             "[constructor]foo",
                             "[method]foo.bar",
                             "[static]foo.baz",
                             "foo:bar/baz",
                             "[method]foo.get-prop",
                             "[method]foo.set-prop" })
    {
        INFO(name);
        CHECK(seen.insert(key(name)).second);
    }
    for (auto const* name: { "foo",
                             "FOO",
                             "[method]foo.foo",
                             "foo-BAR",
                             "foobar",
                             "foob-ar",
                             "[static]foo-BAR.FOO-bar",
                             "[constructor]FOO",
                             "[method]foo.BAR",
                             "[static]foo.bar",
                             "[method]foo.baz",
                             "foo:bar/BAZ" })
    {
        INFO(name);
        CHECK_FALSE(seen.insert(key(name)).second);
    }
    // An interface name's version is compared as written.
    CHECK(key("a:b/c@1.0.0") != key("a:b/c@1.0.0-x"));
    CHECK(key("a:b/c@1.0.0") != key("a:b/c"));
    CHECK(key("a-b:c/d") == key("ab:c/D"));
    CHECK(key("a:b/c") != key("[method]a.b"));
}
