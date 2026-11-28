// SPDX-License-Identifier: Apache-2.0
#include "WastReader.hpp"

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
#include <vector>

#include "ComponentValues.hpp"
#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/text/FloatLiteral.hpp>
#include <isoline/text/Instructions.hpp>
#include <isoline/text/IntegerLiteral.hpp>
#include <isoline/text/Lexer.hpp>
#include <isoline/text/LiteralError.hpp>
#include <isoline/text/ModuleReader.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>

namespace isoline::spectest
{

namespace
{
    using runtime::Ref;
    using runtime::RefKind;
    using runtime::Value;
    using text::NodeId;
    using text::NodeKind;
    using text::NoNode;
    using text::SExpressionTree;
    using text::SourcePosition;
    using text::TextLimits;
    using text::TokenKind;
    using Pattern = ExpectedValue::Pattern;

    template <typename T>
    using Result = std::expected<T, ParseError>;

    /// The error of a text function, at its own line.
    [[nodiscard]] ParseError parseErrorOf(text::TextError const& error)
    {
        return ParseError { .line = error.position.line, .message = text::specMessage(error) };
    }

    /// Where the script uses a value: a pattern may only be expected, never passed.
    enum class ValueUse : uint8_t
    {
        Argument, ///< An invocation's argument: a value.
        Expected, ///< An assertion's expected result: a value or a pattern.
    };

    /// Positions in the script, computed incrementally: the reader asks for its modules' in source
    /// order, so the script is scanned once overall rather than once per module.
    class PositionCounter
    {
      public:
        explicit PositionCounter(std::string_view source) noexcept: _source { source } {}

        /// The position of @p offset, which must not precede the offset of the previous call.
        /// @param offset The offset of a `(`.
        /// @return Its line and column.
        [[nodiscard]] SourcePosition at(std::size_t offset) noexcept
        {
            Require(offset >= _last.offset); // The reader asks in source order.
            // Every offset asked for is a `(`'s, so no CR LF straddles the previous one.
            auto const step = text::positionAt(_source.substr(_last.offset), offset - _last.offset);
            _last = SourcePosition { .line = _last.line + step.line - 1,
                                     .column = step.line == 1 ? _last.column + step.column - 1 : step.column,
                                     .offset = offset };
            return _last;
        }

      private:
        std::string_view _source; ///< The script.
        SourcePosition _last {};  ///< The position of the previous call; the script's start at first.
    };

    /// What reading a script needs throughout: its tree, the text limits, and its modules' positions.
    struct Reader
    {
        SExpressionTree const& tree; ///< The script's tree.
        TextLimits limits;           ///< The caps on reading text.
        PositionCounter positions;   ///< Where the modules are.
    };

    /// The children of a list after its keyword, consumed front to back. Annotations are skipped: the
    /// script grammar has none of its own, and a module keeps its own in its verbatim text.
    class Cursor
    {
      public:
        Cursor(SExpressionTree const& tree, NodeId list) noexcept:
            _tree { &tree }, _list { list }, _next { skipAnnotations(tree.nodes()[list].firstChild) }
        {
            take(); // the keyword
        }

        /// Whether every child was consumed.
        [[nodiscard]] bool atEnd() const noexcept { return _next == NoNode; }

        /// The next child, without consuming it; NoNode at the end.
        [[nodiscard]] NodeId peek() const noexcept { return _next; }

        /// Consumes the next child.
        /// @return It, or NoNode at the end.
        NodeId take() noexcept
        {
            auto const node = _next;
            if (node != NoNode)
                _next = skipAnnotations(_tree->nodes()[node].nextSibling);
            return node;
        }

        /// The list the cursor reads.
        [[nodiscard]] NodeId list() const noexcept { return _list; }

      private:
        [[nodiscard]] NodeId skipAnnotations(NodeId node) const noexcept
        {
            while (node != NoNode && _tree->nodes()[node].kind == NodeKind::Annotation)
                node = _tree->nodes()[node].nextSibling;
            return node;
        }

        SExpressionTree const* _tree; ///< The tree.
        NodeId _list;                 ///< The list.
        NodeId _next;                 ///< Its next child, or NoNode.
    };

    /// An error at @p node's line.
    [[nodiscard]] ParseError errorAt(SExpressionTree const& tree, NodeId node, std::string message)
    {
        return ParseError { .line = tree.positionOf(node).line, .message = std::move(message) };
    }

    /// A node's text for a message: an atom's, or `(…)` for a list.
    [[nodiscard]] std::string_view describe(SExpressionTree const& tree, NodeId node)
    {
        return tree.nodes()[node].kind == NodeKind::Atom ? tree.textOf(node) : std::string_view { "(…)" };
    }

    /// The error of a list that ends early, or that holds something it may not hold where the cursor is.
    [[nodiscard]] ParseError unexpected(SExpressionTree const& tree,
                                        Cursor const& cursor,
                                        std::string_view expected)
    {
        auto const keyword = text::headKeyword(tree, cursor.list());
        if (cursor.atEnd())
            return errorAt(tree, cursor.list(), std::format("expected {} in '{}'", expected, keyword));
        return errorAt(
            tree,
            cursor.peek(),
            std::format("expected {} in '{}', got '{}'", expected, keyword, describe(tree, cursor.peek())));
    }

    /// Requires a list to hold nothing more.
    [[nodiscard]] Result<void> finish(SExpressionTree const& tree, Cursor const& cursor)
    {
        if (!cursor.atEnd())
            return std::unexpected { unexpected(tree, cursor, "')'") };
        return {};
    }

    [[nodiscard]] bool isAtom(SExpressionTree const& tree, NodeId node, TokenKind kind) noexcept
    {
        return node != NoNode && tree.nodes()[node].kind == NodeKind::Atom
               && tree.nodes()[node].tokenKind == kind;
    }

    [[nodiscard]] bool isKeyword(SExpressionTree const& tree, NodeId node, std::string_view keyword) noexcept
    {
        return isAtom(tree, node, TokenKind::Keyword) && tree.textOf(node) == keyword;
    }

    /// Whether a node is a list that starts with @p keyword.
    [[nodiscard]] bool isListOf(SExpressionTree const& tree, NodeId node, std::string_view keyword) noexcept
    {
        return node != NoNode && tree.nodes()[node].kind == NodeKind::List
               && text::headKeyword(tree, node) == keyword;
    }

    /// Decodes an atom's token: a string's bytes or name, or an identifier's name.
    using DecodeAtom = std::expected<std::string, text::TextError> (*)(std::string_view, text::Token);

    /// What a script names with an atom: its token's kind, and what its text denotes.
    struct AtomForm
    {
        TokenKind kind;        ///< The token it must be.
        std::string_view what; ///< What it is, for a message.
        DecodeAtom decode;     ///< What it denotes.
    };

    constexpr auto StringForm =
        AtomForm { .kind = TokenKind::String, .what = "a string", .decode = &text::decodeString };
    constexpr auto NameForm =
        AtomForm { .kind = TokenKind::String, .what = "a name", .decode = &text::decodeName };
    constexpr auto IdentifierForm =
        AtomForm { .kind = TokenKind::Identifier, .what = "an identifier", .decode = &text::identifierName };

    /// Consumes an atom of @p form.
    [[nodiscard]] Result<std::string> take(SExpressionTree const& tree, Cursor& cursor, AtomForm const& form)
    {
        if (!isAtom(tree, cursor.peek(), form.kind))
            return std::unexpected { unexpected(tree, cursor, form.what) };
        auto const& atom = tree.nodes()[cursor.take()];
        return form
            .decode(tree.source(),
                    text::Token { .kind = atom.tokenKind, .offset = atom.offset, .length = atom.length })
            .transform_error(&parseErrorOf);
    }

    /// An optional `$id`, as a module, an action or a registration names one.
    [[nodiscard]] Result<std::optional<std::string>> takeOptionalIdentifier(SExpressionTree const& tree,
                                                                            Cursor& cursor)
    {
        if (!isAtom(tree, cursor.peek(), TokenKind::Identifier))
            return std::nullopt;
        return take(tree, cursor, IdentifierForm).transform([](std::string name) {
            return std::optional { std::move(name) };
        });
    }

    /// The message an assertion ends with, into command.text.
    [[nodiscard]] Result<void> takeMessage(SExpressionTree const& tree, Cursor& cursor, Command& command)
    {
        return take(tree, cursor, StringForm).and_then([&](std::string message) {
            command.text = std::move(message);
            return finish(tree, cursor);
        });
    }

    // Values.

    [[nodiscard]] ExpectedValue exact(Value value) noexcept
    {
        return ExpectedValue { .pattern = Pattern::Exact, .value = value };
    }

    /// A pattern, where @p use admits one.
    [[nodiscard]] Result<ExpectedValue> patternFor(SExpressionTree const& tree,
                                                   NodeId list,
                                                   Pattern pattern,
                                                   ValueUse use)
    {
        if (use == ValueUse::Argument)
            return std::unexpected { errorAt(
                tree, list, std::format("a pattern is no argument: '{}'", tree.sourceOf(list))) };
        return ExpectedValue { .pattern = pattern, .value = {} };
    }

    /// A literal's bits: @p parse of its text, a literal error reported as the text format reports it.
    template <typename Parse>
    [[nodiscard]] Result<uint64_t> literal(SExpressionTree const& tree, Cursor& cursor, Parse parse)
    {
        if (!isAtom(tree, cursor.peek(), TokenKind::Keyword))
            return std::unexpected { unexpected(tree, cursor, "a number") };
        auto const node = cursor.take();
        return parse(tree.textOf(node)).transform_error([&](text::LiteralError error) {
            auto const code = text::textErrorCodeOf(error);
            return errorAt(tree,
                           node,
                           std::format("{} '{}'",
                                       text::TextErrorTable[static_cast<std::size_t>(code)].specMessage,
                                       tree.textOf(node)));
        });
    }

    [[nodiscard]] Result<uint64_t> integerLiteral(Reader const& reader,
                                                  Cursor& cursor,
                                                  text::IntegerWidth width)
    {
        return literal(reader.tree, cursor, [&](std::string_view text) {
            return text::parseInteger(text, width, text::IntegerSyntax::Either, reader.limits);
        });
    }

    /// A float literal of @p format's lane width.
    [[nodiscard]] Result<uint64_t> floatLiteral(Reader const& reader,
                                                Cursor& cursor,
                                                LaneFormat const& format)
    {
        return literal(
            reader.tree, cursor, [&](std::string_view text) -> std::expected<uint64_t, text::LiteralError> {
                if (format.width == 32)
                    return text::parseF32(text, reader.limits).transform([](uint32_t bits) {
                        return uint64_t { bits };
                    });
                return text::parseF64(text, reader.limits);
            });
    }

    /// A NaN pattern a float, or a float lane, may be expected to match: its token, and which of a lane
    /// format's patterns it names.
    struct NanToken
    {
        std::string_view token;        ///< The pattern's token.
        Pattern LaneFormat::* pattern; ///< Its pattern at a lane format's width.
    };

    constexpr auto NanTokens = std::to_array<NanToken>({
        { .token = "nan:canonical", .pattern = &LaneFormat::canonical },
        { .token = "nan:arithmetic", .pattern = &LaneFormat::arithmetic },
    });

    /// The NaN pattern the cursor's next token names in a float of @p format's width, consumed; nullopt
    /// when it names none.
    [[nodiscard]] std::optional<Pattern> takeNanPattern(SExpressionTree const& tree,
                                                        Cursor& cursor,
                                                        LaneFormat const& format)
    {
        if (!isAtom(tree, cursor.peek(), TokenKind::Keyword))
            return std::nullopt;
        auto const nan = std::ranges::find(NanTokens, tree.textOf(cursor.peek()), &NanToken::token);
        if (nan == NanTokens.end())
            return std::nullopt;
        cursor.take();
        return format.*(nan->pattern);
    }

    /// The value after a list's keyword, its list then finished.
    [[nodiscard]] Result<ExpectedValue> finished(SExpressionTree const& tree,
                                                 Cursor const& cursor,
                                                 Result<ExpectedValue> value)
    {
        return std::move(value).and_then([&](ExpectedValue expected) {
            return finish(tree, cursor).transform([&] { return std::move(expected); });
        });
    }

    /// `(i32.const n)` and `(i64.const n)`.
    template <typename Number, text::IntegerWidth Width>
    [[nodiscard]] Result<ExpectedValue> readInteger(Reader const& reader, Cursor& cursor, ValueUse /*use*/)
    {
        auto const bits = integerLiteral(reader, cursor, Width).transform([](uint64_t value) {
            return exact(Number { static_cast<decltype(Number::bits)>(value) });
        });
        return finished(reader.tree, cursor, bits);
    }

    /// `(f32.const z)` and `(f64.const z)`, and their NaN patterns.
    template <typename Number, LaneShape Shape>
    [[nodiscard]] Result<ExpectedValue> readFloat(Reader const& reader, Cursor& cursor, ValueUse use)
    {
        auto const& format = laneFormat(Shape);
        if (auto const nan = takeNanPattern(reader.tree, cursor, format))
            return finished(reader.tree, cursor, patternFor(reader.tree, cursor.list(), *nan, use));
        auto const bits = floatLiteral(reader, cursor, format).transform([](uint64_t value) {
            return exact(Number { static_cast<decltype(Number::bits)>(value) });
        });
        return finished(reader.tree, cursor, bits);
    }

    /// `(v128.const <shape> <lane>…)`: exactly its bits, or, where a float lane is a NaN pattern, a
    /// pattern of lanes, whose value holds the bits of the exact lanes.
    [[nodiscard]] Result<ExpectedValue> readVector(Reader const& reader, Cursor& cursor, ValueUse use)
    {
        auto const& tree = reader.tree;
        auto const shape =
            isAtom(tree, cursor.peek(), TokenKind::Keyword)
                ? std::ranges::find(text::LaneShapes, tree.textOf(cursor.peek()), &text::LaneShape::keyword)
                : text::LaneShapes.end();
        if (shape == text::LaneShapes.end())
            return std::unexpected { unexpected(tree, cursor, "a v128 shape") };
        cursor.take();
        auto const width = std::size_t { shape->laneBytes } * 8U;
        auto const format = shape->number == text::LaneNumber::Float
                                ? std::optional { *std::ranges::find(LaneFormats, width, &LaneFormat::width) }
                                : std::nullopt;
        auto expected = exact(runtime::V128 {});
        auto bits = runtime::V128 {};
        auto lane = std::size_t { 0 };
        while (!cursor.atEnd())
        {
            if (lane == shape->laneCount())
                return std::unexpected { errorAt(tree, cursor.list(), "wrong number of lane literals") };
            auto const nan = format ? takeNanPattern(tree, cursor, *format) : std::nullopt;
            if (nan)
            {
                if (use == ValueUse::Argument)
                    return patternFor(tree, cursor.list(), *nan, use);
                expected.pattern = Pattern::Lanes;
                expected.laneShape = format->shape;
                expected.lanes.at(lane) = *nan;
                ++lane;
                continue;
            }
            auto const value = format
                                   ? floatLiteral(reader, cursor, *format)
                                   : integerLiteral(reader, cursor, static_cast<text::IntegerWidth>(width));
            if (!value)
                return std::unexpected { value.error() };
            auto const offset = lane * width;
            bits.lanes.at(offset / 64) |= *value << (offset % 64);
            ++lane;
        }
        if (lane != shape->laneCount())
            return std::unexpected { errorAt(tree, cursor.list(), "wrong number of lane literals") };
        expected.value = bits;
        return expected;
    }

    /// A heap type `ref.null` may name: its keyword and the heap of the null.
    struct NullHeap
    {
        std::string_view keyword;      ///< The heap type's keyword.
        binary::AbstractHeapType heap; ///< The heap of its null.
    };

    /// The heaps whose null a script value may be; the GC proposal's heaps are not Isoline's.
    constexpr auto NullHeaps = std::to_array<NullHeap>({
        { .keyword = "func", .heap = binary::AbstractHeapType::Func },
        { .keyword = "nofunc", .heap = binary::AbstractHeapType::NoFunc },
        { .keyword = "extern", .heap = binary::AbstractHeapType::Extern },
        { .keyword = "noextern", .heap = binary::AbstractHeapType::NoExtern },
        { .keyword = "exn", .heap = binary::AbstractHeapType::Exn },
        { .keyword = "noexn", .heap = binary::AbstractHeapType::NoExn },
    });

    /// `(ref.null heap)`, and `(ref.null)`, the null of any heap.
    [[nodiscard]] Result<ExpectedValue> readNull(Reader const& reader, Cursor& cursor, ValueUse use)
    {
        auto const& tree = reader.tree;
        if (cursor.atEnd())
            return patternFor(tree, cursor.list(), Pattern::AnyNullRef, use);
        auto const heap = isAtom(tree, cursor.peek(), TokenKind::Keyword)
                              ? std::ranges::find(NullHeaps, tree.textOf(cursor.peek()), &NullHeap::keyword)
                              : NullHeaps.end();
        if (heap == NullHeaps.end())
            return std::unexpected { unexpected(tree, cursor, "a heap type of func, extern or exn") };
        cursor.take();
        auto const null = Ref { .kind = RefKind::Null, .heap = { .kind = heap->heap }, .address = 0 };
        return finished(tree, cursor, exact(null));
    }

    /// `(ref.<kind> N)`, the reference of address N; or `(ref.<kind>)`, any non-null one.
    template <RefKind Kind, binary::AbstractHeapType Heap, Pattern Any>
    [[nodiscard]] Result<ExpectedValue> readReference(Reader const& reader, Cursor& cursor, ValueUse use)
    {
        if (cursor.atEnd())
            return patternFor(reader.tree, cursor.list(), Any, use);
        auto const address = literal(reader.tree, cursor, [&](std::string_view text) {
            return text::parseInteger(
                text, text::IntegerWidth::I32, text::IntegerSyntax::Unsigned, reader.limits);
        });
        return finished(reader.tree, cursor, address.transform([](uint64_t value) {
            return exact(
                Ref { .kind = Kind, .heap = { .kind = Heap }, .address = static_cast<uint32_t>(value) });
        }));
    }

    /// `(ref.exn)`, any non-null exception reference: there is no exception to name by address.
    [[nodiscard]] Result<ExpectedValue> readAnyException(Reader const& reader, Cursor& cursor, ValueUse use)
    {
        return finished(
            reader.tree, cursor, patternFor(reader.tree, cursor.list(), Pattern::AnyExceptionRef, use));
    }

    [[nodiscard]] Result<ExpectedValue> readEither(Reader const& reader, Cursor& cursor, ValueUse use);

    /// What reads the rest of a value's list, after its keyword.
    using ValueReader = Result<ExpectedValue> (*)(Reader const&, Cursor&, ValueUse);

    /// One form of value: its list's keyword, and what reads the rest of it.
    struct ValueForm
    {
        std::string_view keyword; ///< The list's keyword.
        ValueReader read;         ///< What reads the rest of it.
    };

    /// Every form of value a script may give. Adding one is adding a row.
    constexpr auto ValueForms = std::to_array<ValueForm>({
        { .keyword = "i32.const", .read = &readInteger<runtime::I32, text::IntegerWidth::I32> },
        { .keyword = "i64.const", .read = &readInteger<runtime::I64, text::IntegerWidth::I64> },
        { .keyword = "f32.const", .read = &readFloat<runtime::F32, LaneShape::F32x4> },
        { .keyword = "f64.const", .read = &readFloat<runtime::F64, LaneShape::F64x2> },
        { .keyword = "v128.const", .read = &readVector },
        { .keyword = "ref.null", .read = &readNull },
        { .keyword = "ref.extern",
          .read = &readReference<RefKind::Extern, binary::AbstractHeapType::Extern, Pattern::AnyExternRef> },
        { .keyword = "ref.func",
          .read = &readReference<RefKind::Func, binary::AbstractHeapType::Func, Pattern::AnyFunctionRef> },
        { .keyword = "ref.exn", .read = &readAnyException },
        { .keyword = "either", .read = &readEither },
    });

    /// A value: a list whose keyword is a ValueForms row.
    [[nodiscard]] Result<ExpectedValue> readValue(Reader const& reader, Cursor& cursor, ValueUse use)
    {
        auto const& tree = reader.tree;
        auto const node = cursor.peek();
        auto const keyword = node != NoNode && tree.nodes()[node].kind == NodeKind::List
                                 ? text::headKeyword(tree, node)
                                 : std::string_view {};
        auto const form = std::ranges::find(ValueForms, keyword, &ValueForm::keyword);
        if (keyword.empty() || form == ValueForms.end())
            return std::unexpected { unexpected(tree, cursor, "a value") };
        auto inner = Cursor { tree, cursor.take() };
        return form->read(reader, inner, use);
    }

    /// `(either <value> <value>…)`: any one of the values, none of which is an either. Its alternatives
    /// are read by readValue, which an either inside is refused before, so the reading never nests deeper.
    [[nodiscard]] Result<ExpectedValue> readEither(Reader const& reader, Cursor& cursor, ValueUse use)
    {
        auto expected = patternFor(reader.tree, cursor.list(), Pattern::Either, use);
        if (!expected)
            return expected;
        while (!cursor.atEnd())
        {
            if (isListOf(reader.tree, cursor.peek(), "either"))
                return std::unexpected { errorAt(reader.tree, cursor.peek(), "an either within an either") };
            auto alternative = readValue(reader, cursor, use);
            if (!alternative)
                return alternative;
            expected->alternatives.push_back(std::move(*alternative));
        }
        if (expected->alternatives.empty())
            return std::unexpected { unexpected(reader.tree, cursor, "a value") };
        return expected;
    }

    /// One value as each layer reads it: a core value where it is one of ValueForms, a component value
    /// where it is one of ComponentValueForms; `(f32.const …)` and `(f64.const …)` read as both.
    struct LayeredValue
    {
        std::optional<ExpectedValue> core {};         ///< As a core value, if it reads as one.
        std::optional<ComponentLiteral> component {}; ///< As a component value, if it reads as one.
    };

    /// Reads one value as every layer that reads it, consumed from @p cursor. A value one layer reads and
    /// another refuses (`(f32.const nan:canonical)`, no component value) is that layer's alone.
    [[nodiscard]] Result<LayeredValue> readLayeredValue(Reader const& reader, Cursor& cursor, ValueUse use)
    {
        auto const& tree = reader.tree;
        auto const node = cursor.peek();
        auto const keyword = node != NoNode && tree.nodes()[node].kind == NodeKind::List
                                 ? text::headKeyword(tree, node)
                                 : std::string_view {};
        auto const isCore = std::ranges::find(ValueForms, keyword, &ValueForm::keyword) != ValueForms.end();
        if (!isComponentValueKeyword(keyword) || keyword.empty())
            return readValue(reader, cursor, use).transform([](ExpectedValue value) {
                return LayeredValue { .core = std::move(value), .component = std::nullopt };
            });
        auto component = readComponentLiteral(tree, node, reader.limits);
        auto layered = LayeredValue {};
        if (isCore)
        {
            auto core = readValue(reader, cursor, use);
            if (!core && !component)
                return std::unexpected { core.error() };
            if (core)
                layered.core = std::move(*core);
        }
        else
        {
            cursor.take();
            if (!component)
                return std::unexpected { component.error() };
        }
        if (component)
            layered.component = std::move(*component);
        return layered;
    }

    // Actions.

    /// The action `(invoke [$module] "name" <arg>…)` or `(get [$module] "name")`, into command.action and
    /// command.componentArgs.
    /// @param list An `invoke` or `get` list.
    [[nodiscard]] Result<void> readAction(Reader const& reader, NodeId list, Command& command)
    {
        auto const& tree = reader.tree;
        auto action = Action {};
        action.kind = text::headKeyword(tree, list) == "get" ? ActionKind::Get : ActionKind::Invoke;
        auto cursor = Cursor { tree, list };
        auto module = takeOptionalIdentifier(tree, cursor);
        if (!module)
            return std::unexpected { module.error() };
        action.module = std::move(*module);
        auto field = take(tree, cursor, NameForm);
        if (!field)
            return std::unexpected { field.error() };
        action.field = std::move(*field);
        while (action.kind == ActionKind::Invoke && !cursor.atEnd())
        {
            auto argument = readLayeredValue(reader, cursor, ValueUse::Argument);
            if (!argument)
                return std::unexpected { argument.error() };
            ++action.arity;
            if (argument->core)
                action.args.push_back(argument->core->value);
            if (argument->component)
                command.componentArgs.push_back(std::move(*argument->component));
        }
        return finish(tree, cursor).transform([&] { command.action = std::move(action); });
    }

    /// The action an assertion is about, consumed from @p cursor into command.action.
    [[nodiscard]] Result<void> takeAction(Reader const& reader, Cursor& cursor, Command& command)
    {
        auto const node = cursor.peek();
        if (!isListOf(reader.tree, node, "invoke") && !isListOf(reader.tree, node, "get"))
            return std::unexpected { unexpected(reader.tree, cursor, "an action") };
        return readAction(reader, cursor.take(), command);
    }

    // Modules.

    /// How a module given by its strings is encoded: its keyword after `module`, and its form.
    struct EncodedForm
    {
        std::string_view keyword; ///< `binary` or `quote`.
        ModuleForm form;          ///< What its strings are.
    };

    /// The forms a layer's list gives its module or component in: as text, or by its strings.
    struct LayerForms
    {
        std::string_view keyword;           ///< `module` or `component`.
        ModuleForm text;                    ///< The form of its text.
        std::array<EncodedForm, 2> encoded; ///< The forms of its strings, after `binary` or `quote`.
    };

    /// The lists that give a module or a component. Adding a layer is adding a row.
    constexpr auto Layers = std::to_array<LayerForms>({
        { .keyword = "module",
          .text = ModuleForm::Text,
          .encoded = { { { .keyword = "binary", .form = ModuleForm::Binary },
                         { .keyword = "quote", .form = ModuleForm::Quote } } } },
        { .keyword = "component",
          .text = ModuleForm::ComponentText,
          .encoded = { { { .keyword = "binary", .form = ModuleForm::ComponentBinary },
                         { .keyword = "quote", .form = ModuleForm::ComponentQuote } } } },
    });

    /// The layer @p node gives, if it is a `(module …)` or a `(component …)`.
    [[nodiscard]] LayerForms const* layerOf(SExpressionTree const& tree, NodeId node) noexcept
    {
        auto const layer = std::ranges::find_if(
            Layers, [&](LayerForms const& row) { return isListOf(tree, node, row.keyword); });
        return layer == Layers.end() ? nullptr : &*layer;
    }

    /// How a `(module …)` is written, which decides the command it makes.
    enum class ModuleKeyword : uint8_t
    {
        None,       ///< `(module …)`: a module, instantiated.
        Definition, ///< `(module definition …)`: a module, not instantiated.
        Instance,   ///< `(module instance …)`: an instance of a definition.
    };

    /// What a `(module …)` says.
    struct ModuleList
    {
        ModuleKeyword keyword = ModuleKeyword::None; ///< How it is written.
        std::optional<std::string> name;             ///< A module's `$name`, or an instance's.
        std::string definition;                      ///< An instance's definition.
        ModuleSource source;                         ///< A module's source.
    };

    /// The ids of `(module instance [$instance] [$definition])`: two name both, one names the definition.
    [[nodiscard]] Result<void> readInstance(SExpressionTree const& tree, Cursor& cursor, ModuleList& module)
    {
        auto first = takeOptionalIdentifier(tree, cursor);
        if (!first)
            return std::unexpected { first.error() };
        auto second = takeOptionalIdentifier(tree, cursor);
        if (!second)
            return std::unexpected { second.error() };
        if (!*first)
            return std::unexpected { errorAt(
                tree, cursor.list(), "a module instance must name its definition") };
        if (*second)
        {
            module.name = std::move(*first);
            module.definition = std::move(**second);
        }
        else
            module.definition = std::move(**first);
        return finish(tree, cursor);
    }

    /// A module's strings after `binary` or `quote`, concatenated (the reference's `string_list`).
    [[nodiscard]] Result<std::string> takeStrings(SExpressionTree const& tree, Cursor& cursor)
    {
        auto concatenated = std::string {};
        while (!cursor.atEnd())
        {
            auto const string = take(tree, cursor, StringForm);
            if (!string)
                return std::unexpected { string.error() };
            concatenated += *string;
        }
        return concatenated;
    }

    /// A `(module …)` or `(component …)` list.
    [[nodiscard]] Result<ModuleList> readModule(Reader& reader, NodeId list)
    {
        auto const& tree = reader.tree;
        auto const* const layer = layerOf(tree, list);
        Require(layer != nullptr);
        auto const& encodedForms = layer->encoded;
        auto module = ModuleList {};
        auto cursor = Cursor { tree, list };
        if (isKeyword(tree, cursor.peek(), "instance"))
        {
            cursor.take();
            module.keyword = ModuleKeyword::Instance;
            return readInstance(tree, cursor, module).transform([&] { return std::move(module); });
        }
        if (isKeyword(tree, cursor.peek(), "definition"))
        {
            cursor.take();
            module.keyword = ModuleKeyword::Definition;
        }
        auto name = takeOptionalIdentifier(tree, cursor);
        if (!name)
            return std::unexpected { name.error() };
        module.name = std::move(*name);
        module.source.position = reader.positions.at(tree.nodes()[list].offset);
        auto const encoded =
            isAtom(tree, cursor.peek(), TokenKind::Keyword)
                ? std::ranges::find(encodedForms, tree.textOf(cursor.peek()), &EncodedForm::keyword)
                : encodedForms.end();
        if (encoded == encodedForms.end())
        {
            // A module given as text keeps its verbatim source, `$name` and all, which compileText reads
            // as the reference interpreter does.
            module.source.form = layer->text;
            module.source.text = std::string { tree.sourceOf(list) };
            return module;
        }
        cursor.take();
        module.source.form = encoded->form;
        return takeStrings(tree, cursor).transform([&](std::string strings) {
            module.source.text = std::move(strings);
            return std::move(module);
        });
    }

    /// The module an assertion is about, consumed from @p cursor: a module or a component of any form, not
    /// an instance.
    [[nodiscard]] Result<void> takeAssertedModule(Reader& reader, Cursor& cursor, Command& command)
    {
        auto const node = cursor.peek();
        if (layerOf(reader.tree, node) == nullptr)
            return std::unexpected { unexpected(reader.tree, cursor, "a module") };
        auto module = readModule(reader, cursor.take());
        if (!module)
            return std::unexpected { module.error() };
        if (module->keyword == ModuleKeyword::Instance)
            return std::unexpected { errorAt(
                reader.tree, node, "an assertion on a module instance is not supported") };
        command.source = std::move(module->source);
        return {};
    }

    // Commands, each read from the cursor after its keyword into a command of its row's kind.

    /// `(module …)` and `(component …)`: a module or component, a definition, or an instance.
    [[nodiscard]] Result<void> readModuleCommand(Reader& reader, Cursor& cursor, Command& command)
    {
        auto module = readModule(reader, cursor.list());
        if (!module)
            return std::unexpected { module.error() };
        constexpr auto Kinds = std::to_array<CommandKind>({
            CommandKind::Module,           // ModuleKeyword::None
            CommandKind::ModuleDefinition, // ModuleKeyword::Definition
            CommandKind::ModuleInstance,   // ModuleKeyword::Instance
        });
        command.kind = Kinds.at(static_cast<std::size_t>(module->keyword));
        command.name = std::move(module->name);
        command.definition = std::move(module->definition);
        command.source = std::move(module->source);
        return {};
    }

    /// `(register "as" [$module])`.
    [[nodiscard]] Result<void> readRegister(Reader& reader, Cursor& cursor, Command& command)
    {
        auto as = take(reader.tree, cursor, NameForm);
        if (!as)
            return std::unexpected { as.error() };
        command.text = std::move(*as);
        auto name = takeOptionalIdentifier(reader.tree, cursor);
        if (!name)
            return std::unexpected { name.error() };
        command.name = std::move(*name);
        return finish(reader.tree, cursor);
    }

    /// `(invoke …)` and `(get …)` as commands: the command's list is the action.
    [[nodiscard]] Result<void> readActionCommand(Reader& reader, Cursor& cursor, Command& command)
    {
        return readAction(reader, cursor.list(), command);
    }

    /// `(assert_return <action> <expected>…)`.
    [[nodiscard]] Result<void> readAssertReturn(Reader& reader, Cursor& cursor, Command& command)
    {
        if (auto const action = takeAction(reader, cursor, command); !action)
            return action;
        while (!cursor.atEnd())
        {
            auto expected = readLayeredValue(reader, cursor, ValueUse::Expected);
            if (!expected)
                return std::unexpected { expected.error() };
            ++command.expectedCount;
            if (expected->core)
                command.expected.push_back(std::move(*expected->core));
            if (expected->component)
                command.componentExpected.push_back(std::move(*expected->component));
        }
        return {};
    }

    /// `(assert_trap <action> "message")`, `(assert_exhaustion …)` likewise.
    [[nodiscard]] Result<void> readActionMessage(Reader& reader, Cursor& cursor, Command& command)
    {
        if (auto const action = takeAction(reader, cursor, command); !action)
            return action;
        return takeMessage(reader.tree, cursor, command);
    }

    /// `(assert_trap <action> "message")`, or `(assert_trap <module> "message")`: instantiating the
    /// module must trap.
    [[nodiscard]] Result<void> readAssertTrap(Reader& reader, Cursor& cursor, Command& command)
    {
        if (layerOf(reader.tree, cursor.peek()) == nullptr)
            return readActionMessage(reader, cursor, command);
        command.kind = CommandKind::AssertTrapModule;
        return takeAssertedModule(reader, cursor, command).and_then([&] {
            return takeMessage(reader.tree, cursor, command);
        });
    }

    /// `(assert_exception <action>)`: an exception carries no message to expect.
    [[nodiscard]] Result<void> readAssertException(Reader& reader, Cursor& cursor, Command& command)
    {
        if (auto const action = takeAction(reader, cursor, command); !action)
            return action;
        return finish(reader.tree, cursor);
    }

    /// `(assert_invalid <module> "message")` and the other assertions on a module.
    [[nodiscard]] Result<void> readModuleMessage(Reader& reader, Cursor& cursor, Command& command)
    {
        return takeAssertedModule(reader, cursor, command).and_then([&] {
            return takeMessage(reader.tree, cursor, command);
        });
    }

    /// What reads the rest of a command's list, after its keyword.
    using CommandReader = Result<void> (*)(Reader&, Cursor&, Command&);

    /// One kind of command: its keyword, the kind it makes, and what reads the rest of its list. A
    /// reader may refine the kind (a module definition, an assert_trap of a module).
    struct ScriptCommand
    {
        std::string_view keyword; ///< The command list's keyword.
        CommandKind kind;         ///< The command it makes.
        CommandReader read;       ///< What reads the rest of it.
    };

    /// Every command a script may hold. Adding one is adding a row.
    constexpr auto ScriptCommands = std::to_array<ScriptCommand>({
        { .keyword = "module", .kind = CommandKind::Module, .read = &readModuleCommand },
        { .keyword = "component", .kind = CommandKind::Module, .read = &readModuleCommand },
        { .keyword = "register", .kind = CommandKind::Register, .read = &readRegister },
        { .keyword = "invoke", .kind = CommandKind::Invoke, .read = &readActionCommand },
        { .keyword = "get", .kind = CommandKind::Invoke, .read = &readActionCommand },
        { .keyword = "assert_return", .kind = CommandKind::AssertReturn, .read = &readAssertReturn },
        { .keyword = "assert_trap", .kind = CommandKind::AssertTrap, .read = &readAssertTrap },
        { .keyword = "assert_exhaustion", .kind = CommandKind::AssertExhaustion, .read = &readActionMessage },
        { .keyword = "assert_exception", .kind = CommandKind::AssertException, .read = &readAssertException },
        { .keyword = "assert_invalid", .kind = CommandKind::AssertInvalid, .read = &readModuleMessage },
        { .keyword = "assert_malformed", .kind = CommandKind::AssertMalformed, .read = &readModuleMessage },
        { .keyword = "assert_unlinkable", .kind = CommandKind::AssertUnlinkable, .read = &readModuleMessage },
        { .keyword = "assert_invalid_custom",
          .kind = CommandKind::AssertInvalidCustom,
          .read = &readModuleMessage },
        { .keyword = "assert_malformed_custom",
          .kind = CommandKind::AssertMalformedCustom,
          .read = &readModuleMessage },
    });
} // namespace

std::expected<std::vector<Command>, ParseError> parseWast(std::string_view script, TextLimits limits)
{
    auto const tree = text::parseSExpressions(script, limits);
    if (!tree)
        return std::unexpected { parseErrorOf(tree.error()) };
    auto const keywordOf = [&](NodeId node) {
        return tree->nodes()[node].kind == NodeKind::List ? text::headKeyword(*tree, node)
                                                          : std::string_view {};
    };
    auto const rowOf = [&](std::string_view keyword) {
        return std::ranges::find(ScriptCommands, keyword, &ScriptCommand::keyword);
    };
    // A script whose first list is no command is an inline module (the reference's `inline_module1`
    // sugar): one module, the whole script its text, which compileText reads as bare fields.
    if (auto const first = text::firstTopLevelNode(*tree);
        first != NoNode && !keywordOf(first).empty() && rowOf(keywordOf(first)) == ScriptCommands.end())
    {
        auto module = Command {};
        module.kind = CommandKind::Module;
        module.line = tree->positionOf(first).line;
        module.source =
            ModuleSource { .form = ModuleForm::Text, .text = std::string { script }, .position = {} };
        return std::vector { std::move(module) };
    }
    auto reader = Reader { .tree = *tree, .limits = limits, .positions = PositionCounter { script } };
    auto commands = std::vector<Command> {};
    for (auto const node: tree->topLevel())
    {
        if (tree->nodes()[node].kind == NodeKind::Annotation)
            continue;
        auto const keyword = keywordOf(node);
        auto const row = rowOf(keyword);
        if (keyword.empty() || row == ScriptCommands.end())
            return std::unexpected { errorAt(
                *tree, node, std::format("unknown command '{}'", describe(*tree, node))) };
        auto command = Command {};
        command.kind = row->kind;
        command.line = reader.positions.at(tree->nodes()[node].offset).line;
        auto cursor = Cursor { *tree, node };
        if (auto const read = row->read(reader, cursor, command); !read)
            return std::unexpected { read.error() };
        commands.push_back(std::move(command));
    }
    return commands;
}

} // namespace isoline::spectest
