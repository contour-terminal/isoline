// SPDX-License-Identifier: Apache-2.0
#include "ComponentValues.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/canonical/Unicode.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/text/FloatLiteral.hpp>
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
    using component::types::TypeArena;
    using component::types::TypeId;
    using component::types::ValueKind;
    using text::NodeId;
    using text::NodeKind;
    using text::SExpressionTree;
    using text::TokenKind;

    template <typename T>
    using Result = std::expected<T, ParseError>;

    [[nodiscard]] ParseError errorAt(SExpressionTree const& tree, NodeId node, std::string message)
    {
        return ParseError { .line = tree.positionOf(node).line, .message = std::move(message) };
    }

    /// A value's list, and where its keyword is among the list's children: first, or, for a record
    /// field's value, after the field's keyword and label (wast 263 reads `(field "label" u32.const 7)`).
    struct ValueList
    {
        NodeId list = text::NoNode; ///< The list.
        std::size_t keyword = 0;    ///< The index of the value's keyword among its children.
    };

    /// The children of @p list, annotations left out.
    [[nodiscard]] std::vector<NodeId> childrenOf(SExpressionTree const& tree, NodeId list)
    {
        auto children = std::vector<NodeId> {};
        for (auto const node: tree.children(list))
            if (tree.nodes()[node].kind != NodeKind::Annotation)
                children.push_back(node);
        return children;
    }

    [[nodiscard]] bool isAtomOf(SExpressionTree const& tree, NodeId node, TokenKind kind) noexcept
    {
        return tree.nodes()[node].kind == NodeKind::Atom && tree.nodes()[node].tokenKind == kind;
    }

    /// The keyword of @p value, or empty when it has none where it should.
    [[nodiscard]] std::string_view keywordOf(SExpressionTree const& tree, ValueList value)
    {
        if (tree.nodes()[value.list].kind != NodeKind::List)
            return {};
        auto const children = childrenOf(tree, value.list);
        if (value.keyword >= children.size() || !isAtomOf(tree, children[value.keyword], TokenKind::Keyword))
            return {};
        return tree.textOf(children[value.keyword]);
    }

    /// The children of @p value after its keyword.
    [[nodiscard]] std::vector<NodeId> operandsOf(SExpressionTree const& tree, ValueList value)
    {
        auto children = childrenOf(tree, value.list);
        children.erase(children.begin(),
                       children.begin()
                           + static_cast<std::ptrdiff_t>(std::min(value.keyword + 1, children.size())));
        return children;
    }

    /// The text of a string atom, which must be UTF-8 (a label, a `str.const`, a `char.const`).
    [[nodiscard]] Result<std::string> stringOf(SExpressionTree const& tree,
                                               NodeId node,
                                               std::string_view what)
    {
        if (!isAtomOf(tree, node, TokenKind::String))
            return std::unexpected { errorAt(
                tree, node, std::format("expected {}, got '{}'", what, tree.sourceOf(node))) };
        return text::decodeName(tree.source(), tree.tokenOf(node))
            .transform_error([](text::TextError const& error) {
                return ParseError { .line = error.position.line, .message = text::specMessage(error) };
            });
    }

    /// Requires @p operands to hold exactly @p count nodes.
    [[nodiscard]] Result<void> expectCount(SExpressionTree const& tree,
                                           ValueList list,
                                           std::span<NodeId const> operands,
                                           std::size_t count)
    {
        if (operands.size() != count)
            return std::unexpected { errorAt(tree,
                                             list.list,
                                             std::format("'{}' takes {} operand{}, got {}",
                                                         keywordOf(tree, list),
                                                         count,
                                                         count == 1 ? "" : "s",
                                                         operands.size())) };
        return {};
    }

    /// A number's bits: its one atom, parsed by @p parse; a literal error worded as the text format's.
    template <typename Parse>
    [[nodiscard]] Result<uint64_t> numberOf(SExpressionTree const& tree,
                                            ValueList list,
                                            std::span<NodeId const> operands,
                                            Parse parse)
    {
        if (auto const counted = expectCount(tree, list, operands, 1); !counted)
            return std::unexpected { counted.error() };
        auto const atom = operands.front();
        if (!isAtomOf(tree, atom, TokenKind::Keyword))
            return std::unexpected { errorAt(
                tree, atom, std::format("expected a number, got '{}'", tree.sourceOf(atom))) };
        return parse(tree.textOf(atom)).transform_error([&](text::LiteralError error) {
            auto const code = text::textErrorCodeOf(error);
            return errorAt(tree,
                           atom,
                           std::format("{} '{}'",
                                       text::TextErrorTable[static_cast<std::size_t>(code)].specMessage,
                                       tree.textOf(atom)));
        });
    }

    /// An integer of @p kind's width, read as wast 263 reads it: as a core iN literal, so an `sN` takes its
    /// whole unsigned range too (`(s8.const 200)` is -56), and a `uN` no negative number.
    [[nodiscard]] Result<uint64_t> integerOf(SExpressionTree const& tree,
                                             ValueList list,
                                             std::span<NodeId const> operands,
                                             ValueKind kind,
                                             LiteralSyntax syntax,
                                             text::TextLimits const& limits)
    {
        constexpr auto BitsPerByte = 8U;
        auto const width =
            static_cast<text::IntegerWidth>(component::canonical::valueShapeRow(kind).bytes * BitsPerByte);
        auto value = numberOf(tree, list, operands, [&](std::string_view literal) {
            return text::parseInteger(literal, width, text::IntegerSyntax::Either, limits);
        });
        if (value && syntax == LiteralSyntax::Unsigned && tree.textOf(operands.front()).starts_with('-'))
            return std::unexpected { errorAt(
                tree,
                operands.front(),
                std::format("constant out of range '{}'", tree.textOf(operands.front()))) };
        return value;
    }

    /// The one Unicode scalar value @p text holds.
    [[nodiscard]] std::optional<char32_t> onlyCodePoint(std::string const& text)
    {
        auto reader = component::canonical::CodePointReader { std::as_bytes(std::span { text }),
                                                              component::canonical::SimpleEncoding::Utf8 };
        auto const first = reader.next();
        if (!first || !*first)
            return std::nullopt;
        auto const second = reader.next();
        if (!second || *second)
            return std::nullopt;
        return *first;
    }

    /// Reads what one literal node holds after its keyword: its bits, text or labels into the node, the
    /// values among its operands, its children, into a list, in order.
    class OperandReader
    {
      public:
        OperandReader(SExpressionTree const& tree,
                      ValueList list,
                      ComponentValueForm const& row,
                      text::TextLimits const& limits,
                      LiteralNode& node,
                      std::vector<ValueList>& children):
            _tree { &tree },
            _list { list },
            _row { &row },
            _limits { &limits },
            _node { &node },
            _children { &children },
            _operands { operandsOf(tree, list) }
        {
        }

        /// Reads the node's operands by its form's syntax.
        [[nodiscard]] Result<void> read();

      private:
        using Read = Result<void> (OperandReader::*)();

        [[nodiscard]] Result<void> takeBits(Result<uint64_t> bits)
        {
            if (!bits)
                return std::unexpected { bits.error() };
            _node->bits = *bits;
            return {};
        }

        [[nodiscard]] Result<void> takeLabels(std::span<NodeId const> labels)
        {
            for (auto const label: labels)
            {
                auto text = stringOf(*_tree, label, "a label");
                if (!text)
                    return std::unexpected { text.error() };
                _node->labels.push_back(std::move(*text));
            }
            return {};
        }

        /// The operands as values, each a list of its own.
        void takeValues(std::span<NodeId const> values)
        {
            for (auto const value: values)
                _children->push_back(ValueList { .list = value, .keyword = 0 });
        }

        [[nodiscard]] Result<void> expect(std::size_t count) const
        {
            return expectCount(*_tree, _list, _operands, count);
        }

        [[nodiscard]] Result<void> readBool()
        {
            if (auto const counted = expect(1); !counted)
                return counted;
            auto const atom = _operands.front();
            auto const word =
                isAtomOf(*_tree, atom, TokenKind::Keyword) ? _tree->textOf(atom) : std::string_view {};
            if (word != "true" && word != "false")
                return std::unexpected { errorAt(
                    *_tree, atom, std::format("expected true or false, got '{}'", _tree->sourceOf(atom))) };
            _node->bits = word == "true" ? 1 : 0;
            return {};
        }

        [[nodiscard]] Result<void> readInteger()
        {
            return takeBits(integerOf(*_tree, _list, _operands, _row->kind, _row->syntax, *_limits));
        }

        [[nodiscard]] Result<void> readFloat()
        {
            auto const kind = _row->kind;
            auto const& limits = *_limits;
            return takeBits(
                numberOf(*_tree,
                         _list,
                         _operands,
                         [&](std::string_view literal) -> std::expected<uint64_t, text::LiteralError> {
                             if (kind == ValueKind::F32)
                                 return text::parseF32(literal, limits).transform([](uint32_t bits) {
                                     return uint64_t { bits };
                                 });
                             return text::parseF64(literal, limits);
                         }));
        }

        /// A `str.const`'s string, or a `char.const`'s one character.
        [[nodiscard]] Result<void> readText()
        {
            if (auto const counted = expect(1); !counted)
                return counted;
            auto const atom = _operands.front();
            auto text = stringOf(*_tree, atom, "a string");
            if (!text)
                return std::unexpected { text.error() };
            if (_row->syntax == LiteralSyntax::String)
            {
                _node->text = std::move(*text);
                return {};
            }
            auto const codePoint = onlyCodePoint(*text);
            if (!codePoint)
                return std::unexpected { errorAt(
                    *_tree, atom, std::format("'{}' is no one character", _tree->sourceOf(atom))) };
            _node->bits = *codePoint;
            return {};
        }

        [[nodiscard]] Result<void> readValues()
        {
            takeValues(_operands);
            return {};
        }

        /// Each operand a `(<keyword> …)` list, checked to be one.
        [[nodiscard]] Result<std::vector<NodeId>> wrappedOperands(NodeId wrapped,
                                                                  std::string_view keyword) const
        {
            if (keywordOf(*_tree, ValueList { .list = wrapped, .keyword = 0 }) != keyword)
                return std::unexpected { errorAt(
                    *_tree,
                    wrapped,
                    std::format("expected '({} …)', got '{}'", keyword, _tree->sourceOf(wrapped))) };
            return operandsOf(*_tree, ValueList { .list = wrapped, .keyword = 0 });
        }

        /// `(field "label" <value>)` per field, its value bare after its label (wast 263).
        [[nodiscard]] Result<void> readFields()
        {
            for (auto const wrapped: _operands)
            {
                auto const inner = wrappedOperands(wrapped, "field");
                if (!inner)
                    return std::unexpected { inner.error() };
                if (inner->size() < 2)
                    return std::unexpected { errorAt(*_tree, wrapped, "'field' takes a label and a value") };
                if (auto const taken = takeLabels(std::span { *inner }.first(1)); !taken)
                    return taken;
                _children->push_back(ValueList { .list = wrapped, .keyword = 2 });
            }
            return {};
        }

        /// `(entry <key> <value>)` per entry.
        [[nodiscard]] Result<void> readEntries()
        {
            for (auto const wrapped: _operands)
            {
                auto const inner = wrappedOperands(wrapped, "entry");
                if (!inner)
                    return std::unexpected { inner.error() };
                if (auto const counted =
                        expectCount(*_tree, ValueList { .list = wrapped, .keyword = 0 }, *inner, 2);
                    !counted)
                    return counted;
                takeValues(*inner);
            }
            return {};
        }

        [[nodiscard]] Result<void> readCase()
        {
            if (_operands.empty() || _operands.size() > 2)
                return std::unexpected { errorAt(
                    *_tree, _list.list, "'variant.const' takes a case label and at most one value") };
            if (auto const taken = takeLabels(std::span { _operands }.first(1)); !taken)
                return taken;
            takeValues(std::span { _operands }.subspan(1));
            return {};
        }

        [[nodiscard]] Result<void> readLabel()
        {
            if (auto const counted = expect(1); !counted)
                return counted;
            return takeLabels(_operands);
        }

        [[nodiscard]] Result<void> readLabels() { return takeLabels(_operands); }

        [[nodiscard]] Result<void> readNothing() { return expect(0); }

        [[nodiscard]] Result<void> readOneValue()
        {
            if (auto const counted = expect(1); !counted)
                return counted;
            takeValues(_operands);
            return {};
        }

        [[nodiscard]] Result<void> readOptionalValue()
        {
            if (_operands.size() > 1)
                return std::unexpected { errorAt(
                    *_tree, _list.list, std::format("'{}' takes at most one value", _row->keyword)) };
            takeValues(_operands);
            return {};
        }

        SExpressionTree const* _tree;
        ValueList _list;
        ComponentValueForm const* _row;
        text::TextLimits const* _limits;
        LiteralNode* _node;
        std::vector<ValueList>* _children;
        std::vector<NodeId> _operands;
    };

    Result<void> OperandReader::read()
    {
        /// What reads each LiteralSyntax, in its order.
        static constexpr auto Syntaxes = std::to_array<Read>({
            &OperandReader::readBool,
            &OperandReader::readInteger, // Signed
            &OperandReader::readInteger, // Unsigned
            &OperandReader::readFloat,
            &OperandReader::readText, // Char
            &OperandReader::readText, // String
            &OperandReader::readValues,
            &OperandReader::readFields,
            &OperandReader::readCase,
            &OperandReader::readLabel,
            &OperandReader::readLabels,
            &OperandReader::readEntries,
            &OperandReader::readNothing,
            &OperandReader::readOneValue,
            &OperandReader::readOptionalValue,
        });
        static_assert(Syntaxes.size() == static_cast<std::size_t>(LiteralSyntax::OptionalValue) + 1,
                      "Syntaxes holds one reader per LiteralSyntax");
        return (this->*Syntaxes.at(static_cast<std::size_t>(_row->syntax)))();
    }

    /// The failure of a literal that is no value of the type expected of it.
    [[nodiscard]] Failure mismatchAt(ComponentLiteral const& literal, std::string message)
    {
        return Failure { .stage = Stage::Harness,
                         .message = std::format("line {}: {}", literal.line, message) };
    }

    [[nodiscard]] std::string_view kindName(ValueKind kind) noexcept
    {
        return component::types::ValueKindTable[static_cast<std::size_t>(kind)].text;
    }

    /// The index of @p label among @p labels.
    [[nodiscard]] std::optional<uint32_t> labelIndex(std::span<component::types::SharedName const> labels,
                                                     std::string_view label)
    {
        auto const found =
            std::ranges::find_if(labels, [&](auto const& name) { return name.str() == label; });
        if (found == labels.end())
            return std::nullopt;
        return static_cast<uint32_t>(found - labels.begin());
    }

    /// What one literal node opens: the builder's begin() operand, and the types of its children.
    struct Opening
    {
        uint32_t operand = 0;            ///< begin()'s case or count.
        std::vector<TypeId> children {}; ///< Its children's types, in order.
    };

    /// The steps of one literal node of type @p type: a scalar step, or what a compound opens.
    using Step = std::variant<uint64_t, std::string_view, Opening>;

    /// The step a literal node takes as a value of @p type, or why it is none.
    [[nodiscard]] std::expected<Step, std::string> stepOf(LiteralNode const& node,
                                                          component::types::DefinedType const& type)
    {
        auto const& row = componentValueForm(node);
        auto const fixed = row.kind == ValueKind::List && type.kind == ValueKind::FixedList;
        if (row.kind != type.kind && !fixed)
            return std::unexpected { std::format(
                "({} …) is no value of type {}", row.keyword, kindName(type.kind)) };
        auto const payloadOf = [&](uint32_t caseIndex) -> std::expected<Step, std::string> {
            auto const& payload = type.children.at(caseIndex);
            if (payload.has_value() != (node.childCount == 1))
                return std::unexpected { std::format("({} …) gives {} payload where its case takes {}",
                                                     row.keyword,
                                                     node.childCount == 1 ? "a" : "no",
                                                     payload ? "one" : "none") };
            auto opening = Opening { .operand = caseIndex };
            if (payload)
                opening.children.push_back(*payload);
            return opening;
        };
        switch (row.syntax)
        {
            case LiteralSyntax::Bool:
            case LiteralSyntax::Signed:
            case LiteralSyntax::Unsigned:
            case LiteralSyntax::Float:
            case LiteralSyntax::Char: return node.bits;
            case LiteralSyntax::String: return std::string_view { node.text };
            case LiteralSyntax::Label: {
                auto const index = labelIndex(type.labels, node.labels.front());
                if (!index)
                    return std::unexpected { std::format("enum has no case \"{}\"", node.labels.front()) };
                return uint64_t { *index };
            }
            case LiteralSyntax::Labels: {
                auto bits = uint64_t { 0 };
                for (auto const& label: node.labels)
                {
                    auto const index = labelIndex(type.labels, label);
                    if (!index)
                        return std::unexpected { std::format("flags have no flag \"{}\"", label) };
                    bits |= uint64_t { 1 } << *index;
                }
                return bits;
            }
            case LiteralSyntax::Values: {
                auto opening = Opening { .operand = node.childCount };
                if (type.kind == ValueKind::Tuple)
                {
                    if (node.childCount != type.children.size())
                        return std::unexpected { std::format(
                            "tuple of {} elements given {}", type.children.size(), node.childCount) };
                    for (auto const& element: type.children)
                        opening.children.push_back(*element);
                    return opening;
                }
                if (fixed && node.childCount != type.length)
                    return std::unexpected { std::format(
                        "list of {} elements given {}", type.length, node.childCount) };
                opening.children.assign(node.childCount, *type.children.front());
                return opening;
            }
            case LiteralSyntax::Fields: {
                if (node.labels.size() != type.labels.size()
                    || !std::ranges::equal(
                        node.labels, type.labels, {}, {}, &component::types::SharedName::str))
                    return std::unexpected { std::string {
                        "record fields are not the type's, in its order" } };
                auto opening = Opening { .operand = node.childCount };
                for (auto const& field: type.children)
                    opening.children.push_back(*field);
                return opening;
            }
            case LiteralSyntax::Entries: {
                auto opening = Opening { .operand = node.childCount / 2 };
                for ([[maybe_unused]] auto const entry: std::views::iota(uint32_t { 0 }, node.childCount / 2))
                {
                    opening.children.push_back(*type.children.at(0));
                    opening.children.push_back(*type.children.at(1));
                }
                return opening;
            }
            case LiteralSyntax::Case: {
                auto const index = labelIndex(type.labels, node.labels.front());
                if (!index)
                    return std::unexpected { std::format("variant has no case \"{}\"", node.labels.front()) };
                return payloadOf(*index);
            }
            case LiteralSyntax::NoValue: return Opening { .operand = row.caseIndex };
            case LiteralSyntax::OneValue:
            case LiteralSyntax::OptionalValue:
                return payloadOf(type.kind == ValueKind::Option ? 0 : row.caseIndex)
                    .transform([&](Step step) {
                        std::get<Opening>(step).operand = row.caseIndex;
                        return step;
                    });
        }
        return std::unexpected { std::string { "unknown value syntax" } };
    }

    /// Builds one literal as a value of @p root.
    [[nodiscard]] std::expected<void, Failure> buildOne(ComponentLiteral const& literal,
                                                        TypeArena const& arena,
                                                        TypeId root,
                                                        component::canonical::ValueBuilder& builder)
    {
        auto types = std::vector<TypeId> { root }; // the types of the nodes to come, the next one last
        auto open = std::vector<uint32_t> {};      // the children each open compound still awaits
        for (auto const& node: literal.nodes)
        {
            auto const type = types.back();
            types.pop_back();
            auto const step = stepOf(node, arena[type]);
            if (!step)
                return std::unexpected { mismatchAt(literal, step.error()) };
            if (auto const* bits = std::get_if<uint64_t>(&*step))
                builder.scalar(*bits);
            else if (auto const* text = std::get_if<std::string_view>(&*step))
                builder.string(*text);
            else
            {
                auto const& opening = std::get<Opening>(*step);
                builder.begin(opening.operand);
                if (!opening.children.empty())
                {
                    types.insert(types.end(), opening.children.rbegin(), opening.children.rend());
                    open.push_back(static_cast<uint32_t>(opening.children.size()));
                    continue;
                }
                builder.end();
            }
            // A value is complete: it completes every compound it was the last child of.
            while (!open.empty() && --open.back() == 0)
            {
                open.pop_back();
                builder.end();
            }
        }
        if (auto const& error = builder.error())
            return std::unexpected { mismatchAt(literal, error->message()) };
        return {};
    }
} // namespace

std::expected<ComponentLiteral, ParseError> readComponentLiteral(SExpressionTree const& tree,
                                                                 NodeId list,
                                                                 text::TextLimits const& limits)
{
    auto literal = ComponentLiteral { .line = tree.positionOf(list).line, .nodes = {} };
    // The values to read, the next one last.
    auto pending = std::vector<ValueList> { ValueList { .list = list, .keyword = 0 } };
    while (!pending.empty())
    {
        auto const value = pending.back();
        pending.pop_back();
        auto const keyword = keywordOf(tree, value);
        auto const row = std::ranges::find(ComponentValueForms, keyword, &ComponentValueForm::keyword);
        if (keyword.empty() || row == ComponentValueForms.end())
            return std::unexpected { errorAt(
                tree,
                value.list,
                std::format("expected a component value, got '{}'", tree.sourceOf(value.list))) };
        auto out = LiteralNode {};
        out.form = static_cast<uint8_t>(row - ComponentValueForms.begin());
        auto children = std::vector<ValueList> {};
        if (auto const read = OperandReader { tree, value, *row, limits, out, children }.read(); !read)
            return std::unexpected { read.error() };
        out.childCount = static_cast<uint32_t>(children.size());
        literal.nodes.push_back(std::move(out));
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    // Each node's subtree, from the last node back: a node's children's subtrees follow it.
    auto sizes = std::vector<uint32_t> {};
    for (auto& node: std::views::reverse(literal.nodes))
    {
        node.subtreeSize = 1;
        for ([[maybe_unused]] auto const child: std::views::iota(uint32_t { 0 }, node.childCount))
        {
            node.subtreeSize += sizes.back();
            sizes.pop_back();
        }
        sizes.push_back(node.subtreeSize);
    }
    return literal;
}

std::expected<void, Failure> buildComponentValues(std::span<ComponentLiteral const> literals,
                                                  TypeArena const& arena,
                                                  std::span<TypeId const> roots,
                                                  component::canonical::ValueBuilder& builder)
{
    if (literals.size() != roots.size())
        return std::unexpected { Failure {
            .stage = Stage::Harness,
            .message = std::format("{} values given where {} are taken", literals.size(), roots.size()) } };
    for (auto const index: std::views::iota(std::size_t { 0 }, literals.size()))
        if (auto const built = buildOne(literals[index], arena, roots[index], builder); !built)
            return built;
    return {};
}

std::expected<component::canonical::Values, Failure> componentValuesOf(
    std::span<ComponentLiteral const> literals,
    TypeArena const& arena,
    std::span<TypeId const> roots,
    std::shared_ptr<component::canonical::LiveBytesBudget> budget)
{
    auto builder = component::canonical::ValueBuilder { arena, roots, std::move(budget) };
    if (auto const built = buildComponentValues(literals, arena, roots, builder); !built)
        return std::unexpected { built.error() };
    return builder.finish().transform_error([&](component::canonical::ValueError const& error) {
        return Failure { .stage = Stage::Harness, .message = error.message() };
    });
}

} // namespace isoline::spectest
