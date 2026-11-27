// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/canonical/Values.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/component/Limits.hpp>
#include <isoline/component/canonical/Layout.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::canonical
{

namespace
{

    using types::TypeId;
    using types::ValueKind;

    /// The most nodes a Values holds: a node's index and subtree size are 32-bit.
    constexpr auto MaxNodes = std::size_t { std::numeric_limits<uint32_t>::max() };

    /// The most bytes a string holds: its range's count is 32-bit.
    constexpr auto MaxStringBytes = std::size_t { std::numeric_limits<uint32_t>::max() };

    /// The name of a value kind, as errors and the wast forms spell it.
    [[nodiscard]] std::string_view kindText(ValueKind kind) noexcept
    {
        return types::valueKindRow(kind).text;
    }

    // ---------------------------------------------------------------------------------------------
    // Text: decoding a string's code points in each encoding, and encoding UTF-8.

    /// One row of the RFC 3629 table: a range of lead bytes, the sequence length they start, the
    /// range the second byte must fall in (which excludes overlongs, surrogates and code points
    /// above U+10FFFF), and the payload bits of the lead. Every later byte is a plain continuation.
    struct Utf8LeadRow
    {
        uint8_t leadFirst;
        uint8_t leadLast;
        uint8_t length;
        uint8_t secondFirst;
        uint8_t secondLast;
        uint8_t leadPayload;
    };

    constexpr auto Utf8LeadTable = std::to_array<Utf8LeadRow>({
        // clang-format off
        { .leadFirst = 0x00, .leadLast = 0x7F, .length = 1, .secondFirst = 0x80, .secondLast = 0xBF, .leadPayload = 0x7F },
        { .leadFirst = 0xC2, .leadLast = 0xDF, .length = 2, .secondFirst = 0x80, .secondLast = 0xBF, .leadPayload = 0x1F },
        { .leadFirst = 0xE0, .leadLast = 0xE0, .length = 3, .secondFirst = 0xA0, .secondLast = 0xBF, .leadPayload = 0x0F },
        { .leadFirst = 0xE1, .leadLast = 0xEC, .length = 3, .secondFirst = 0x80, .secondLast = 0xBF, .leadPayload = 0x0F },
        { .leadFirst = 0xED, .leadLast = 0xED, .length = 3, .secondFirst = 0x80, .secondLast = 0x9F, .leadPayload = 0x0F },
        { .leadFirst = 0xEE, .leadLast = 0xEF, .length = 3, .secondFirst = 0x80, .secondLast = 0xBF, .leadPayload = 0x0F },
        { .leadFirst = 0xF0, .leadLast = 0xF0, .length = 4, .secondFirst = 0x90, .secondLast = 0xBF, .leadPayload = 0x07 },
        { .leadFirst = 0xF1, .leadLast = 0xF3, .length = 4, .secondFirst = 0x80, .secondLast = 0xBF, .leadPayload = 0x07 },
        { .leadFirst = 0xF4, .leadLast = 0xF4, .length = 4, .secondFirst = 0x80, .secondLast = 0x8F, .leadPayload = 0x07 },
        // clang-format on
    });

    constexpr auto Utf8ContinuationFirst = uint8_t { 0x80 };
    constexpr auto Utf8ContinuationLast = uint8_t { 0xBF };
    constexpr auto Utf8ContinuationPayload = uint8_t { 0x3F };
    constexpr auto Utf8ContinuationBits = 6U;

    constexpr auto HighSurrogateFirst = char32_t { 0xD800 };
    constexpr auto LowSurrogateFirst = char32_t { 0xDC00 };
    constexpr auto SurrogateLast = char32_t { 0xDFFF };
    constexpr auto SurrogateBits = 10U;
    constexpr auto SupplementaryFirst = char32_t { 0x1'0000 };
    constexpr auto MaxCodePoint = char32_t { 0x10'FFFF };

    /// Reads a string's code points one at a time, in its encoding: no copy, no allocation.
    class CodePointReader
    {
      public:
        explicit CodePointReader(StringValue const& value) noexcept:
            _bytes { value.bytes }, _form { formOf(value) }
        {
        }

        /// The next code point.
        /// @return It, empty at the end, or why the bytes are not text.
        [[nodiscard]] std::expected<std::optional<char32_t>, StringError> next() noexcept
        {
            if (_position == _bytes.size())
                return std::nullopt;
            switch (_form)
            {
                case Form::Utf8: return nextUtf8();
                case Form::Utf16: return nextUtf16();
                case Form::Latin1: return char32_t { byteAt(_position++) };
            }
            return std::nullopt; // uncovered(exhaustive-switch)
        }

      private:
        enum class Form : uint8_t
        {
            Utf8,
            Utf16,
            Latin1,
        };

        [[nodiscard]] static Form formOf(StringValue const& value) noexcept
        {
            switch (value.encoding)
            {
                case StringEncoding::Utf8: return Form::Utf8;
                case StringEncoding::Utf16: return Form::Utf16;
                case StringEncoding::Latin1Utf16:
                    return (value.taggedCodeUnits & Utf16Tag) != 0 ? Form::Utf16 : Form::Latin1;
            }
            return Form::Utf8; // uncovered(exhaustive-switch)
        }

        [[nodiscard]] uint8_t byteAt(std::size_t index) const noexcept
        {
            return std::to_integer<uint8_t>(_bytes[index]);
        }

        [[nodiscard]] std::expected<std::optional<char32_t>, StringError> nextUtf8() noexcept
        {
            auto const lead = byteAt(_position);
            auto const row = std::ranges::find_if(Utf8LeadTable, [lead](Utf8LeadRow const& candidate) {
                return candidate.leadFirst <= lead && lead <= candidate.leadLast;
            });
            if (row == Utf8LeadTable.end() || _bytes.size() - _position < row->length)
                return std::unexpected { StringError::InvalidUtf8 };
            auto codePoint = static_cast<char32_t>(lead & row->leadPayload);
            auto first = row->secondFirst;
            auto last = row->secondLast;
            for (auto const offset: std::views::iota(std::size_t { 1 }, std::size_t { row->length }))
            {
                auto const byte = byteAt(_position + offset);
                if (byte < first || last < byte)
                    return std::unexpected { StringError::InvalidUtf8 };
                codePoint = (codePoint << Utf8ContinuationBits) | (byte & Utf8ContinuationPayload);
                first = Utf8ContinuationFirst;
                last = Utf8ContinuationLast;
            }
            _position += row->length;
            return codePoint;
        }

        [[nodiscard]] std::optional<char32_t> unitAt(std::size_t index) const noexcept
        {
            if (_bytes.size() - index < 2)
                return std::nullopt;
            return static_cast<char32_t>(byteAt(index) | (byteAt(index + 1) << 8U));
        }

        [[nodiscard]] std::expected<std::optional<char32_t>, StringError> nextUtf16() noexcept
        {
            auto const unit = unitAt(_position);
            if (!unit || (*unit >= LowSurrogateFirst && *unit <= SurrogateLast))
                return std::unexpected { StringError::InvalidUtf16 };
            if (*unit < HighSurrogateFirst || *unit > SurrogateLast)
            {
                _position += 2;
                return *unit;
            }
            auto const low = unitAt(_position + 2);
            if (!low || *low < LowSurrogateFirst || *low > SurrogateLast)
                return std::unexpected { StringError::InvalidUtf16 };
            _position += 4;
            return SupplementaryFirst + ((*unit - HighSurrogateFirst) << SurrogateBits)
                   + (*low - LowSurrogateFirst);
        }

        std::span<std::byte const> _bytes;
        std::size_t _position = 0;
        Form _form;
    };

    /// Appends @p codePoint (a Unicode scalar value) to @p text in UTF-8.
    void appendUtf8(std::string& text, char32_t codePoint)
    {
        auto const unit = [](uint32_t bits) {
            return static_cast<char>(bits);
        };
        constexpr auto Payload = 0x3FU;
        if (codePoint < 0x80)
            text.push_back(unit(codePoint));
        else if (codePoint < 0x800)
        {
            text.push_back(unit(0xC0U | (codePoint >> 6U)));
            text.push_back(unit(0x80U | (codePoint & Payload)));
        }
        else if (codePoint < SupplementaryFirst)
        {
            text.push_back(unit(0xE0U | (codePoint >> 12U)));
            text.push_back(unit(0x80U | ((codePoint >> 6U) & Payload)));
            text.push_back(unit(0x80U | (codePoint & Payload)));
        }
        else
        {
            text.push_back(unit(0xF0U | (codePoint >> 18U)));
            text.push_back(unit(0x80U | ((codePoint >> 12U) & Payload)));
            text.push_back(unit(0x80U | ((codePoint >> 6U) & Payload)));
            text.push_back(unit(0x80U | (codePoint & Payload)));
        }
    }

    /// Whether two strings hold the same text, whatever their encodings. Strings whose bytes are not
    /// text (which no ValueBuilder makes) compare by their text up to the fault and the fault.
    [[nodiscard]] bool sameText(StringValue const& lhs, StringValue const& rhs) noexcept
    {
        auto left = CodePointReader { lhs };
        auto right = CodePointReader { rhs };
        while (true)
        {
            auto const leftPoint = left.next();
            auto const rightPoint = right.next();
            if (leftPoint != rightPoint)
                return false;
            if (!leftPoint.has_value() || !leftPoint->has_value())
                return true;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Scalars: their validity, their bits in the pool, their text.

    /// Whether @p bits are a value of a scalar of @p row.
    [[nodiscard]] bool isScalarValue(ValueShapeRow const& row, uint64_t bits) noexcept
    {
        switch (row.rule)
        {
            case ScalarRule::Bool: return bits <= 1;
            case ScalarRule::Char:
                return bits <= MaxCodePoint && (bits < HighSurrogateFirst || bits > SurrogateLast);
            case ScalarRule::Unsigned:
            case ScalarRule::Signed:
            case ScalarRule::Float:
            case ScalarRule::None: break;
        }
        constexpr auto BitsPerByte = 8U;
        return row.bytes == sizeof(uint64_t) || (bits >> (BitsPerByte * row.bytes)) == 0;
    }

    /// The @p bytes low bytes of @p bits, as the pool holds a packed element.
    [[nodiscard]] std::array<std::byte, sizeof(uint64_t)> packedBytes(uint64_t bits,
                                                                      std::size_t bytes) noexcept
    {
        auto packed = std::array<std::byte, sizeof(uint64_t)> {};
        auto const copy = [&]<typename U>(U value) {
            std::memcpy(packed.data(), &value, sizeof(U));
        };
        switch (bytes)
        {
            case 1: copy(static_cast<uint8_t>(bits)); break;
            case 2: copy(static_cast<uint16_t>(bits)); break;
            case 4: copy(static_cast<uint32_t>(bits)); break;
            default: copy(bits); break;
        }
        return packed;
    }

    /// The bits of a packed element of @p bytes bytes at @p data.
    [[nodiscard]] uint64_t unpackedBits(std::byte const* data, std::size_t bytes) noexcept
    {
        auto const read = [data]<typename U>(U value) {
            std::memcpy(&value, data, sizeof(U));
            return uint64_t { value };
        };
        switch (bytes)
        {
            case 1: return read(uint8_t {});
            case 2: return read(uint16_t {});
            case 4: return read(uint32_t {});
            default: return read(uint64_t {});
        }
    }

    /// Whether @p bits are a NaN of a float of @p bytes bytes.
    [[nodiscard]] bool isNan(uint64_t bits, std::size_t bytes) noexcept
    {
        if (bytes == sizeof(float))
            return std::isnan(std::bit_cast<float>(static_cast<uint32_t>(bits)));
        return std::isnan(std::bit_cast<double>(bits));
    }

    /// One row of TextEscapes: a code point the wast string syntax escapes, and its escape.
    struct TextEscapeRow
    {
        char32_t codePoint;
        std::string_view escape;
    };

    constexpr auto TextEscapes = std::to_array<TextEscapeRow>({
        { .codePoint = U'\t', .escape = "\\t" },
        { .codePoint = U'\n', .escape = "\\n" },
        { .codePoint = U'\r', .escape = "\\r" },
        { .codePoint = U'"', .escape = "\\\"" },
        { .codePoint = U'\'', .escape = "\\'" },
        { .codePoint = U'\\', .escape = "\\\\" },
    });

    constexpr auto FirstPrintable = char32_t { 0x20 };
    constexpr auto Delete = char32_t { 0x7F };

    /// Appends @p codePoint as the wast string syntax spells it inside quotes.
    void appendEscaped(std::string& text, char32_t codePoint)
    {
        auto const escape = std::ranges::find(TextEscapes, codePoint, &TextEscapeRow::codePoint);
        if (escape != TextEscapes.end())
            text += escape->escape;
        else if (codePoint < FirstPrintable || codePoint == Delete)
            text += std::format("\\u{{{:x}}}", static_cast<uint32_t>(codePoint));
        else
            appendUtf8(text, codePoint);
    }

    /// Appends @p value quoted, as a wast string.
    void appendQuoted(std::string& text, StringValue const& value)
    {
        text.push_back('"');
        auto reader = CodePointReader { value };
        for (auto codePoint = reader.next(); codePoint && *codePoint; codePoint = reader.next())
            appendEscaped(text, **codePoint);
        text.push_back('"');
    }

    /// Appends @p label quoted.
    void appendQuoted(std::string& text, types::SharedName const& label)
    {
        auto const& name = label.str();
        appendQuoted(text,
                     StringValue { .bytes = std::as_bytes(std::span { name.data(), name.size() }),
                                   .encoding = StringEncoding::Utf8,
                                   .taggedCodeUnits = 0 });
    }

    /// Appends a float's wast text: nan, inf, -inf or its shortest round-trip decimal.
    template <typename F>
    void appendFloat(std::string& text, F value)
    {
        if (std::isnan(value))
            text += "nan";
        else if (std::isinf(value))
            text += value < 0 ? "-inf" : "inf";
        else
            text += std::format("{}", value);
    }

    /// The two's-complement integer of @p bytes bytes whose bits are @p bits.
    [[nodiscard]] int64_t signExtended(uint64_t bits, std::size_t bytes) noexcept
    {
        constexpr auto BitsPerByte = 8U;
        auto const shift = (sizeof(uint64_t) - bytes) * BitsPerByte;
        return static_cast<int64_t>(bits << shift) >> shift;
    }

    /// Appends a scalar's wast payload: its number, `true`/`false`, or its quoted char.
    void appendScalar(std::string& text, ValueShapeRow const& row, uint64_t bits)
    {
        switch (row.rule)
        {
            case ScalarRule::Bool: text += bits != 0 ? "true" : "false"; return;
            case ScalarRule::Char:
                text.push_back('"');
                appendEscaped(text, static_cast<char32_t>(bits));
                text.push_back('"');
                return;
            case ScalarRule::Signed: text += std::format("{}", signExtended(bits, row.bytes)); return;
            case ScalarRule::Float:
                if (row.bytes == sizeof(float))
                    appendFloat(text, std::bit_cast<float>(static_cast<uint32_t>(bits)));
                else
                    appendFloat(text, std::bit_cast<double>(bits));
                return;
            case ScalarRule::Unsigned:
            case ScalarRule::None: break;
        }
        text += std::format("{}", bits);
    }

    // ---------------------------------------------------------------------------------------------
    // Builder steps.

    /// The bit of @p shape in a set of shapes.
    [[nodiscard]] constexpr uint32_t shapeBit(ValueShape shape) noexcept
    {
        return uint32_t { 1 } << static_cast<uint32_t>(shape);
    }

    /// One row of BuildStepTable: a step, how errors name it, and the shapes it builds.
    struct BuildStepRow
    {
        BuildStep step;
        std::string_view text;
        uint32_t shapes;
    };

    /// Every builder step, one row per BuildStep enumerator, in enumerator order.
    constexpr auto BuildStepTable = std::to_array<BuildStepRow>({
        { .step = BuildStep::Scalar,
          .text = "a scalar",
          .shapes = shapeBit(ValueShape::Scalar) | shapeBit(ValueShape::Enum) | shapeBit(ValueShape::Flags) },
        { .step = BuildStep::String, .text = "a string", .shapes = shapeBit(ValueShape::String) },
        { .step = BuildStep::Begin,
          .text = "a compound",
          .shapes = shapeBit(ValueShape::Fields) | shapeBit(ValueShape::List)
                    | shapeBit(ValueShape::FixedList) | shapeBit(ValueShape::Cases)
                    | shapeBit(ValueShape::Map) },
        { .step = BuildStep::Resource, .text = "a handle", .shapes = shapeBit(ValueShape::Handle) },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: BuildStepTable)
            {
                if (static_cast<std::size_t>(row.step) != index)
                    return false;
                ++index;
            }
            return true;
        }(),
        "BuildStepTable must hold one row per BuildStep, at the index of its enumerator");

    /// How errors name a handle of each ownership, by Ownership enumerator.
    constexpr auto OwnershipTexts = std::to_array<std::string_view>({ "an own", "a borrow" });

    /// The ownership a handle type's values carry.
    [[nodiscard]] constexpr Ownership ownershipOf(ValueKind kind) noexcept
    {
        return kind == ValueKind::Own ? Ownership::Own : Ownership::Borrow;
    }

    /// The bits a handle node's payload packs a resource into.
    constexpr auto RepresentationBits = 32U;

    /// The payload type of case @p caseIndex of the variant, option or result @p type.
    [[nodiscard]] std::optional<TypeId> casePayload(types::DefinedType const& type,
                                                    uint32_t caseIndex) noexcept
    {
        auto const first = valueShapeRow(type.kind).firstPayloadCase;
        if (caseIndex < first)
            return std::nullopt;
        return type.children[caseIndex - first];
    }

    // ---------------------------------------------------------------------------------------------
    // Comparing and printing values.

    /// Whether two values are equal but for their children.
    [[nodiscard]] bool shallowEqual(ValueView lhs, ValueView rhs)
    {
        if (lhs.kind() != rhs.kind() || lhs.childCount() != rhs.childCount())
            return false;
        auto const& row = valueShapeRow(lhs.kind());
        switch (row.shape)
        {
            case ValueShape::Scalar:
                if (row.rule == ScalarRule::Float && isNan(lhs.scalarBits(), row.bytes))
                    return isNan(rhs.scalarBits(), row.bytes);
                return lhs.scalarBits() == rhs.scalarBits();
            case ValueShape::String: return sameText(lhs.string(), rhs.string());
            case ValueShape::Cases:
            case ValueShape::Enum: return lhs.caseIndex() == rhs.caseIndex();
            case ValueShape::Flags: return lhs.flagBits() == rhs.flagBits();
            case ValueShape::Handle: return lhs.resource() == rhs.resource();
            case ValueShape::Fields:
            case ValueShape::List:
            case ValueShape::FixedList:
            case ValueShape::Map:
            case ValueShape::Unsupported: break;
        }
        return true;
    }

    /// The wast keyword of an option's or a result's case.
    [[nodiscard]] std::string_view caseForm(ValueKind kind, uint32_t caseIndex) noexcept
    {
        auto const row = std::ranges::find_if(CaseForms, [&](CaseFormRow const& candidate) {
            return candidate.kind == kind && candidate.caseIndex == caseIndex;
        });
        Require(row != CaseForms.end());
        return row->form;
    }

    /// Appends a value's opening: `(form` and what it holds besides its children.
    void appendHead(std::string& text, types::TypeArena const& arena, ValueView value)
    {
        auto const& row = valueShapeRow(value.kind());
        text.push_back('(');
        switch (row.shape)
        {
            case ValueShape::Cases:
                if (value.kind() != ValueKind::Variant)
                {
                    text += caseForm(value.kind(), value.caseIndex());
                    return;
                }
                text += row.form;
                text.push_back(' ');
                appendQuoted(text, arena[value.type()].labels[value.caseIndex()]);
                return;
            case ValueShape::Scalar:
                text += row.form;
                text.push_back(' ');
                appendScalar(text, row, value.scalarBits());
                return;
            case ValueShape::String:
                text += row.form;
                text.push_back(' ');
                appendQuoted(text, value.string());
                return;
            case ValueShape::Enum:
                text += row.form;
                text.push_back(' ');
                appendQuoted(text, arena[value.type()].labels[value.caseIndex()]);
                return;
            case ValueShape::Flags: {
                text += row.form;
                auto const& labels = arena[value.type()].labels;
                for (auto const index: std::views::iota(std::size_t { 0 }, labels.size()))
                {
                    if (((value.flagBits() >> index) & 1U) == 0)
                        continue;
                    text.push_back(' ');
                    appendQuoted(text, labels[index]);
                }
                return;
            }
            case ValueShape::Handle: {
                auto const resource = value.resource();
                text += std::format("{} {} {}", row.form, resource.type.value, resource.rep);
                return;
            }
            case ValueShape::Fields:
            case ValueShape::List:
            case ValueShape::FixedList:
            case ValueShape::Map:
            case ValueShape::Unsupported: break;
        }
        text += row.form;
    }

    /// What precedes and what follows child @p index of @p parent: a record field's
    /// `(field "label" … )`, a map entry's `(entry key value)`.
    struct ChildFrame
    {
        std::string_view before;
        std::string_view after;
    };

    [[nodiscard]] ChildFrame childFrame(ValueKind parent, std::size_t index) noexcept
    {
        constexpr auto EntryWidth = std::size_t { 2 };
        if (parent == ValueKind::Record)
            return { .before = "(field ", .after = ")" };
        if (parent == ValueKind::Map)
            return index % EntryWidth == 0 ? ChildFrame { .before = "(entry ", .after = {} }
                                           : ChildFrame { .before = {}, .after = ")" };
        return {};
    }

} // namespace

// -------------------------------------------------------------------------------------------------
// LiveBytesBudget, BudgetExceeded, StringValue, ValueError

std::string BudgetExceeded::message() const
{
    return limitMessage(ComponentLimit::LiveValueBytes, limit);
}

std::expected<void, BudgetExceeded> LiveBytesBudget::charge(uint64_t bytes) noexcept
{
    auto current = _live.load(std::memory_order_relaxed);
    do
    {
        if (bytes > _limit - current)
            return std::unexpected { BudgetExceeded {
                .requested = bytes, .live = current, .limit = _limit } };
    } while (!_live.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed));
    return {};
}

void LiveBytesBudget::release(uint64_t bytes) noexcept
{
    auto const previous = _live.fetch_sub(bytes, std::memory_order_relaxed);
    Require(previous >= bytes);
}

std::expected<std::string, StringError> StringValue::utf8() const
{
    auto text = std::string {};
    text.reserve(bytes.size());
    auto reader = CodePointReader { *this };
    while (true)
    {
        auto const codePoint = reader.next();
        if (!codePoint)
            return std::unexpected { codePoint.error() };
        if (!*codePoint)
            return text;
        appendUtf8(text, **codePoint);
    }
}

std::string ValueError::message() const
{
    return std::vformat(ValueErrorMessages[static_cast<std::size_t>(code)].format,
                        std::make_format_args(path, detail));
}

// -------------------------------------------------------------------------------------------------
// Values, ValueView, ChildIterator

ValueView Values::root(std::size_t index) const
{
    Require(index < _roots.size());
    return ValueView { *this, _roots[index], 0, ViewForm::Node };
}

ValueNode const& ValueView::node() const
{
    return _values->_nodes[_node];
}

ValueRange const& ValueView::range() const
{
    return _values->_ranges[node().payload];
}

types::ValueKind ValueView::kind() const noexcept
{
    return _form == ViewForm::Node ? node().kind : range().elementKind;
}

types::TypeId ValueView::type() const noexcept
{
    return _form == ViewForm::Node ? node().type : range().element;
}

types::TypeArenaId ValueView::arena() const noexcept
{
    return _values->_arena;
}

uint64_t ValueView::scalarBits() const
{
    auto const& row = shapeRow();
    Require(row.shape == ValueShape::Scalar);
    if (_form == ViewForm::Node)
        return node().payload;
    auto const& list = range();
    auto const offset = list.offset + (uint64_t { _element } * row.bytes);
    return unpackedBits(_values->_pool.items().subspan(offset, row.bytes).data(), row.bytes);
}

StringValue ValueView::string() const
{
    Require(kind() == ValueKind::String);
    auto const& text = range();
    return StringValue { .bytes = _values->_pool.items().subspan(text.offset, text.count),
                         .encoding = text.encoding,
                         .taggedCodeUnits = text.taggedCodeUnits };
}

std::size_t ValueView::childCount() const noexcept
{
    if (_form == ViewForm::Element)
        return 0;
    auto const& value = node();
    switch (shapeRow().shape)
    {
        case ValueShape::Fields: return value.payload;
        case ValueShape::List:
        case ValueShape::FixedList: return range().count;
        case ValueShape::Map: return 2 * value.payload;
        case ValueShape::Cases: return value.subtreeSize > 1 ? 1 : 0;
        case ValueShape::Scalar:
        case ValueShape::String:
        case ValueShape::Enum:
        case ValueShape::Flags:
        case ValueShape::Handle:
        case ValueShape::Unsupported: break;
    }
    return 0;
}

ChildRange ValueView::children() const
{
    if (_form == ViewForm::Element)
    {
        auto const self = ChildIterator { *_values, _node, _element, ViewForm::Element };
        return ChildRange { self, self };
    }
    auto const& value = node();
    auto const shape = shapeRow().shape;
    if ((shape == ValueShape::List || shape == ValueShape::FixedList)
        && range().storage == ElementStorage::Pool)
        return ChildRange { ChildIterator { *_values, _node, 0, ViewForm::Element },
                            ChildIterator { *_values, _node, range().count, ViewForm::Element } };
    return ChildRange { ChildIterator { *_values, _node + 1, 0, ViewForm::Node },
                        ChildIterator { *_values, _node + value.subtreeSize, 0, ViewForm::Node } };
}

ValueView ValueView::child(std::size_t index) const
{
    Require(index < childCount());
    auto const children = this->children();
    auto position = children.begin();
    if (position._form == ViewForm::Element)
        return ValueView { *_values, _node, static_cast<uint32_t>(index), ViewForm::Element };
    for ([[maybe_unused]] auto const skipped: std::views::iota(std::size_t { 0 }, index))
        ++position;
    return *position;
}

uint32_t ValueView::caseIndex() const
{
    auto const shape = shapeRow().shape;
    Require(_form == ViewForm::Node && (shape == ValueShape::Cases || shape == ValueShape::Enum));
    return static_cast<uint32_t>(node().payload);
}

std::optional<ValueView> ValueView::payload() const
{
    Require(_form == ViewForm::Node && shapeRow().shape == ValueShape::Cases);
    if (node().subtreeSize == 1)
        return std::nullopt;
    return ValueView { *_values, _node + 1, 0, ViewForm::Node };
}

uint32_t ValueView::flagBits() const
{
    Require(_form == ViewForm::Node && shapeRow().shape == ValueShape::Flags);
    return static_cast<uint32_t>(node().payload);
}

ResourceValue ValueView::resource() const
{
    Require(_form == ViewForm::Node && shapeRow().shape == ValueShape::Handle);
    auto const bits = node().payload;
    return ResourceValue { .type = ResourceTypeIndex { static_cast<uint32_t>(bits >> RepresentationBits) },
                           .rep = static_cast<uint32_t>(bits),
                           .ownership = ownershipOf(kind()) };
}

ValueView ChildIterator::operator*() const noexcept
{
    return ValueView { *_values, _node, _element, _form };
}

ChildIterator& ChildIterator::operator++() noexcept
{
    if (_form == ViewForm::Element)
        ++_element;
    else
        _node += _values->_nodes[_node].subtreeSize;
    return *this;
}

// -------------------------------------------------------------------------------------------------
// ValueBuilder

ValueBuilder::ValueBuilder(types::TypeArena const& arena,
                           std::span<types::TypeId const> roots,
                           std::shared_ptr<LiveBytesBudget> budget):
    _arena { &arena },
    _rootTypes { roots.begin(), roots.end() },
    _nodes { budget },
    _pool { budget },
    _ranges { budget },
    _roots { std::move(budget) }
{
    for (auto const root: _rootTypes)
        Require(root.value < arena.valueTypeCount());
    if (auto reserved = reserve(_roots, _rootTypes.size()); !reserved)
        _error = std::move(reserved.error());
}

template <typename F>
ValueBuilder& ValueBuilder::step(F const& body)
{
    Require(_state == BuildState::Building);
    if (_error)
        return *this;
    if (auto done = body(); !done)
        _error = std::move(done.error());
    return *this;
}

ValueBuilder& ValueBuilder::scalar(uint64_t bits)
{
    return step([&] { return buildScalar(bits); });
}

ValueBuilder& ValueBuilder::string(std::string_view utf8)
{
    return step([&] { return buildString(utf8); });
}

ValueBuilder& ValueBuilder::begin(uint32_t caseIndexOrCount)
{
    return step([&] { return buildBegin(caseIndexOrCount); });
}

ValueBuilder& ValueBuilder::end()
{
    return step([&] { return buildEnd(); });
}

ValueBuilder& ValueBuilder::resource(ResourceValue value)
{
    return step([&] { return buildResource(value); });
}

std::expected<Values, ValueError> ValueBuilder::finish()
{
    Require(_state == BuildState::Building);
    _state = BuildState::Finished;
    if (_error)
        return std::unexpected { *_error };
    if (auto balanced = unbalancedAtFinish(); !balanced)
        return std::unexpected { std::move(balanced.error()) };
    return Values {
        _arena->id(), std::move(_nodes), std::move(_pool), std::move(_ranges), std::move(_roots)
    };
}

std::expected<void, ValueError> ValueBuilder::unbalancedAtFinish() const
{
    if (!_frames.empty())
    {
        auto const& frame = _frames.back();
        return std::unexpected { errorHere(ValueErrorCode::Unbalanced,
                                           std::format("the {} is open, {} of its {} children given",
                                                       kindText(frame.kind),
                                                       frame.given,
                                                       frame.expected)) };
    }
    if (_nextRoot < _rootTypes.size())
        return std::unexpected { errorHere(
            ValueErrorCode::Unbalanced, std::format("{} of {} values given", _nextRoot, _rootTypes.size())) };
    return {};
}

ValueError ValueBuilder::errorHere(ValueErrorCode code, std::string detail) const
{
    auto path = std::format("[{}]", _nextRoot);
    for (auto const& frame: _frames)
    {
        auto const index =
            valueShapeRow(frame.kind).shape == ValueShape::Cases ? uint64_t { frame.caseIndex } : frame.given;
        path += std::format("[{}]", index);
    }
    return ValueError { .code = code, .path = std::move(path), .detail = std::move(detail) };
}

template <typename T>
std::expected<void, ValueError> ValueBuilder::reserve(ChargedVector<T>& vector, std::size_t extra) const
{
    return vector.reserveFor(extra).transform_error(
        [&](BudgetExceeded const& refusal) { return errorHere(ValueErrorCode::Budget, refusal.message()); });
}

std::expected<ValueBuilder::Slot, ValueError> ValueBuilder::slotFor(BuildStep step) const
{
    auto slot = Slot {};
    if (_frames.empty())
    {
        if (_nextRoot == _rootTypes.size())
            return std::unexpected { errorHere(
                ValueErrorCode::Unbalanced,
                std::format("a value past the last of {} values", _rootTypes.size())) };
        slot.type = _rootTypes[_nextRoot];
    }
    else
    {
        auto const& frame = _frames.back();
        if (frame.given == frame.expected)
            return std::unexpected { errorHere(ValueErrorCode::Unbalanced,
                                               std::format("a value past the {} children of the {}",
                                                           frame.expected,
                                                           kindText(frame.kind))) };
        auto const& parent = (*_arena)[frame.type];
        auto child = std::optional<TypeId> {};
        switch (valueShapeRow(frame.kind).shape)
        {
            case ValueShape::Fields: child = parent.children[frame.given]; break;
            case ValueShape::Cases: child = casePayload(parent, frame.caseIndex); break;
            case ValueShape::Map: child = parent.children[frame.given % 2]; break;
            default: child = parent.children[0]; break;
        }
        Require(child.has_value());
        slot.type = *child;
        slot.storage = frame.storage;
    }
    slot.kind = (*_arena)[slot.type].kind;
    auto const& row = valueShapeRow(slot.kind);
    if (row.shape == ValueShape::Unsupported)
        return std::unexpected { errorHere(
            ValueErrorCode::Unsupported,
            std::format("{} values are not built before P12", kindText(slot.kind))) };
    auto const& stepRow = BuildStepTable[static_cast<std::size_t>(step)];
    if ((stepRow.shapes & shapeBit(row.shape)) == 0)
        return std::unexpected { errorHere(
            ValueErrorCode::TypeMismatch,
            std::format("expected {}, given {}", kindText(slot.kind), stepRow.text)) };
    return slot;
}

std::expected<void, ValueError> ValueBuilder::pushNode(ValueNode node, uint64_t reserveAfter)
{
    if (_nodes.size() == MaxNodes)
        // uncovered-begin(node-count)
        return std::unexpected { errorHere(ValueErrorCode::TooLarge,
                                           std::format("more than {} nodes", MaxNodes)) };
    // uncovered-end(node-count)
    if (auto reserved = reserve(_nodes, 1 + reserveAfter); !reserved)
        return reserved;
    if (_frames.empty())
        _roots.append(static_cast<uint32_t>(_nodes.size()));
    _nodes.append(node);
    return {};
}

void ValueBuilder::completed() noexcept
{
    if (_frames.empty())
        ++_nextRoot;
    else
        ++_frames.back().given;
}

std::expected<void, ValueError> ValueBuilder::buildScalar(uint64_t bits)
{
    auto const slot = slotFor(BuildStep::Scalar);
    if (!slot)
        return std::unexpected { slot.error() };
    auto const& row = valueShapeRow(slot->kind);
    auto const& type = (*_arena)[slot->type];
    if (row.shape == ValueShape::Enum && bits >= type.labels.size())
        return std::unexpected { errorHere(
            ValueErrorCode::InvalidValue,
            std::format("case {} of an enum of {}", bits, type.labels.size())) };
    if (row.shape == ValueShape::Flags && type.labels.size() > MaxFlagLabels)
        return std::unexpected { errorHere(
            ValueErrorCode::TooManyFlags,
            std::format("a flags type of {} labels, more than the {}", type.labels.size(), MaxFlagLabels)) };
    if (row.shape == ValueShape::Flags && (bits >> type.labels.size()) != 0)
        return std::unexpected { errorHere(
            ValueErrorCode::InvalidValue,
            std::format("bits {:#x} past the {} labels", bits, type.labels.size())) };
    if (row.shape == ValueShape::Scalar && !isScalarValue(row, bits))
        return std::unexpected { errorHere(ValueErrorCode::InvalidValue,
                                           std::format("{:#x} is not a {}", bits, kindText(slot->kind))) };
    if (slot->storage == ElementStorage::Pool)
    {
        auto const packed = packedBytes(bits, row.bytes);
        _pool.append(std::span { packed }.first(row.bytes));
    }
    else if (auto pushed = pushNode(ValueNode { .type = slot->type, .kind = slot->kind, .payload = bits }, 0);
             !pushed)
        return pushed;
    completed();
    return {};
}

std::expected<void, ValueError> ValueBuilder::buildString(std::string_view utf8)
{
    auto const slot = slotFor(BuildStep::String);
    if (!slot)
        return std::unexpected { slot.error() };
    auto const bytes = std::as_bytes(std::span { utf8.data(), utf8.size() });
    auto const text = StringValue { .bytes = bytes, .encoding = StringEncoding::Utf8 };
    auto reader = CodePointReader { text };
    for (auto codePoint = reader.next(); !codePoint || *codePoint; codePoint = reader.next())
    {
        if (!codePoint)
            return std::unexpected { errorHere(ValueErrorCode::InvalidValue, "invalid UTF-8") };
    }
    if (bytes.size() > MaxStringBytes)
        // uncovered-begin(string-bytes)
        return std::unexpected { errorHere(ValueErrorCode::TooLarge,
                                           std::format("a string of {} bytes", bytes.size())) };
    // uncovered-end(string-bytes)
    auto const range = ValueRange { .offset = _pool.size(),
                                    .count = static_cast<uint32_t>(bytes.size()),
                                    .taggedCodeUnits = static_cast<uint32_t>(bytes.size()),
                                    .encoding = StringEncoding::Utf8 };
    auto const node = ValueNode { .type = slot->type, .kind = slot->kind, .payload = _ranges.size() };
    auto const stored =
        reserve(_pool, bytes.size()).and_then([&] { return reserve(_ranges, 1); }).and_then([&] {
            return pushNode(node, 0);
        });
    if (!stored)
        return stored;
    _pool.append(bytes);
    _ranges.append(range);
    completed();
    return {};
}

std::expected<void, ValueError> ValueBuilder::buildBegin(uint32_t caseIndexOrCount)
{
    auto const slot = slotFor(BuildStep::Begin);
    if (!slot)
        return std::unexpected { slot.error() };
    auto const& row = valueShapeRow(slot->kind);
    auto const& type = (*_arena)[slot->type];
    auto const count = uint64_t { caseIndexOrCount };
    auto frame =
        Frame { .node = static_cast<uint32_t>(_nodes.size()), .type = slot->type, .kind = slot->kind };
    auto node = ValueNode { .type = slot->type, .kind = slot->kind, .payload = count, .subtreeSize = 0 };
    auto childNodes = uint64_t { 0 };
    switch (row.shape)
    {
        case ValueShape::Fields:
            if (count != type.children.size())
                return std::unexpected { errorHere(ValueErrorCode::TypeMismatch,
                                                   std::format("{} fields begun for a {} of {}",
                                                               count,
                                                               kindText(slot->kind),
                                                               type.children.size())) };
            frame.expected = count;
            childNodes = count;
            break;
        case ValueShape::FixedList:
        case ValueShape::List: {
            if (row.shape == ValueShape::FixedList && count != type.length)
                return std::unexpected { errorHere(
                    ValueErrorCode::TypeMismatch,
                    std::format("{} elements begun for a list of {}", count, type.length)) };
            auto const element = *type.children[0];
            auto const elementKind = (*_arena)[element].kind;
            auto const& elementRow = valueShapeRow(elementKind);
            auto const storage =
                elementRow.shape == ValueShape::Scalar ? ElementStorage::Pool : ElementStorage::Nodes;
            // The count's reservation first: a count that lies is refused before anything grows.
            auto reserved = storage == ElementStorage::Pool ? reserve(_pool, count * elementRow.bytes)
                                                            : reserve(_nodes, 1 + count);
            if (reserved)
                reserved = reserve(_ranges, 1);
            if (!reserved)
                return reserved;
            frame.expected = count;
            frame.storage = storage;
            childNodes = storage == ElementStorage::Nodes ? count : 0;
            node.payload = _ranges.size();
            _ranges.append(ValueRange { .offset = _pool.size(),
                                        .count = caseIndexOrCount,
                                        .element = element,
                                        .elementKind = elementKind,
                                        .storage = storage });
            break;
        }
        case ValueShape::Cases: {
            auto const cases = type.children.size() + row.firstPayloadCase;
            if (count >= cases)
                return std::unexpected { errorHere(
                    ValueErrorCode::InvalidValue,
                    std::format("case {} of a {} of {}", count, kindText(slot->kind), cases)) };
            frame.caseIndex = caseIndexOrCount;
            frame.expected = casePayload(type, caseIndexOrCount).has_value() ? 1 : 0;
            childNodes = frame.expected;
            break;
        }
        default: // ValueShape::Map, the last shape slotFor() lets begin() through
            frame.expected = 2 * count;
            childNodes = 2 * count;
            break;
    }
    if (auto pushed = pushNode(node, childNodes); !pushed)
        return pushed;
    _frames.push_back(frame);
    return {};
}

std::expected<void, ValueError> ValueBuilder::buildEnd()
{
    if (_frames.empty())
        return std::unexpected { errorHere(ValueErrorCode::Unbalanced, "end() without begin()") };
    auto const& frame = _frames.back();
    if (frame.given != frame.expected)
        return std::unexpected { errorHere(
            ValueErrorCode::Unbalanced,
            std::format(
                "{} of the {}'s {} children given", frame.given, kindText(frame.kind), frame.expected)) };
    _nodes[frame.node].subtreeSize = static_cast<uint32_t>(_nodes.size() - frame.node);
    _frames.pop_back();
    completed();
    return {};
}

std::expected<void, ValueError> ValueBuilder::buildResource(ResourceValue value)
{
    auto const slot = slotFor(BuildStep::Resource);
    if (!slot)
        return std::unexpected { slot.error() };
    if (value.ownership != ownershipOf(slot->kind))
        return std::unexpected { errorHere(
            ValueErrorCode::TypeMismatch,
            std::format("expected {}, given {}",
                        kindText(slot->kind),
                        OwnershipTexts[static_cast<std::size_t>(value.ownership)])) };
    auto const payload = (uint64_t { value.type.value } << RepresentationBits) | value.rep;
    if (auto pushed = pushNode(ValueNode { .type = slot->type, .kind = slot->kind, .payload = payload }, 0);
        !pushed)
        return pushed;
    completed();
    return {};
}

// -------------------------------------------------------------------------------------------------
// equal, toText

bool equal(ValueView lhs, ValueView rhs)
{
    if (!shallowEqual(lhs, rhs))
        return false;
    struct Pair
    {
        ChildIterator left;
        ChildIterator leftEnd;
        ChildIterator right;
    };
    auto pending = std::vector<Pair> { Pair {
        .left = lhs.children().begin(), .leftEnd = lhs.children().end(), .right = rhs.children().begin() } };
    while (!pending.empty())
    {
        auto& top = pending.back();
        if (top.left == top.leftEnd)
        {
            pending.pop_back();
            continue;
        }
        auto const left = *top.left;
        auto const right = *top.right;
        ++top.left;
        ++top.right;
        if (!shallowEqual(left, right))
            return false;
        if (left.childCount() > 0)
            pending.push_back(Pair { .left = left.children().begin(),
                                     .leftEnd = left.children().end(),
                                     .right = right.children().begin() });
    }
    return true;
}

std::string toText(types::TypeArena const& arena, ValueView value)
{
    Require(arena.id() == value.arena());
    struct Frame
    {
        ChildIterator next;
        ChildIterator end;
        ValueView parent;
        std::size_t index = 0;
    };
    auto text = std::string {};
    auto pending = std::vector<Frame> {};
    auto const open = [&](ValueView opened) {
        appendHead(text, arena, opened);
        if (opened.childCount() == 0)
            text.push_back(')');
        else
            pending.push_back(Frame {
                .next = opened.children().begin(), .end = opened.children().end(), .parent = opened });
    };
    open(value);
    while (!pending.empty())
    {
        auto& top = pending.back();
        if (top.index > 0)
            text += childFrame(top.parent.kind(), top.index - 1).after;
        if (top.next == top.end)
        {
            text.push_back(')');
            pending.pop_back();
            continue;
        }
        auto const child = *top.next;
        auto const frame = childFrame(top.parent.kind(), top.index);
        text.push_back(' ');
        text += frame.before;
        if (top.parent.kind() == ValueKind::Record)
        {
            appendQuoted(text, arena[top.parent.type()].labels[top.index]);
            text.push_back(' ');
        }
        ++top.next;
        ++top.index;
        open(child);
    }
    return text;
}

} // namespace isoline::component::canonical
