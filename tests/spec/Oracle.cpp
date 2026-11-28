// SPDX-License-Identifier: Apache-2.0
#include "Oracle.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "Hex.hpp"
#include "Script.hpp"

namespace isoline::spectest
{

namespace
{
    /// Whether a command loads a module of its own.
    [[nodiscard]] bool hasModule(Command const& command) noexcept
    {
        switch (command.kind)
        {
            case CommandKind::Module:
            case CommandKind::ModuleDefinition:
            case CommandKind::AssertTrapModule:
            case CommandKind::AssertInvalid:
            case CommandKind::AssertMalformed:
            case CommandKind::AssertUnlinkable:
            case CommandKind::AssertUninstantiable:
            case CommandKind::AssertInvalidCustom:
            case CommandKind::AssertMalformedCustom: return true;
            case CommandKind::ModuleInstance:
            case CommandKind::Register:
            case CommandKind::Invoke:
            case CommandKind::AssertReturn:
            case CommandKind::AssertTrap:
            case CommandKind::AssertExhaustion:
            case CommandKind::AssertException:
            case CommandKind::Skip: break;
        }
        return false;
    }

    /// Whether a command loads a module or a component the script gives as text, `quote` included: the
    /// only ones the oracle's binary is checked against, and whose text can diverge in its error.
    [[nodiscard]] bool hasTextModule(Command const& command) noexcept
    {
        return hasModule(command) && isTextForm(command.source.form);
    }

    /// What a command is, for a message: its kind, and the form of its module when it has one.
    [[nodiscard]] std::string describeKind(Command const& command)
    {
        constexpr auto Kinds = std::to_array<std::string_view>({
            "a module",
            "a module definition",
            "a module instance",
            "a register",
            "an action",
            "an assert_return",
            "an assert_trap",
            "an assert_trap of a module",
            "an assert_exhaustion",
            "an assert_invalid",
            "an assert_malformed",
            "an assert_unlinkable",
            "an assert_uninstantiable",
            "an assert_exception",
            "an assert_invalid_custom",
            "an assert_malformed_custom",
            "a skip",
        });
        static_assert(Kinds.size() == static_cast<std::size_t>(CommandKind::Skip) + 1,
                      "Kinds names every CommandKind, in enumerator order");
        constexpr auto Forms = std::to_array<std::string_view>(
            { "text", "binary", "quote", "component text", "component binary", "component quote" });
        static_assert(Forms.size() == static_cast<std::size_t>(ModuleForm::ComponentQuote) + 1,
                      "Forms names every ModuleForm, in enumerator order");
        auto const kind = Kinds.at(static_cast<std::size_t>(command.kind));
        if (!hasModule(command))
            return std::string { kind };
        return std::format("{} ({} form)", kind, Forms.at(static_cast<std::size_t>(command.source.form)));
    }

    using LineResult = std::expected<void, std::string>;

    /// @p text without the CR of a CR LF line ending, which a checkout that converts line endings leaves.
    [[nodiscard]] std::string_view withoutCarriageReturn(std::string_view text) noexcept
    {
        if (text.ends_with('\r'))
            text.remove_suffix(1);
        return text;
    }

    /// A decimal number, the whole of @p text but a trailing CR.
    [[nodiscard]] std::expected<std::size_t, std::string> parseDecimal(std::string_view text,
                                                                       std::string_view what)
    {
        text = withoutCarriageReturn(text);
        if (text.empty())
            return std::unexpected { std::format("malformed {} ''", what) };
        auto value = std::size_t { 0 };
        auto const* const first = &text.front();
        auto const* const end = first + text.size();
        auto const [rest, error] = std::from_chars(first, end, value);
        if (error != std::errc {} || rest != end)
            return std::unexpected { std::format("malformed {} '{}'", what, text) };
        return value;
    }

    /// The tokens of a line, separated by exactly one space.
    [[nodiscard]] std::vector<std::string_view> tokensOf(std::string_view line)
    {
        auto tokens = std::vector<std::string_view> {};
        for (auto const token: line | std::views::split(' '))
            tokens.emplace_back(token.begin(), token.end());
        return tokens;
    }

    /// Sets a member of an entry that the manifest may give once.
    [[nodiscard]] LineResult setOnce(std::optional<std::string>& member,
                                     std::string value,
                                     std::string_view kind)
    {
        if (member)
            return std::unexpected { std::format("a second {} line for one command", kind) };
        member = std::move(value);
        return {};
    }

    /// How an entry line spells its value.
    enum class ValueEncoding : uint8_t
    {
        Verbatim, ///< As it is: a file name.
        Hex,      ///< Hex-encoded UTF-8: a reason, which may hold spaces.
    };

    /// Whether every character of @p text is a lower-case hex digit, and there is one.
    [[nodiscard]] bool isLowerHex(std::string_view text) noexcept
    {
        return !text.empty() && std::ranges::all_of(text, [](char character) {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        });
    }

    /// Checks a parity divergence's shape, `0x<offset> sha256:<digest> <reason>`: the offset as the parity
    /// check prints it (lower-case hex digits without leading zeros), the 64 lower-case hex digits of
    /// Isoline's encoding's SHA-256, and a reason that is not empty.
    [[nodiscard]] LineResult checkParityDivergence(std::string_view divergence)
    {
        constexpr auto OffsetPrefix = std::string_view { "0x" };
        constexpr auto DigestPrefix = std::string_view { "sha256:" };
        constexpr auto DigestDigits = std::size_t { 64 };
        auto const offsetEnd = divergence.find(' ');
        auto const offset = divergence.substr(0, offsetEnd);
        auto const rest =
            offsetEnd == std::string_view::npos ? std::string_view {} : divergence.substr(offsetEnd + 1);
        auto const digestEnd = rest.find(' ');
        auto const digest = rest.substr(0, digestEnd);
        auto const reason =
            digestEnd == std::string_view::npos ? std::string_view {} : rest.substr(digestEnd + 1);
        auto const digits = offset.substr(std::min(offset.size(), OffsetPrefix.size()));
        auto const hash = digest.substr(std::min(digest.size(), DigestPrefix.size()));
        if (!offset.starts_with(OffsetPrefix) || !isLowerHex(digits)
            || (digits.size() > 1 && digits.front() == '0') || !digest.starts_with(DigestPrefix)
            || hash.size() != DigestDigits || !isLowerHex(hash) || reason.empty())
            return std::unexpected { std::format(
                "malformed parity divergence '{}': expected '0x<offset> sha256:<digest> <reason>'",
                divergence) };
        return {};
    }

    /// What an entry line's kind sets: its member of the entry, from the line's last token.
    struct EntryKind
    {
        std::string_view keyword;                         ///< The line's third token.
        std::optional<std::string> OracleEntry::* member; ///< What it sets.
        ValueEncoding encoding;                           ///< How the line spells the value.
        LineResult (*checkValue)(std::string_view);       ///< Checks the decoded value, or nullptr.
        /// What the value is, for the message that refuses it beside a skip, or empty when a skipped
        /// command may have it: a skipped command never runs, so what it is checked against means nothing.
        std::string_view refusedBesideSkip;
    };

    /// Every kind of entry line. Adding one is adding a row, and a member to OracleEntry.
    constexpr auto EntryKinds = std::to_array<EntryKind>({
        { .keyword = "wasm",
          .member = &OracleEntry::wasm,
          .encoding = ValueEncoding::Verbatim,
          .checkValue = nullptr,
          .refusedBesideSkip = {} },
        { .keyword = "skip",
          .member = &OracleEntry::skip,
          .encoding = ValueEncoding::Hex,
          .checkValue = nullptr,
          .refusedBesideSkip = {} },
        { .keyword = "message",
          .member = &OracleEntry::message,
          .encoding = ValueEncoding::Hex,
          .checkValue = nullptr,
          .refusedBesideSkip = "a message" },
        { .keyword = "parity",
          .member = &OracleEntry::parity,
          .encoding = ValueEncoding::Hex,
          .checkValue = checkParityDivergence,
          .refusedBesideSkip = "a parity divergence" },
    });

    /// An entry line, `<ordinal> <line> <kind> <value>`, into @p oracle.
    [[nodiscard]] LineResult parseEntry(std::string_view line, Oracle& oracle)
    {
        auto const tokens = tokensOf(line);
        if (tokens.size() != 4)
            return std::unexpected { std::format(
                "expected '<ordinal> <line> wasm|skip|message|parity <value>', got '{}'", line) };
        auto const ordinal = parseDecimal(tokens[0], "ordinal");
        if (!ordinal)
            return std::unexpected { ordinal.error() };
        auto const scriptLine = parseDecimal(tokens[1], "line");
        if (!scriptLine)
            return std::unexpected { scriptLine.error() };
        // An empty message would match every message, and an empty file or reason means nothing.
        if (tokens[3].empty())
            return std::unexpected { std::format("an empty {} value", tokens[2]) };
        auto const kind = std::ranges::find(EntryKinds, tokens[2], &EntryKind::keyword);
        if (kind == EntryKinds.end())
            return std::unexpected { std::format("unknown entry kind '{}'", tokens[2]) };
        auto value = kind->encoding == ValueEncoding::Hex
                         ? decodeHex(tokens[3])
                         : std::expected<std::string, std::string> { std::string { tokens[3] } };
        if (!value)
            return std::unexpected { value.error() };
        if (kind->checkValue)
            if (auto const checked = kind->checkValue(*value); !checked)
                return checked;
        auto& entry = oracle.entries.try_emplace(*ordinal, *ordinal, *scriptLine).first->second;
        if (entry.line != *scriptLine)
            return std::unexpected { std::format(
                "command {} is on line {} here, but on line {} before", *ordinal, *scriptLine, entry.line) };
        if (auto const set = setOnce(entry.*(kind->member), std::move(*value), kind->keyword); !set)
            return set;
        if (!entry.skip)
            return {};
        for (auto const& refused: EntryKinds)
            if (!refused.refusedBesideSkip.empty() && entry.*(refused.member))
                return std::unexpected { std::format(
                    "command {} is both skipped and given {}", *ordinal, refused.refusedBesideSkip) };
        return {};
    }

    /// The first line, `commands <n>`.
    [[nodiscard]] std::expected<std::size_t, std::string> parseCount(std::string_view line)
    {
        constexpr auto Prefix = std::string_view { "commands " };
        if (!line.starts_with(Prefix))
            return std::unexpected { std::format("expected 'commands <n>', got '{}'", line) };
        return parseDecimal(line.substr(Prefix.size()), "command count");
    }
} // namespace

std::expected<Oracle, ParseError> parseOracle(std::string_view manifest)
{
    auto oracle = Oracle {};
    auto count = std::optional<std::size_t> {};
    auto number = std::size_t { 0 };
    while (!manifest.empty())
    {
        ++number;
        auto const end = manifest.find('\n');
        // .gitattributes checks manifests out with LF endings; a CR LF one reads the same regardless.
        auto const line = withoutCarriageReturn(manifest.substr(0, end));
        manifest = end == std::string_view::npos ? std::string_view {} : manifest.substr(end + 1);
        if (line.empty())
            continue;
        auto const parsed = count ? parseEntry(line, oracle)
                                  : parseCount(line).transform([&](std::size_t value) { count = value; });
        if (!parsed)
            return std::unexpected { ParseError { .line = number, .message = parsed.error() } };
    }
    if (!count)
        return std::unexpected { ParseError { .line = number, .message = "missing 'commands <n>'" } };
    oracle.commands = *count;
    return oracle;
}

std::expected<void, std::string> checkAlignment(Oracle const& oracle, std::size_t commands)
{
    if (commands != oracle.commands)
        return std::unexpected { std::format(
            "the script holds {} commands, but its oracle counts {}", commands, oracle.commands) };
    if (!oracle.entries.empty() && oracle.entries.rbegin()->first >= commands)
        return std::unexpected { std::format("the oracle names command {}, but the script holds {}",
                                             oracle.entries.rbegin()->first,
                                             commands) };
    return {};
}

std::expected<void, std::string> checkEntries(Oracle const& oracle, std::span<Command const> commands)
{
    for (auto const& [ordinal, entry]: oracle.entries)
    {
        auto const& command = commands[ordinal];
        auto const fails = [&](std::string_view why) {
            return std::unexpected { std::format("the oracle's entry for command {} (line {}) {}, but the "
                                                 "command on line {} is {}",
                                                 ordinal,
                                                 entry.line,
                                                 why,
                                                 command.line,
                                                 describeKind(command)) };
        };
        if (entry.wasm && !hasTextModule(command))
            return fails("names a binary of a module given as text");
        if (entry.parity && !entry.wasm)
            return fails("records a parity divergence without the oracle's binary");
        if (entry.message)
        {
            if ((command.kind != CommandKind::AssertMalformed
                 && command.kind != CommandKind::AssertMalformedCustom)
                || !hasTextModule(command))
                return fails("names a message of malformed text");
            if (*entry.message == command.text)
                return std::unexpected { std::format(
                    "the oracle's message for command {} (line {}) is the script's own, so no divergence",
                    ordinal,
                    entry.line) };
        }
    }
    return {};
}

} // namespace isoline::spectest
