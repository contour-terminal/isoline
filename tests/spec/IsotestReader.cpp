// SPDX-License-Identifier: Apache-2.0
#include "IsotestReader.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::spectest
{

namespace
{
    using runtime::Ref;
    using runtime::RefKind;
    using runtime::Value;
    using Pattern = ExpectedValue::Pattern;

    /// What failed on a line; the line number is added by the caller.
    using LineResult = std::expected<void, std::string>;

    /// The tokens of one line, consumed front to back.
    class Tokens
    {
      public:
        explicit Tokens(std::string_view line)
        {
            // Exactly one space separates two tokens, so two adjacent spaces delimit an empty one.
            auto start = std::size_t { 0 };
            for (auto const position: std::views::iota(std::size_t { 0 }, line.size()))
            {
                if (line[position] != ' ')
                    continue;
                _tokens.push_back(line.substr(start, position - start));
                start = position + 1;
            }
            _tokens.push_back(line.substr(start));
        }

        [[nodiscard]] bool atEnd() const noexcept { return _next == _tokens.size(); }

        /// The next token, or an error naming @p what was missing.
        [[nodiscard]] std::expected<std::string_view, std::string> next(std::string_view what)
        {
            if (atEnd())
                return std::unexpected { std::format("missing {}", what) };
            return _tokens[_next++];
        }

        /// The next token without consuming it; empty at the end.
        [[nodiscard]] std::string_view peek() const noexcept
        {
            return atEnd() ? std::string_view {} : _tokens[_next];
        }

        /// Succeeds when every token was consumed.
        [[nodiscard]] LineResult finish() const
        {
            if (!atEnd())
                return std::unexpected { std::format("unexpected token '{}'", _tokens[_next]) };
            return {};
        }

      private:
        std::vector<std::string_view> _tokens;
        std::size_t _next = 0;
    };

    [[nodiscard]] std::optional<uint8_t> hexDigit(char digit) noexcept
    {
        if (digit >= '0' && digit <= '9')
            return static_cast<uint8_t>(digit - '0');
        if (digit >= 'a' && digit <= 'f')
            return static_cast<uint8_t>(digit - 'a' + 10);
        return std::nullopt;
    }

    /// Decodes a hex-encoded UTF-8 string.
    [[nodiscard]] std::expected<std::string, std::string> decodeHex(std::string_view hex)
    {
        if (hex.size() % 2 != 0)
            return std::unexpected { std::format("odd-length hex string '{}'", hex) };
        auto text = std::string {};
        text.reserve(hex.size() / 2);
        for (auto const index: std::views::iota(std::size_t { 0 }, hex.size() / 2))
        {
            auto const high = hexDigit(hex[2 * index]);
            auto const low = hexDigit(hex[(2 * index) + 1]);
            if (!high || !low)
                return std::unexpected { std::format("malformed hex string '{}'", hex) };
            text.push_back(static_cast<char>((*high << 4) | *low));
        }
        return text;
    }

    /// Parses an unsigned number of at most @p maxValue in @p base, the whole of @p digits.
    [[nodiscard]] std::expected<uint64_t, std::string> parseNumber(std::string_view digits,
                                                                   int base,
                                                                   uint64_t maxValue)
    {
        if (digits.empty())
            return std::unexpected { std::string { "missing number" } };
        auto value = uint64_t { 0 };
        auto const* const first = &digits.front();
        auto const* const end = first + digits.size();
        auto const [rest, error] = std::from_chars(first, end, value, base);
        if (error != std::errc {} || rest != end || value > maxValue)
            return std::unexpected { std::format("malformed number '{}'", digits) };
        return value;
    }

    constexpr auto FuncHeap = binary::HeapType { .kind = binary::AbstractHeapType::Func };
    constexpr auto ExternHeap = binary::HeapType { .kind = binary::AbstractHeapType::Extern };
    constexpr auto Max32 = uint64_t { std::numeric_limits<uint32_t>::max() };
    constexpr auto Max64 = std::numeric_limits<uint64_t>::max();

    /// A value token spelled exactly so: the pattern it stands for, and for Exact the value. It
    /// holds no ExpectedValue, whose std::vector member GCC and MSVC refuse in a constexpr table.
    struct FixedValue
    {
        std::string_view token;
        Pattern pattern;
        Value value;
    };

    constexpr auto exact(Value value) noexcept
    {
        return ExpectedValue { .pattern = Pattern::Exact, .value = value };
    }

    constexpr auto pattern(Pattern kind) noexcept
    {
        return ExpectedValue { .pattern = kind, .value = {} };
    }

    constexpr auto NullFunc = Ref { .kind = RefKind::Null, .heap = FuncHeap, .address = 0 };
    constexpr auto NullExtern = Ref { .kind = RefKind::Null, .heap = ExternHeap, .address = 0 };

    constexpr auto FixedValues = std::to_array<FixedValue>({
        { .token = "f32:nan:canonical", .pattern = Pattern::CanonicalNan32, .value = {} },
        { .token = "f32:nan:arithmetic", .pattern = Pattern::ArithmeticNan32, .value = {} },
        { .token = "f64:nan:canonical", .pattern = Pattern::CanonicalNan64, .value = {} },
        { .token = "f64:nan:arithmetic", .pattern = Pattern::ArithmeticNan64, .value = {} },
        { .token = "ref.null:func", .pattern = Pattern::Exact, .value = NullFunc },
        { .token = "ref.null:extern", .pattern = Pattern::Exact, .value = NullExtern },
        { .token = "ref.func", .pattern = Pattern::AnyFunctionRef, .value = {} },
        { .token = "ref.null", .pattern = Pattern::AnyNullRef, .value = {} },
    });

    /// A value token: a prefix, then a number in a base, which make() turns into the value.
    struct NumericValue
    {
        std::string_view prefix;
        int base;
        uint64_t maxValue;
        Value (*make)(uint64_t number);
    };

    constexpr auto NumericValues = std::to_array<NumericValue>({
        { .prefix = "i32:",
          .base = 16,
          .maxValue = Max32,
          .make = [](uint64_t bits) -> Value { return runtime::I32 { static_cast<uint32_t>(bits) }; } },
        { .prefix = "i64:",
          .base = 16,
          .maxValue = Max64,
          .make = [](uint64_t bits) -> Value { return runtime::I64 { bits }; } },
        { .prefix = "f32:",
          .base = 16,
          .maxValue = Max32,
          .make = [](uint64_t bits) -> Value { return runtime::F32 { static_cast<uint32_t>(bits) }; } },
        { .prefix = "f64:",
          .base = 16,
          .maxValue = Max64,
          .make = [](uint64_t bits) -> Value { return runtime::F64 { bits }; } },
        { .prefix = "ref.extern:",
          .base = 10,
          .maxValue = Max32,
          .make = [](uint64_t address) -> Value {
              return Ref { .kind = RefKind::Extern,
                           .heap = ExternHeap,
                           .address = static_cast<uint32_t>(address) };
          } },
        { .prefix = "ref.func:",
          .base = 10,
          .maxValue = Max32,
          .make = [](uint64_t address) -> Value {
              return Ref { .kind = RefKind::Func,
                           .heap = FuncHeap,
                           .address = static_cast<uint32_t>(address) };
          } },
    });

    /// The lanes of a lane pattern token after its prefix: each its bits in hex or a NaN pattern.
    [[nodiscard]] std::expected<ExpectedValue, std::string> parseLanes(LaneFormat const& format,
                                                                       std::string_view lanes)
    {
        auto expected = pattern(Pattern::Lanes);
        expected.laneShape = format.shape;
        auto bits = runtime::V128 {};
        auto index = std::size_t { 0 };
        for (auto const lane: lanes | std::views::split(','))
        {
            if (index == format.count)
                return std::unexpected { std::format("more than {} lanes in '{}'", format.count, lanes) };
            auto const text = std::string_view { lane.begin(), lane.end() };
            if (text == "nan:canonical")
                expected.lanes.at(index) = format.canonical;
            else if (text == "nan:arithmetic")
                expected.lanes.at(index) = format.arithmetic;
            else
            {
                auto const number = parseNumber(text, 16, format.width == 32 ? Max32 : Max64);
                if (!number)
                    return std::unexpected { number.error() };
                auto const offset = index * format.width;
                bits.lanes.at(offset / 64) |= *number << (offset % 64);
            }
            ++index;
        }
        if (index != format.count)
            return std::unexpected { std::format("{} lanes in '{}', not {}", index, lanes, format.count) };
        expected.value = bits;
        return expected;
    }

    /// The digits of a `v128:` token: 32 hex digits, the most significant first.
    [[nodiscard]] std::expected<ExpectedValue, std::string> parseV128(std::string_view digits)
    {
        constexpr auto HalfDigits = std::size_t { 16 };
        if (digits.size() != 2 * HalfDigits)
            return std::unexpected { std::format(
                "a v128 needs {} hex digits, not '{}'", 2 * HalfDigits, digits) };
        return parseNumber(digits.substr(0, HalfDigits), 16, Max64).and_then([&](uint64_t high) {
            return parseNumber(digits.substr(HalfDigits), 16, Max64).transform([&](uint64_t low) {
                return exact(runtime::V128 { .lanes = { low, high } });
            });
        });
    }

    /// A value token other than `either(…)`.
    [[nodiscard]] std::expected<ExpectedValue, std::string> parseSingleValue(std::string_view token)
    {
        for (auto const& fixed: FixedValues)
            if (token == fixed.token)
                return ExpectedValue { .pattern = fixed.pattern, .value = fixed.value };
        for (auto const& format: LaneFormats)
            if (token.starts_with(format.prefix))
                return parseLanes(format, token.substr(format.prefix.size()));
        if (constexpr auto V128Prefix = std::string_view { "v128:" }; token.starts_with(V128Prefix))
            return parseV128(token.substr(V128Prefix.size()));
        for (auto const& numeric: NumericValues)
            if (token.starts_with(numeric.prefix))
                return parseNumber(token.substr(numeric.prefix.size()), numeric.base, numeric.maxValue)
                    .transform([&](uint64_t number) { return exact(numeric.make(number)); });
        return std::unexpected { std::format("malformed value '{}'", token) };
    }

    /// A value token: a single value, or `either(<value>;<value>…)` of single values.
    [[nodiscard]] std::expected<ExpectedValue, std::string> parseValue(std::string_view token)
    {
        constexpr auto EitherPrefix = std::string_view { "either(" };
        if (!token.starts_with(EitherPrefix))
            return parseSingleValue(token);
        if (!token.ends_with(')') || token.size() == EitherPrefix.size() + 1)
            return std::unexpected { std::format("malformed either '{}'", token) };
        auto expected = pattern(Pattern::Either);
        auto const inner = token.substr(EitherPrefix.size(), token.size() - EitherPrefix.size() - 1);
        for (auto const alternative: inner | std::views::split(';'))
        {
            auto value = parseSingleValue(std::string_view { alternative.begin(), alternative.end() });
            if (!value)
                return std::unexpected { value.error() };
            expected.alternatives.push_back(std::move(*value));
        }
        return expected;
    }

    [[nodiscard]] std::expected<std::optional<std::string>, std::string> parseModuleName(
        std::string_view token)
    {
        if (token == "-")
            return std::nullopt;
        return decodeHex(token).transform([](std::string name) { return std::optional { std::move(name) }; });
    }

    /// The tokens after a module command's keyword: the .wasm file.
    [[nodiscard]] LineResult parseFile(Tokens& tokens, Command& command)
    {
        auto const file = tokens.next(".wasm file");
        if (!file)
            return std::unexpected { file.error() };
        command.file = std::string { *file };
        return {};
    }

    /// The last token of a line: a message.
    [[nodiscard]] LineResult parseMessage(Tokens& tokens, Command& command)
    {
        auto message = tokens.next("message").and_then(decodeHex);
        if (!message)
            return std::unexpected { message.error() };
        command.text = std::move(*message);
        return tokens.finish();
    }

    /// An optional last token: a name, into command.name.
    [[nodiscard]] LineResult parseOptionalName(Tokens& tokens, Command& command)
    {
        if (tokens.atEnd())
            return {};
        auto name = tokens.next("module name").and_then(decodeHex);
        if (!name)
            return std::unexpected { name.error() };
        command.name = std::move(*name);
        return tokens.finish();
    }

    /// `<export> <module|-> <arg>* ->`, the part every action shares after its kind.
    [[nodiscard]] LineResult parseActionHead(Tokens& tokens, Command& command, ActionKind kind)
    {
        command.action.kind = kind;
        auto field = tokens.next("export name").and_then(decodeHex);
        if (!field)
            return std::unexpected { field.error() };
        command.action.field = std::move(*field);
        auto module = tokens.next("module name").and_then(parseModuleName);
        if (!module)
            return std::unexpected { module.error() };
        command.action.module = std::move(*module);
        while (true)
        {
            auto const token = tokens.next("'->'");
            if (!token)
                return std::unexpected { token.error() };
            if (*token == "->")
                return {};
            auto const value = parseValue(*token);
            if (!value)
                return std::unexpected { value.error() };
            if (value->pattern != Pattern::Exact)
                return std::unexpected { std::format("a pattern is no argument: '{}'", *token) };
            command.action.args.push_back(value->value);
        }
    }

    /// The values after an action's arrow, into command.expected.
    [[nodiscard]] LineResult parseExpectedValues(Tokens& tokens, Command& command)
    {
        while (!tokens.atEnd())
        {
            auto const value = tokens.next("value").and_then(parseValue);
            if (!value)
                return std::unexpected { value.error() };
            command.expected.push_back(*value);
        }
        return {};
    }

    // The grammar of each command line after its keyword.

    [[nodiscard]] LineResult parseModuleCommand(Tokens& tokens, Command& command)
    {
        return parseFile(tokens, command).and_then([&] { return parseOptionalName(tokens, command); });
    }

    /// `<name|-> <definition>`.
    [[nodiscard]] LineResult parseModuleInstanceCommand(Tokens& tokens, Command& command)
    {
        auto name = tokens.next("instance name").and_then(parseModuleName);
        if (!name)
            return std::unexpected { name.error() };
        command.name = std::move(*name);
        auto definition = tokens.next("module definition").and_then(decodeHex);
        if (!definition)
            return std::unexpected { definition.error() };
        command.definition = std::move(*definition);
        return tokens.finish();
    }

    [[nodiscard]] LineResult parseRegisterCommand(Tokens& tokens, Command& command)
    {
        auto as = tokens.next("registered name").and_then(decodeHex);
        if (!as)
            return std::unexpected { as.error() };
        command.text = std::move(*as);
        return parseOptionalName(tokens, command);
    }

    /// `<export> <module|-> <arg>* -> <result>*`: the results are parsed, and left unchecked.
    [[nodiscard]] LineResult parseInvokeCommand(Tokens& tokens, Command& command)
    {
        return parseActionHead(tokens, command, ActionKind::Invoke).and_then([&] {
            return parseExpectedValues(tokens, command);
        });
    }

    /// `invoke|get <export> <module|-> <arg>* -> <expected>*`.
    [[nodiscard]] LineResult parseAssertReturnCommand(Tokens& tokens, Command& command)
    {
        auto const kind = tokens.next("action");
        if (!kind)
            return std::unexpected { kind.error() };
        auto actionKind = ActionKind::Invoke;
        if (*kind == "get")
            actionKind = ActionKind::Get;
        else if (*kind != "invoke")
            return std::unexpected { std::format("unknown action '{}'", *kind) };
        return parseActionHead(tokens, command, actionKind).and_then([&] {
            return parseExpectedValues(tokens, command);
        });
    }

    /// The keyword `invoke`, which the assertions that take only an invocation start with.
    [[nodiscard]] LineResult expectInvoke(Tokens& tokens)
    {
        auto const kind = tokens.next("'invoke'");
        if (!kind)
            return std::unexpected { kind.error() };
        if (*kind != "invoke")
            return std::unexpected { std::format("expected 'invoke', not '{}'", *kind) };
        return {};
    }

    /// `invoke <export> <module|-> <arg>* -> <message>`.
    [[nodiscard]] LineResult parseInvokeMessageCommand(Tokens& tokens, Command& command)
    {
        return expectInvoke(tokens)
            .and_then([&] { return parseActionHead(tokens, command, ActionKind::Invoke); })
            .and_then([&] { return parseMessage(tokens, command); });
    }

    /// `invoke <export> <module|-> <arg>* ->`: an exception carries no result to expect.
    [[nodiscard]] LineResult parseAssertExceptionCommand(Tokens& tokens, Command& command)
    {
        return expectInvoke(tokens)
            .and_then([&] { return parseActionHead(tokens, command, ActionKind::Invoke); })
            .and_then([&] { return tokens.finish(); });
    }

    [[nodiscard]] LineResult parseAssertTrapCommand(Tokens& tokens, Command& command)
    {
        if (tokens.peek() == "module")
        {
            command.kind = CommandKind::AssertTrapModule;
            std::ignore = tokens.next("'module'");
            if (auto const result = parseFile(tokens, command); !result)
                return result;
            return parseMessage(tokens, command);
        }
        return parseInvokeMessageCommand(tokens, command);
    }

    [[nodiscard]] LineResult parseFileMessageCommand(Tokens& tokens, Command& command)
    {
        if (auto const result = parseFile(tokens, command); !result)
            return result;
        return parseMessage(tokens, command);
    }

    [[nodiscard]] LineResult parseSkipCommand(Tokens& tokens, Command& command)
    {
        return parseMessage(tokens, command);
    }

    /// A command keyword: the kind it introduces and the grammar of the rest of its line.
    struct Keyword
    {
        std::string_view word;
        CommandKind kind;
        LineResult (*parse)(Tokens& tokens, Command& command);
    };

    constexpr auto Keywords = std::to_array<Keyword>({
        { .word = "module", .kind = CommandKind::Module, .parse = parseModuleCommand },
        { .word = "module_definition", .kind = CommandKind::ModuleDefinition, .parse = parseModuleCommand },
        { .word = "module_instance",
          .kind = CommandKind::ModuleInstance,
          .parse = parseModuleInstanceCommand },
        { .word = "register", .kind = CommandKind::Register, .parse = parseRegisterCommand },
        { .word = "invoke", .kind = CommandKind::Invoke, .parse = parseInvokeCommand },
        { .word = "assert_return", .kind = CommandKind::AssertReturn, .parse = parseAssertReturnCommand },
        { .word = "assert_trap", .kind = CommandKind::AssertTrap, .parse = parseAssertTrapCommand },
        { .word = "assert_exhaustion",
          .kind = CommandKind::AssertExhaustion,
          .parse = parseInvokeMessageCommand },
        { .word = "assert_invalid", .kind = CommandKind::AssertInvalid, .parse = parseFileMessageCommand },
        { .word = "assert_malformed",
          .kind = CommandKind::AssertMalformed,
          .parse = parseFileMessageCommand },
        { .word = "assert_unlinkable",
          .kind = CommandKind::AssertUnlinkable,
          .parse = parseFileMessageCommand },
        { .word = "assert_uninstantiable",
          .kind = CommandKind::AssertUninstantiable,
          .parse = parseFileMessageCommand },
        { .word = "assert_exception",
          .kind = CommandKind::AssertException,
          .parse = parseAssertExceptionCommand },
        { .word = "assert_invalid_custom",
          .kind = CommandKind::AssertInvalidCustom,
          .parse = parseFileMessageCommand },
        { .word = "skip", .kind = CommandKind::Skip, .parse = parseSkipCommand },
    });

    [[nodiscard]] std::expected<Command, std::string> parseLine(std::string_view line, std::size_t number)
    {
        auto tokens = Tokens { line };
        auto const word = tokens.next("command");
        for (auto const& keyword: Keywords)
        {
            if (*word != keyword.word)
                continue;
            auto command = Command {};
            command.kind = keyword.kind;
            command.line = number;
            return keyword.parse(tokens, command).transform([&] { return std::move(command); });
        }
        return std::unexpected { std::format("unknown command '{}'", *word) };
    }
} // namespace

std::expected<std::vector<Command>, ParseError> parseIsotest(std::string_view script)
{
    auto commands = std::vector<Command> {};
    auto number = std::size_t { 0 };
    while (!script.empty())
    {
        ++number;
        auto const end = script.find('\n');
        auto line = script.substr(0, end);
        script = end == std::string_view::npos ? std::string_view {} : script.substr(end + 1);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.empty())
            continue;
        auto command = parseLine(line, number);
        if (!command)
            return std::unexpected { ParseError { .line = number, .message = std::move(command.error()) } };
        commands.push_back(std::move(*command));
    }
    return commands;
}

} // namespace isoline::spectest
