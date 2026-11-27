// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <isoline/component/Features.hpp>

namespace isoline::component::validate
{

/// Whether an extern name is an import's or an export's: the word wasm-tools' messages call it.
enum class ExternKind : uint8_t
{
    Import,
    Export,
};

/// What each ExternKind is called in messages, by enumerator (ExternKind::desc, wasmparser 0.263.0
/// src/validator/component.rs:205-210).
inline constexpr auto ExternKindWords = std::to_array<std::string_view>({ "import", "export" });

static_assert(ExternKindWords.size() == static_cast<std::size_t>(ExternKind::Export) + 1,
              "ExternKindWords holds one word per ExternKind");

/// The word messages use for @p kind.
/// @param kind An extern kind.
/// @return "import" or "export".
[[nodiscard]] constexpr std::string_view externKindWord(ExternKind kind) noexcept
{
    return ExternKindWords[static_cast<std::size_t>(kind)];
}

/// Whether @p label is in kebab case, as wasmparser 0.263.0 defines it (KebabStr::is_kebab_case,
/// src/validator/names.rs:68-92): words of lower- or upper-case letters and digits, one case per word,
/// separated by single hyphens; only the first word must begin with a letter.
/// @param label A label.
/// @return true when it is.
[[nodiscard]] bool isKebabCase(std::string_view label) noexcept;

/// The form of @p label strong uniqueness compares (KebabStr::canonical_chars, names.rs:62-66): hyphens
/// dropped, ASCII letters lowered, so "a-B-c" and "abc" are the same label.
/// @param label A label.
/// @return Its canonical form.
[[nodiscard]] std::string canonicalLabel(std::string_view label);

/// What an extern name is, by the `externname` grammar of the Explainer at the pin (design/mvp/
/// Explainer.md, "Import and Export Definitions"). 📡 `[get]`/`[set]` names are refused while
/// ComponentFeature::GettersSetters is, and 🪺 nested namespaces and projections fail the grammar, as
/// wasm-tools reports them without its feature; the pin defines no other form of name.
enum class NameKind : uint8_t
{
    Plain,       ///< `a-b`: a label.
    Constructor, ///< `[constructor]r`: resource r's constructor.
    Method,      ///< `[method]r.f`: resource r's method f.
    Static,      ///< `[static]r.f`: resource r's static function f.
    Interface,   ///< `ns:pkg/iface@version`: an interface of a package, the version optional.
};

/// A version, as semver.org's Semantic Versioning 2.0 defines it: build metadata is parsed and dropped,
/// as it never decides a match (D8).
struct SemanticVersion
{
    uint64_t major = 0;          ///< The major version.
    uint64_t minor = 0;          ///< The minor version.
    uint64_t patch = 0;          ///< The patch version.
    std::string prerelease = {}; ///< The pre-release identifiers, dot-separated, without the `-`.

    /// Versions order by their fields in turn; the order serves sorting, not semver precedence.
    auto operator<=>(SemanticVersion const&) const = default;
};

/// Why a version failed to parse: the error kinds of the `semver` crate (1.0.28, src/error.rs), whose
/// messages wasm-tools reports for an interface version.
enum class SemanticVersionErrorKind : uint8_t
{
    Empty,
    UnexpectedEnd,
    UnexpectedCharacter,
    UnexpectedCharacterAfter,
    LeadingZero,
    Overflow,
    EmptySegment,
};

/// Which part of a version an error is in: the `semver` crate's Position.
enum class SemanticVersionPart : uint8_t
{
    Major,
    Minor,
    Patch,
    Prerelease,
    Build,
};

/// A version that failed to parse: what went wrong, where, and at which character.
struct SemanticVersionError
{
    SemanticVersionErrorKind kind = SemanticVersionErrorKind::Empty; ///< What went wrong.
    SemanticVersionPart part = SemanticVersionPart::Major;           ///< Where.
    std::string character = {}; ///< The offending character, as UTF-8, for the two character kinds.

    /// Two errors are equal when their fields are.
    bool operator==(SemanticVersionError const&) const = default;
};

/// Parses a version as the `semver` crate's Version::parse does (1.0.28, src/parse.rs:27-82).
/// @param text The version, without the `@`.
/// @return The version, or the first error.
[[nodiscard]] std::expected<SemanticVersion, SemanticVersionError> parseSemanticVersion(
    std::string_view text);

/// The `semver` crate's message for @p error (its Display), which wasm-tools embeds in its own.
/// @param error A parse error.
/// @return Its message, e.g. "unexpected end of input while parsing minor version number".
[[nodiscard]] std::string semanticVersionMessage(SemanticVersionError const& error);

/// Whether a definition of version @p provided satisfies an import of version @p required (D8): the
/// two share their canonical version (Explainer, "Canonical Interface Name"). From 1.0.0 that is the
/// major version; for 0.M.x with M ≥ 1 the major and minor; 0.0.N matches only itself; a pre-release
/// matches only the same version. The relation is symmetric: which of two compatible versions to link
/// is the linker's choice (the highest), and the types decide the rest.
/// @param required The version an import names.
/// @param provided The version a definition offers.
/// @return true when they are compatible.
[[nodiscard]] bool isSemverCompatible(SemanticVersion const& required,
                                      SemanticVersion const& provided) noexcept;

/// Why an extern name failed the grammar.
///
/// The enumerators are the rows of NameErrorMessages, in the same order.
enum class NameErrorCode : uint8_t
{
    NotKebabCase,
    NotLowercase,
    ExpectedText,
    ExpectedPackageSlash,
    MissingCharacter,
    TrailingCharacters,
    AccessorsDisabled,
    InvalidVersion,
};

/// One row of NameErrorMessages: a name error and its text.
struct NameErrorRow
{
    NameErrorCode code;    ///< The error; equal to the row's index.
    std::string_view text; ///< Its text, a std::format string of the error's subject and rest.
};

/// Every name error, in NameErrorCode's order: wasm-tools 1.263.0's texts (wasmparser 0.263.0,
/// src/validator/names.rs:896-1090 and component.rs:4904), {0} the error's subject, {1} its rest.
inline constexpr auto NameErrorMessages = std::to_array<NameErrorRow>({
    { .code = NameErrorCode::NotKebabCase, .text = "`{0}` is not in kebab case" },
    { .code = NameErrorCode::NotLowercase,
      .text = "character `{0}` is not lowercase in package name/namespace" },
    { .code = NameErrorCode::ExpectedText, .text = "expected `{0}` at `{1}`" },
    { .code = NameErrorCode::ExpectedPackageSlash, .text = "expected `/` after package name" },
    { .code = NameErrorCode::MissingCharacter, .text = "failed to find `{0}` character" },
    { .code = NameErrorCode::TrailingCharacters, .text = "trailing characters found: `{0}`" },
    { .code = NameErrorCode::AccessorsDisabled,
      .text = "`[get]` and `[set]` names require the component model accessors feature" },
    { .code = NameErrorCode::InvalidVersion, .text = "invalid interface version: {0}" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: NameErrorMessages)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(NameErrorCode::InvalidVersion) + 1;
    }(),
    "NameErrorMessages must hold one row per NameErrorCode, at the index of its enumerator");

/// An extern name that failed the grammar: why, and what the message names.
struct NameError
{
    NameErrorCode code = NameErrorCode::NotKebabCase; ///< Why.
    std::string subject = {};                         ///< The text {0} of its message names.
    std::string rest = {};                            ///< The text {1} names: the unparsed rest.
    std::optional<ComponentFeature> disabled = {};    ///< For AccessorsDisabled: the feature.

    /// Two errors are equal when their fields are.
    bool operator==(NameError const&) const = default;
};

/// The message wasm-tools reports for @p error, before the validator adds which name it was.
/// @param error A name error.
/// @return Its row's text, formatted.
[[nodiscard]] std::string nameErrorMessage(NameError const& error);

/// An extern name, parsed.
///
/// What the fields hold, by kind: resource and function for Constructor (resource only), Method and
/// Static; package (`ns:pkg`), interface and the optional version for Interface.
struct ExternName
{
    NameKind kind = NameKind::Plain;             ///< What the name is.
    std::string text = {};                       ///< The name, as encoded.
    std::string resource = {};                   ///< The resource a resource function belongs to.
    std::string function = {};                   ///< A method's or static function's own label.
    std::string package = {};                    ///< An interface's namespace and package.
    std::string interface = {};                  ///< An interface's name within its package.
    std::optional<SemanticVersion> version = {}; ///< An interface's version, if it has one.

    /// Two names are equal when their fields are.
    bool operator==(ExternName const&) const = default;
};

/// Parses an extern name by the `externname` grammar, as wasm-tools does (ComponentNameParser,
/// wasmparser 0.263.0 src/validator/names.rs:896-1090), then an interface name's version (component.rs:
/// 4904, InterfaceName::version without a version suffix, as 🔗 is refused).
/// @param text The name, as encoded.
/// @param features The gated features to admit: 📡 (refused in P11) admits `[get]` and `[set]`, 🪺
///                 nested namespaces and projections (refused in P11 too).
/// @return The name, or the first error. An InvalidVersion error is found after the grammar holds; a
///         validator reports it at the stage wasm-tools does, after the name's attributes.
[[nodiscard]] std::expected<ExternName, NameError> parseExternName(std::string_view text,
                                                                   ComponentFeatureSet features);

/// The key under which strong uniqueness compares a name (Explainer, "strongly-unique"; wasmparser's
/// Ord for ComponentName, names.rs:384-470): two names conflict exactly when their keys are equal.
///
/// A plain name's key is its label with hyphens dropped and ASCII letters lowered, `[method]r.r` and
/// `[static]r.r` reduced to `r`, and only `[constructor]` kept of the annotations; an interface name's
/// is its segments, each lowered likewise but its version, which is kept as written.
/// @param name A parsed name.
/// @return Its key.
[[nodiscard]] std::string strongUniquenessKey(ExternName const& name);

} // namespace isoline::component::validate
