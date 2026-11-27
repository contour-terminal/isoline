// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/validate/Names.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <isoline/component/Features.hpp>

namespace isoline::component::validate
{

namespace
{
    // The accessor forms after their gate are unreachable while 📡 is refused: wasm-tools refuses a
    // `[constructor]` with an accessor (names.rs:938-945), and canonicalizes `[set]` apart from `[get]`
    // (names.rs:526-551). Implementing 📡 adds both here, and the accessor rules of Binary.md.
    static_assert(componentFeatureRow(ComponentFeature::GettersSetters).p11 != Implementation::Implemented,
                  "implementing accessors needs their name rules here");
    // Nested namespaces and projections (🪺) parse only under their feature; while it is refused they
    // fail the grammar exactly as wasm-tools reports them without it.
    static_assert(componentFeatureRow(ComponentFeature::NestedNamespaces).p11 != Implementation::Implemented,
                  "implementing nested names needs their grammar here");

    constexpr auto ConstructorPrefix = std::string_view { "[constructor]" };
    constexpr auto MethodPrefix = std::string_view { "[method]" };
    constexpr auto StaticPrefix = std::string_view { "[static]" };
    constexpr auto GetPrefix = std::string_view { "[get]" };
    constexpr auto SetPrefix = std::string_view { "[set]" };

    /// One row of ResourceFunctionPrefixes: an annotation and the kind of name it makes.
    struct PrefixRow
    {
        std::string_view prefix; ///< The annotation, brackets included.
        NameKind kind;           ///< The kind of a name it begins.
    };

    /// The resource function annotations, in the order wasm-tools tries them.
    constexpr auto ResourceFunctionPrefixes = std::to_array<PrefixRow>({
        { .prefix = ConstructorPrefix, .kind = NameKind::Constructor },
        { .prefix = MethodPrefix, .kind = NameKind::Method },
        { .prefix = StaticPrefix, .kind = NameKind::Static },
    });

    template <typename T>
    using Parsed = std::expected<T, NameError>;

    [[nodiscard]] NameError nameError(NameErrorCode code,
                                      std::string_view subject,
                                      std::string_view rest = {})
    {
        return NameError { .code = code, .subject = std::string { subject }, .rest = std::string { rest } };
    }

    [[nodiscard]] constexpr bool isKebabCharacter(char character) noexcept
    {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
               || (character >= '0' && character <= '9') || character == '-';
    }

    /// wasmparser's ComponentNameParser (names.rs:896-1090): the unparsed rest of a name, consumed as
    /// the grammar is matched, every refusal its own.
    class NameParser
    {
      public:
        explicit NameParser(std::string_view text): _next { text } {}

        [[nodiscard]] std::string_view rest() const noexcept { return _next; }

        [[nodiscard]] bool eat(std::string_view prefix) noexcept
        {
            if (!_next.starts_with(prefix))
                return false;
            _next.remove_prefix(prefix.size());
            return true;
        }

        [[nodiscard]] Parsed<void> expect(std::string_view prefix)
        {
            if (eat(prefix))
                return {};
            return std::unexpected { nameError(NameErrorCode::ExpectedText, prefix, _next) };
        }

        [[nodiscard]] static Parsed<std::string_view> kebab(std::string_view text)
        {
            if (isKebabCase(text))
                return text;
            return std::unexpected { nameError(NameErrorCode::NotKebabCase, text) };
        }

        /// The text up to @p character, which is consumed too.
        [[nodiscard]] Parsed<std::string_view> takeUntil(char character)
        {
            auto const found = _next.find(character);
            if (found == std::string_view::npos)
                return std::unexpected { nameError(NameErrorCode::MissingCharacter,
                                                   std::string_view { &character, 1 }) };
            auto const taken = _next.substr(0, found);
            _next.remove_prefix(found + 1);
            return taken;
        }

        [[nodiscard]] std::string_view takeRest() noexcept
        {
            return std::exchange(_next, std::string_view {});
        }

        [[nodiscard]] Parsed<std::string_view> expectKebab() { return kebab(takeRest()); }

        /// The longest prefix of kebab characters, which must be a label; the whole rest if all are.
        [[nodiscard]] Parsed<std::string_view> takeKebab()
        {
            auto const end = std::ranges::find_if_not(_next, isKebabCharacter);
            if (end == _next.end())
                return expectKebab();
            auto const length = static_cast<std::size_t>(end - _next.begin());
            auto const taken = _next.substr(0, length);
            _next.remove_prefix(length);
            return kebab(taken);
        }

        [[nodiscard]] Parsed<std::string_view> takeLowercaseKebab()
        {
            auto const taken = takeKebab();
            if (!taken)
                return taken;
            // A label holds ASCII letters, digits and hyphens only, so "alphabetic and not lowercase"
            // is an upper-case ASCII letter.
            auto const upper = std::ranges::find_if(*taken, [](char c) { return c >= 'A' && c <= 'Z'; });
            if (upper != taken->end())
                return std::unexpected { nameError(NameErrorCode::NotLowercase,
                                                   std::string_view { &*upper, 1 }) };
            return taken;
        }

      private:
        std::string_view _next;
    };

    /// `pkgpath` (names.rs:972-1001), without 🪺's further namespaces and projections.
    [[nodiscard]] Parsed<void> packagePath(NameParser& parser, ExternName& name)
    {
        auto const namespaceLabel = parser.takeLowercaseKebab();
        if (!namespaceLabel)
            return std::unexpected { namespaceLabel.error() };
        if (auto colon = parser.expect(":"); !colon)
            return colon;
        auto const packageLabel = parser.takeLowercaseKebab();
        if (!packageLabel)
            return std::unexpected { packageLabel.error() };
        name.package = std::format("{}:{}", *namespaceLabel, *packageLabel);
        if (!parser.rest().starts_with('/'))
            return std::unexpected { nameError(NameErrorCode::ExpectedPackageSlash, {}) };
        if (auto slash = parser.expect("/"); !slash)
            return slash;
        auto const interfaceLabel = parser.takeKebab();
        if (!interfaceLabel)
            return std::unexpected { interfaceLabel.error() };
        name.interface = std::string { *interfaceLabel };
        return {};
    }

    /// `plainname` (names.rs:910-956).
    [[nodiscard]] Parsed<void> plainName(NameParser& parser, ExternName& name, ComponentFeatureSet features)
    {
        for (auto const& row: ResourceFunctionPrefixes)
        {
            if (parser.eat(row.prefix))
            {
                name.kind = row.kind;
                break;
            }
        }
        if ((parser.eat(GetPrefix) || parser.eat(SetPrefix))
            && !validationAdmits(features, ComponentFeature::GettersSetters))
        {
            auto refused = nameError(NameErrorCode::AccessorsDisabled, {});
            refused.disabled = ComponentFeature::GettersSetters;
            return std::unexpected { std::move(refused) };
        }
        if (name.kind == NameKind::Method || name.kind == NameKind::Static)
        {
            auto const resource = parser.takeUntil('.').and_then(&NameParser::kebab);
            if (!resource)
                return std::unexpected { resource.error() };
            name.resource = std::string { *resource };
        }
        auto const label = parser.expectKebab();
        if (!label)
            return std::unexpected { label.error() };
        if (name.kind == NameKind::Constructor)
            name.resource = std::string { *label };
        else if (name.kind != NameKind::Plain)
            name.function = std::string { *label };
        return {};
    }

    /// Writes @p character as Rust's Debug of a char does, quotes included (semver's QuotedChar): the
    /// escapes for quote, backslash, tab, return, newline and NUL; other ASCII controls as `\u{…}`;
    /// anything else as itself. Rust also escapes non-printable characters beyond ASCII, which no
    /// suite names; they are written as themselves here.
    [[nodiscard]] std::string quotedCharacter(std::string_view character)
    {
        if (character.size() == 1)
        {
            auto const byte = static_cast<unsigned char>(character.front());
            switch (byte)
            {
                case '\'': return R"('\'')";
                case '\\': return R"('\\')";
                case '\t': return R"('\t')";
                case '\r': return R"('\r')";
                case '\n': return R"('\n')";
                case '\0': return R"('\0')";
                default: break;
            }
            if (byte < 0x20 || byte == 0x7F)
                return std::format("'\\u{{{:x}}}'", byte);
        }
        return std::format("'{}'", character);
    }

    /// The UTF-8 encoded character @p text begins with (the name is valid UTF-8).
    [[nodiscard]] std::string_view firstCharacter(std::string_view text) noexcept
    {
        Require(!text.empty());
        auto const lead = static_cast<unsigned char>(text.front());
        auto length = std::size_t { 4 };
        if (lead < 0x80)
            length = 1;
        else if (lead < 0xE0)
            length = 2;
        else if (lead < 0xF0)
            length = 3;
        return text.substr(0, std::min(length, text.size()));
    }

    /// One row of SemanticVersionErrorTexts: an error kind and its text, {0} the part, {1} the
    /// character quoted.
    struct SemanticVersionErrorRow
    {
        SemanticVersionErrorKind kind; ///< The kind; equal to the row's index.
        std::string_view text;         ///< Its text.
    };

    /// The `semver` crate's messages (1.0.28, src/error.rs:31-72), in SemanticVersionErrorKind's order.
    constexpr auto SemanticVersionErrorTexts = std::to_array<SemanticVersionErrorRow>({
        { .kind = SemanticVersionErrorKind::Empty, .text = "empty string, expected a semver version" },
        { .kind = SemanticVersionErrorKind::UnexpectedEnd,
          .text = "unexpected end of input while parsing {0}" },
        { .kind = SemanticVersionErrorKind::UnexpectedCharacter,
          .text = "unexpected character {1} while parsing {0}" },
        { .kind = SemanticVersionErrorKind::UnexpectedCharacterAfter,
          .text = "unexpected character {1} after {0}" },
        { .kind = SemanticVersionErrorKind::LeadingZero, .text = "invalid leading zero in {0}" },
        { .kind = SemanticVersionErrorKind::Overflow, .text = "value of {0} exceeds u64::MAX" },
        { .kind = SemanticVersionErrorKind::EmptySegment, .text = "empty identifier segment in {0}" },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: SemanticVersionErrorTexts)
            {
                if (static_cast<std::size_t>(row.kind) != index)
                    return false;
                ++index;
            }
            return index == static_cast<std::size_t>(SemanticVersionErrorKind::EmptySegment) + 1;
        }(),
        "SemanticVersionErrorTexts holds one row per SemanticVersionErrorKind, in its order");

    /// What each part of a version is called in the messages (Position's Display), by enumerator.
    constexpr auto SemanticVersionPartWords = std::to_array<std::string_view>({
        "major version number",
        "minor version number",
        "patch version number",
        "pre-release identifier",
        "build metadata",
    });

    static_assert(SemanticVersionPartWords.size() == static_cast<std::size_t>(SemanticVersionPart::Build) + 1,
                  "SemanticVersionPartWords holds one word per SemanticVersionPart");

    template <typename T>
    using Version = std::expected<T, SemanticVersionError>;

    [[nodiscard]] SemanticVersionError versionError(SemanticVersionErrorKind kind,
                                                    SemanticVersionPart part,
                                                    std::string_view rest = {})
    {
        return SemanticVersionError { .kind = kind,
                                      .part = part,
                                      .character = rest.empty() ? std::string {}
                                                                : std::string { firstCharacter(rest) } };
    }

    [[nodiscard]] constexpr bool isDigit(char character) noexcept
    {
        return character >= '0' && character <= '9';
    }

    /// numeric_identifier (parse.rs:162-189): digits, no leading zero, within u64.
    [[nodiscard]] Version<uint64_t> numericIdentifier(std::string_view& text, SemanticVersionPart part)
    {
        auto value = uint64_t { 0 };
        auto length = std::size_t { 0 };
        for (auto const character: text)
        {
            if (!isDigit(character))
                break;
            if (value == 0 && length > 0)
                return std::unexpected { versionError(SemanticVersionErrorKind::LeadingZero, part) };
            auto const digit = static_cast<uint64_t>(character - '0');
            if (value > (UINT64_MAX - digit) / 10)
                return std::unexpected { versionError(SemanticVersionErrorKind::Overflow, part) };
            value = (value * 10) + digit;
            ++length;
        }
        if (length == 0)
            return std::unexpected { versionError(text.empty()
                                                      ? SemanticVersionErrorKind::UnexpectedEnd
                                                      : SemanticVersionErrorKind::UnexpectedCharacter,
                                                  part,
                                                  text) };
        text.remove_prefix(length);
        return value;
    }

    /// dot (parse.rs:204-212).
    [[nodiscard]] Version<void> dot(std::string_view& text, SemanticVersionPart part)
    {
        if (text.starts_with('.'))
        {
            text.remove_prefix(1);
            return {};
        }
        return std::unexpected { versionError(text.empty()
                                                  ? SemanticVersionErrorKind::UnexpectedEnd
                                                  : SemanticVersionErrorKind::UnexpectedCharacterAfter,
                                              part,
                                              text) };
    }

    /// identifier (parse.rs:226-265): dot-separated segments of ASCII alphanumerics and hyphens; for
    /// a pre-release, a numeric segment has no leading zero.
    [[nodiscard]] Version<std::string_view> identifier(std::string_view& text, SemanticVersionPart part)
    {
        auto accumulated = std::size_t { 0 };
        auto segment = std::size_t { 0 };
        auto nonDigit = false;
        while (true)
        {
            auto const position = accumulated + segment;
            auto const boundary =
                position < text.size() ? std::optional<char> { text[position] } : std::nullopt;
            if (boundary
                && ((*boundary >= 'A' && *boundary <= 'Z') || (*boundary >= 'a' && *boundary <= 'z')
                    || *boundary == '-'))
            {
                ++segment;
                nonDigit = true;
                continue;
            }
            if (boundary && isDigit(*boundary))
            {
                ++segment;
                continue;
            }
            if (segment == 0)
            {
                if (accumulated == 0 && boundary != '.')
                    return std::string_view {};
                return std::unexpected { versionError(SemanticVersionErrorKind::EmptySegment, part) };
            }
            if (part == SemanticVersionPart::Prerelease && segment > 1 && !nonDigit
                && text[accumulated] == '0')
                return std::unexpected { versionError(SemanticVersionErrorKind::LeadingZero, part) };
            accumulated += segment;
            if (boundary == '.')
            {
                ++accumulated;
                segment = 0;
                nonDigit = false;
                continue;
            }
            auto const taken = text.substr(0, accumulated);
            text.remove_prefix(accumulated);
            return taken;
        }
    }

    /// The parts of a version a separator introduces: a pre-release (`-`) or build metadata (`+`).
    [[nodiscard]] Version<std::string_view> suffix(std::string_view& text,
                                                   char separator,
                                                   SemanticVersionPart part)
    {
        if (!text.starts_with(separator))
            return std::string_view {};
        text.remove_prefix(1);
        auto const taken = identifier(text, part);
        if (taken && taken->empty())
            return std::unexpected { versionError(SemanticVersionErrorKind::EmptySegment, part) };
        return taken;
    }
} // namespace

bool isKebabCase(std::string_view label) noexcept
{
    auto lower = false;
    auto upper = false;
    auto first = true;
    auto digit = false;
    for (auto const character: label)
    {
        auto const isLower = character >= 'a' && character <= 'z';
        auto const isUpper = character >= 'A' && character <= 'Z';
        auto const isNumber = isDigit(character);
        auto const inWord = lower || upper;
        if (isLower && !inWord)
            lower = true;
        else if (isUpper && !inWord)
            upper = true;
        else if (isNumber && !inWord && !first)
            digit = true;
        else if ((isLower && lower) || (isUpper && upper))
            continue;
        else if (isNumber && inWord)
            digit = true;
        else if (character == '-' && (inWord || digit))
        {
            lower = false;
            upper = false;
            first = false;
            digit = false;
        }
        else
            return false;
    }
    return !label.empty() && !label.ends_with('-');
}

std::string canonicalLabel(std::string_view label)
{
    auto canonical = std::string {};
    canonical.reserve(label.size());
    for (auto const character: label)
    {
        if (character == '-')
            continue;
        canonical.push_back(character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                                 : character);
    }
    return canonical;
}

std::expected<SemanticVersion, SemanticVersionError> parseSemanticVersion(std::string_view text)
{
    // Version::from_str (parse.rs:27-82).
    using enum SemanticVersionPart;
    if (text.empty())
        return std::unexpected { versionError(SemanticVersionErrorKind::Empty, Major) };
    auto version = SemanticVersion {};
    auto const major = numericIdentifier(text, Major);
    if (!major)
        return std::unexpected { major.error() };
    if (auto separated = dot(text, Major); !separated)
        return std::unexpected { separated.error() };
    auto const minor = numericIdentifier(text, Minor);
    if (!minor)
        return std::unexpected { minor.error() };
    if (auto separated = dot(text, Minor); !separated)
        return std::unexpected { separated.error() };
    auto const patch = numericIdentifier(text, Patch);
    if (!patch)
        return std::unexpected { patch.error() };
    version.major = *major;
    version.minor = *minor;
    version.patch = *patch;
    auto part = Patch;
    if (text.starts_with('-'))
        part = Prerelease;
    auto const prerelease = suffix(text, '-', Prerelease);
    if (!prerelease)
        return std::unexpected { prerelease.error() };
    version.prerelease = std::string { *prerelease };
    if (text.starts_with('+'))
        part = Build;
    auto const build = suffix(text, '+', Build);
    if (!build)
        return std::unexpected { build.error() };
    if (!text.empty())
        return std::unexpected { versionError(
            SemanticVersionErrorKind::UnexpectedCharacterAfter, part, text) };
    return version;
}

std::string semanticVersionMessage(SemanticVersionError const& error)
{
    auto const& row = SemanticVersionErrorTexts[static_cast<std::size_t>(error.kind)];
    auto const part = SemanticVersionPartWords[static_cast<std::size_t>(error.part)];
    auto const quoted = error.character.empty() ? std::string {} : quotedCharacter(error.character);
    return std::vformat(row.text, std::make_format_args(part, quoted));
}

bool isSemverCompatible(SemanticVersion const& required, SemanticVersion const& provided) noexcept
{
    // The canonical version of each (Explainer, "Canonical Interface Name"): a pre-release is not
    // split, so it is the whole version; otherwise the major from 1.0.0, the major and minor for 0.M.x
    // (M ≥ 1), and the whole version for 0.0.N.
    if (!required.prerelease.empty() || !provided.prerelease.empty())
        return required == provided;
    if (required.major != provided.major)
        return false;
    if (required.major != 0)
        return true;
    if (required.minor != provided.minor)
        return false;
    if (required.minor != 0)
        return true;
    return required.patch == provided.patch;
}

std::string nameErrorMessage(NameError const& error)
{
    auto const& row = NameErrorMessages[static_cast<std::size_t>(error.code)];
    return std::vformat(row.text, std::make_format_args(error.subject, error.rest));
}

std::expected<ExternName, NameError> parseExternName(std::string_view text, ComponentFeatureSet features)
{
    // ComponentName::new_with_features (names.rs:297-312): a name holding `:` is an interface name.
    auto name = ExternName { .text = std::string { text } };
    auto parser = NameParser { text };
    auto versionText = std::optional<std::string_view> {};
    if (text.contains(':'))
    {
        name.kind = NameKind::Interface;
        if (auto parsed = packagePath(parser, name); !parsed)
            return std::unexpected { parsed.error() };
        // The version is checked once the name holds, as wasm-tools validates it (below).
        if (parser.eat("@"))
            versionText = parser.takeRest();
    }
    else if (auto parsed = plainName(parser, name, features); !parsed)
        return std::unexpected { parsed.error() };
    if (!parser.rest().empty())
        return std::unexpected { nameError(NameErrorCode::TrailingCharacters, parser.rest()) };
    if (versionText)
    {
        // InterfaceName::version (names.rs:708-730) without a version suffix: Version::parse.
        auto const version = parseSemanticVersion(*versionText);
        if (!version)
            return std::unexpected { nameError(NameErrorCode::InvalidVersion,
                                               semanticVersionMessage(version.error())) };
        name.version = *version;
    }
    return name;
}

std::string strongUniquenessKey(ExternName const& name)
{
    if (name.kind == NameKind::Interface)
    {
        // InterfaceName::components (names.rs:737-768): segments split at `:`, `/` and `@`, each a
        // label but the one after the last `@`, which is a version and compared as written.
        auto key = std::string { "I" };
        auto next = std::string_view { name.text };
        auto previous = char { 0 };
        while (!next.empty())
        {
            auto const found = next.find_first_of(":/@");
            auto const segment = next.substr(0, std::min(found, next.size()));
            auto const isVersion = found == std::string_view::npos && previous == '@';
            auto const text = isVersion ? std::string { segment } : canonicalLabel(segment);
            auto tag = 'p'; // a projection, or the package before a `/`
            if (isVersion)
                tag = 'v';
            else if (found != std::string_view::npos && next[found] == ':')
                tag = 'n';
            key += std::format("{}{}:{}", tag, text.size(), text);
            if (found == std::string_view::npos)
                break;
            previous = next[found];
            next.remove_prefix(found + 1);
        }
        return key;
    }
    // PlainName::canonicalized (names.rs:526-551).
    auto unannotated = std::string_view { name.text };
    for (auto const& row: ResourceFunctionPrefixes)
    {
        if (row.kind == name.kind)
            unannotated.remove_prefix(row.prefix.size());
    }
    auto const lower = canonicalLabel(unannotated);
    if (auto const dot = lower.find('.'); dot != std::string::npos)
    {
        if (std::string_view { lower }.substr(0, dot) == std::string_view { lower }.substr(dot + 1))
            return std::format("P{}", lower.substr(0, dot));
    }
    if (name.kind == NameKind::Constructor)
        return std::format("P{}{}", ConstructorPrefix, lower);
    return std::format("P{}", lower);
}

} // namespace isoline::component::validate
