// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/text/ComponentWriter.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/text/Forms.hpp>
#include <isoline/text/Keywords.hpp>
#include <isoline/text/ModuleWriter.hpp>

namespace isoline::component::text
{

namespace
{
    /// A string literal of @p bytes: printable ASCII as it is, every other byte, `"` and `\` escaped.
    std::string quoted(std::string_view bytes)
    {
        auto text = std::string { "\"" };
        for (auto const c: bytes)
        {
            auto const byte = static_cast<uint8_t>(c);
            if (byte >= 0x20 && byte < 0x7F && c != '"' && c != '\\')
                text += c;
            else
                text += std::format("\\{:02x}", byte);
        }
        text += '"';
        return text;
    }

    /// A name and its attributes: `"name" (implements "…")…`.
    std::string nameText(binary::NameAttributes const& name)
    {
        auto text = quoted(name.name);
        for (auto const& attribute: name.attributes)
        {
            auto const& row = binary::NameAttributeTable[attribute.index()];
            auto const value = std::visit(
                [](auto const& alternative) -> std::string_view {
                    using Alternative = std::remove_cvref_t<decltype(alternative)>;
                    if constexpr (std::is_same_v<Alternative, binary::Implements>)
                        return alternative.interfaceName;
                    else if constexpr (std::is_same_v<Alternative, binary::VersionSuffix>)
                        return alternative.suffix;
                    else
                        return alternative.id;
                },
                attribute);
            text += std::format(" ({} {})", row.keyword, quoted(value));
        }
        return text;
    }

    /// A sort's keyword: `core func`, `func`, …
    std::string sortKeyword(binary::Sort sort)
    {
        auto const& row = sortText(sort);
        return row.prefix == SortPrefix::Core ? std::format("core {}", row.keyword)
                                              : std::string { row.keyword };
    }

    /// The `(;n;)` comment of a definition's index.
    std::string comment(uint32_t index)
    {
        return std::format("(;{};)", index);
    }

    std::string valueTypeText(binary::ValueTypeRef const& type)
    {
        if (auto const* primitive = std::get_if<binary::PrimitiveValueType>(&type))
            return std::string { binary::PrimitiveValueTypeTable[static_cast<std::size_t>(*primitive)].text };
        return std::to_string(std::get<binary::TypeIndex>(type).index);
    }

    std::string optionalTypeText(std::optional<binary::ValueTypeRef> const& type)
    {
        return type ? " " + valueTypeText(*type) : std::string {};
    }

    std::string labels(std::vector<std::string> const& names)
    {
        auto text = std::string {};
        for (auto const& name: names)
            text += " " + quoted(name);
        return text;
    }

    std::string definedTypeText(binary::DefinedValueType const& type)
    {
        if (auto const* primitive = std::get_if<binary::PrimitiveValueType>(&type))
            return std::string { binary::PrimitiveValueTypeTable[static_cast<std::size_t>(*primitive)].text };
        auto const keyword = binary::DefinedValueTypeTable[type.index() - 1].keyword;
        auto const body = std::visit(
            [](auto const& alternative) -> std::string {
                using Alternative = std::remove_cvref_t<decltype(alternative)>;
                auto text = std::string {};
                if constexpr (std::is_same_v<Alternative, binary::RecordType>)
                    for (auto const& field: alternative.fields)
                        text += std::format(" (field {} {})", quoted(field.label), valueTypeText(field.type));
                else if constexpr (std::is_same_v<Alternative, binary::VariantType>)
                    for (auto const& entry: alternative.cases)
                        text +=
                            std::format(" (case {}{})", quoted(entry.label), optionalTypeText(entry.type));
                else if constexpr (std::is_same_v<Alternative, binary::ListType>)
                    text = " " + valueTypeText(alternative.element);
                else if constexpr (std::is_same_v<Alternative, binary::FixedListType>)
                    text = std::format(" {} {}", valueTypeText(alternative.element), alternative.length);
                else if constexpr (std::is_same_v<Alternative, binary::TupleType>)
                    for (auto const& element: alternative.elements)
                        text += " " + valueTypeText(element);
                else if constexpr (std::is_same_v<Alternative, binary::FlagsType>
                                   || std::is_same_v<Alternative, binary::EnumType>)
                    text = labels(alternative.labels);
                else if constexpr (std::is_same_v<Alternative, binary::OptionType>)
                    text = " " + valueTypeText(alternative.type);
                else if constexpr (std::is_same_v<Alternative, binary::ResultType>)
                {
                    text = optionalTypeText(alternative.ok);
                    if (alternative.error)
                        text += std::format(" (error {})", valueTypeText(*alternative.error));
                }
                else if constexpr (std::is_same_v<Alternative, binary::OwnType>
                                   || std::is_same_v<Alternative, binary::BorrowType>)
                    text = std::format(" {}", alternative.resource);
                else if constexpr (std::is_same_v<Alternative, binary::StreamType>
                                   || std::is_same_v<Alternative, binary::FutureType>)
                    text = optionalTypeText(alternative.element);
                else if constexpr (std::is_same_v<Alternative, binary::MapType>)
                    text = std::format(
                        " {} {}", valueTypeText(alternative.key), valueTypeText(alternative.value));
                return text;
            },
            type);
        return std::format("({}{})", keyword, body);
    }

    std::string functionTypeText(binary::FunctionTypeDefinition const& type)
    {
        auto text = std::string { "(func" };
        if (type.asyncness == binary::Asyncness::Async)
            text += " async";
        for (auto const& param: type.params)
            text += std::format(" (param {} {})", quoted(param.label), valueTypeText(param.type));
        if (type.result)
            text += std::format(" (result {})", valueTypeText(*type.result));
        return text + ")";
    }

    std::string coreValueTypeText(isoline::binary::ValueType const& type)
    {
        auto const keyword =
            std::ranges::find(isoline::text::ValueTypeKeywords, type, &isoline::text::ValueTypeKeyword::type);
        if (keyword != isoline::text::ValueTypeKeywords.end())
            return std::string { keyword->keyword };
        auto const heap = std::ranges::find(
            isoline::text::HeapTypeKeywords, type.heap.kind, &isoline::text::HeapTypeKeyword::heap);
        auto const heapText = type.heap.kind == isoline::binary::AbstractHeapType::Concrete
                                      || heap == isoline::text::HeapTypeKeywords.end()
                                  ? std::to_string(type.heap.typeIndex)
                                  : std::string { heap->keyword };
        return std::format(
            "(ref {}{})", type.nullable == isoline::binary::Nullable::Yes ? "null " : "", heapText);
    }

    std::string coreFunctionTypeText(isoline::binary::FuncType const& type)
    {
        auto text = std::string { "(func" };
        auto const list = [&](std::string_view keyword,
                              std::vector<isoline::binary::ValueType> const& types) {
            if (types.empty())
                return;
            text += std::format(" ({}", keyword);
            for (auto const& value: types)
                text += " " + coreValueTypeText(value);
            text += ")";
        };
        list("param", type.params);
        list("result", type.results);
        return text + ")";
    }

    std::string limitsText(isoline::binary::Limits const& limits)
    {
        auto text =
            limits.addressType == isoline::binary::AddressType::I64 ? std::string { "i64 " } : std::string {};
        text += std::to_string(limits.min);
        if (limits.max)
            text += " " + std::to_string(*limits.max);
        return text;
    }

    std::string memoryTypeText(isoline::binary::MemoryType const& type)
    {
        auto const page = type.pageSizeLog2 == isoline::binary::DefaultPageSizeLog2
                              ? std::string {}
                              : std::format(" (pagesize {})", uint64_t { 1 } << type.pageSizeLog2);
        return std::format("(memory {}{})", limitsText(type.limits), page);
    }

    std::string globalTypeText(isoline::binary::GlobalType const& type)
    {
        auto const value = coreValueTypeText(type.type);
        if (type.mutability == isoline::binary::Mutability::Var)
            return std::format("(global (mut {}))", value);
        return std::format("(global {})", value);
    }

    std::string coreExternTypeText(binary::CoreExternType const& type)
    {
        switch (type.kind)
        {
            case isoline::binary::ExternKind::Func: return std::format("(func (type {}))", type.typeIndex);
            case isoline::binary::ExternKind::Tag: return std::format("(tag (type {}))", type.typeIndex);
            case isoline::binary::ExternKind::Table:
                return std::format(
                    "(table {} {})", limitsText(type.table.limits), coreValueTypeText(type.table.element));
            case isoline::binary::ExternKind::Memory: return memoryTypeText(type.memory);
            case isoline::binary::ExternKind::Global: return globalTypeText(type.global);
        }
        return {}; // uncovered(exhaustive-switch)
    }

    std::string moduleDeclarationText(binary::CoreModuleDeclaration const& declaration, uint32_t& types)
    {
        return std::visit(
            [&](auto const& alternative) -> std::string {
                using Alternative = std::remove_cvref_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Alternative, binary::CoreImportDeclaration>)
                    return std::format("(import {} {} {})",
                                       quoted(alternative.module),
                                       quoted(alternative.name),
                                       coreExternTypeText(alternative.type));
                else if constexpr (std::is_same_v<Alternative, binary::CoreTypeDeclaration>)
                    return std::format(
                        "(type {} {})", comment(types++), coreFunctionTypeText(alternative.type));
                else if constexpr (std::is_same_v<Alternative, binary::CoreOuterAliasDeclaration>)
                    return std::format("(alias outer {} {} (type {}))",
                                       alternative.outerCount,
                                       alternative.outerIndex,
                                       comment(types++));
                else
                    return std::format(
                        "(export {} {})", quoted(alternative.name), coreExternTypeText(alternative.type));
            },
            declaration);
    }

    /// An import's or an export declaration's type: `(func (;n;) (type t))`, … @p index is the comment,
    /// none for an export's ascription.
    std::string externDescText(binary::ExternDesc const& type, std::optional<uint32_t> index)
    {
        auto const id = index ? " " + comment(*index) : std::string {};
        if (type.sort == binary::Sort::Type)
        {
            if (type.bound == binary::TypeBound::SubResource)
                return std::format("(type{} (sub resource))", id);
            return std::format("(type{} (eq {}))", id, type.index);
        }
        return std::format("({}{} (type {}))", sortKeyword(type.sort), id, type.index);
    }

    /// A canonical definition's immediates and options, as ComponentReader reads them.
    std::string canonText(binary::CanonDefinition const& canon, uint32_t index)
    {
        auto const& row =
            *std::ranges::find(binary::CanonBuiltinTable, canon.builtin, &binary::CanonBuiltinRow::builtin);
        auto const& syntaxes = text::canonText(canon.builtin).indices;
        auto text = std::format("(canon {}", row.text);
        auto stored = std::size_t { 0 };
        auto next = std::size_t { 0 };
        auto liftType = uint32_t { 0 };
        for (auto const immediate: row.shape)
        {
            switch (immediate)
            {
                case binary::Immediate::None:
                case binary::Immediate::SortByte:
                case binary::Immediate::Reserved: break;
                case binary::Immediate::Index: {
                    auto const value = canon.immediates[stored++];
                    switch (syntaxes[next++])
                    {
                        case IndexSyntax::Func: text += std::format(" (func {})", value); break;
                        case IndexSyntax::CoreFunc: text += std::format(" (core func {})", value); break;
                        case IndexSyntax::Memory: text += std::format(" (memory {})", value); break;
                        case IndexSyntax::LiftType: liftType = value; break;
                        case IndexSyntax::Type:
                        case IndexSyntax::CoreType:
                        case IndexSyntax::CoreTable:
                        case IndexSyntax::Slot:
                        case IndexSyntax::None: text += std::format(" {}", value); break;
                    }
                    break;
                }
                case binary::Immediate::Options:
                    for (auto const& option: canon.options)
                    {
                        auto const& optionRow =
                            binary::CanonOptionTable[static_cast<std::size_t>(option.kind)];
                        if (optionRow.immediate == binary::OptionImmediate::Index)
                            text += std::format(" ({} {})", optionRow.text, option.index);
                        else
                            text += std::format(" {}", optionRow.text);
                    }
                    break;
                case binary::Immediate::ResultList:
                    if (canon.result)
                        text += std::format(" (result {})", valueTypeText(*canon.result));
                    break;
                case binary::Immediate::CoreValueType:
                    text += std::format(" {}", CoreValueKeywords[canon.immediates[stored++]].keyword);
                    break;
                case binary::Immediate::Async:
                    if (canon.immediates[stored++] != 0)
                        text += " async";
                    break;
                case binary::Immediate::Shared:
                    if (canon.immediates[stored++] != 0)
                        text += " shared";
                    break;
            }
        }
        if (canon.builtin == binary::CanonBuiltin::Lift)
            return text + std::format(" (func {} (type {})))", comment(index), liftType);
        return text + std::format(" (core func {}))", comment(index));
    }

    /// Whether a module names itself: its name section decodes and gives a module name, which only its
    /// raw `(@custom "name" …)` can say in component text.
    bool namesItself(isoline::binary::Module const& module)
    {
        return std::ranges::any_of(module.customs, [](isoline::binary::CustomSection const& custom) {
            if (custom.name != isoline::binary::NameSectionName)
                return false;
            auto const names = isoline::binary::decodeNameSection(custom.bytes);
            return names && names->module.has_value();
        });
    }

    /// One scope being written: its pool entry, the next definition, and the indices handed out.
    struct Frame
    {
        uint32_t scope = 0;                                  ///< Its pool entry.
        std::size_t next = 0;                                ///< Its next definition.
        std::array<uint32_t, binary::SortCount> counters {}; ///< The indices of each sort so far.
        std::string closing;                                 ///< What ends it.
        uint32_t level = 0;                                  ///< The indentation level of its first line.
    };

    /// The text written so far, within a limit.
    class Output
    {
      public:
        Output(isoline::text::WriterOptions const& options) noexcept: _options { options } {}

        /// Appends one line at @p level.
        void line(uint32_t level, std::string_view content)
        {
            if (_isTooLarge)
                return;
            auto const indent = std::min(level, _options.maxIndentLevel) * _options.indentWidth;
            append(std::string(indent, ' '));
            append(content);
            append("\n");
        }

        /// Appends text as it is.
        void append(std::string_view content)
        {
            if (_isTooLarge || _text.size() + content.size() > _options.maxTextBytes)
            {
                _isTooLarge = true;
                return;
            }
            _text += content;
        }

        [[nodiscard]] bool tooLarge() const noexcept { return _isTooLarge; }
        [[nodiscard]] std::string take() noexcept { return std::move(_text); }

      private:
        isoline::text::WriterOptions const& _options;
        std::string _text;
        bool _isTooLarge = false;
    };
    /// What a definition writes: its line, as text, or nothing when it wrote lines itself.
    using DefinitionText = std::expected<std::string, ComponentWriteError>;

    /// Writes one definition of a scope: its text, and the scope it opens, if any.
    class DefinitionWriter
    {
      public:
        DefinitionWriter(isoline::text::WriterOptions const& options,
                         FeatureSet coreFeatures,
                         Output& output,
                         std::span<std::byte const> bytes,
                         uint32_t& ordinal,
                         uint32_t level,
                         uint32_t index) noexcept:
            _options { options },
            _coreFeatures { coreFeatures },
            _output { output },
            _bytes { bytes },
            _ordinal { ordinal },
            _level { level },
            _index { index },
            _id { comment(index) }
        {
        }

        /// The scope the definition opened; none for one that opens none.
        [[nodiscard]] std::optional<Frame>& opened() noexcept { return _opened; }

        DefinitionText operator()(binary::CoreModuleDefinition const& module) const
        {
            auto decoded =
                isoline::binary::decodeModule(_bytes.subspan(module.offset, module.size), _coreFeatures);
            if (!decoded)
                return std::unexpected { CoreModuleMalformed { .ordinal = _ordinal,
                                                               .error = decoded.error() } };
            ++_ordinal;
            auto options = _options;
            if (namesItself(*decoded))
                options.nameSection = isoline::text::NameSectionForm::Custom;
            auto written = isoline::text::ModuleWriter { options }.write(*decoded, _level);
            if (!written)
                return std::unexpected { written.error() };
            auto const head = written->find("(module");
            written->replace(head, std::string_view { "(module" }.size(), "(core module " + _id);
            _output.append(*written);
            return std::string {};
        }

        DefinitionText operator()(binary::CoreInstanceDefinition const& instance) const
        {
            if (auto const* instantiate = std::get_if<binary::CoreInstantiate>(&instance.expression))
            {
                auto text = std::format("(core instance {} (instantiate {}", _id, instantiate->module);
                for (auto const& argument: instantiate->arguments)
                    text += std::format(" (with {} (instance {}))", quoted(argument.name), argument.instance);
                return text + "))";
            }
            auto text = std::format("(core instance {}", _id);
            for (auto const& entry: std::get<binary::CoreInlineExports>(instance.expression).exports)
                text += std::format(" (export {} ({} {}))",
                                    quoted(entry.name),
                                    sortText(binary::sortOf(entry.sort)).keyword,
                                    entry.index);
            return text + ")";
        }

        DefinitionText operator()(binary::CoreTypeDefinition const& type) const
        {
            if (auto const* function = std::get_if<isoline::binary::FuncType>(&type.type))
                return std::format("(core type {} {})", _id, coreFunctionTypeText(*function));
            _output.line(_level, std::format("(core type {} (module", _id));
            auto types = uint32_t { 0 };
            for (auto const& declaration: std::get<binary::CoreModuleTypeDefinition>(type.type).declarations)
                _output.line(_level + 1, moduleDeclarationText(declaration, types));
            _output.line(_level, "))");
            return std::string {};
        }

        DefinitionText operator()(binary::NestedComponent const& component)
        {
            _output.line(_level, std::format("(component {}", _id));
            _opened = Frame {
                .scope = component.index, .next = 0, .counters = {}, .closing = ")", .level = _level
            };
            return std::string {};
        }

        DefinitionText operator()(binary::InstanceDefinition const& instance) const
        {
            if (auto const* instantiate = std::get_if<binary::Instantiate>(&instance.expression))
            {
                auto text = std::format("(instance {} (instantiate {}", _id, instantiate->component);
                for (auto const& argument: instantiate->arguments)
                    text += std::format(" (with {} ({} {}))",
                                        quoted(argument.name),
                                        sortKeyword(argument.item.sort),
                                        argument.item.index);
                return text + "))";
            }
            auto text = std::format("(instance {}", _id);
            for (auto const& entry: std::get<binary::InlineExports>(instance.expression).exports)
                text += std::format(" (export {} ({} {}))",
                                    nameText(entry.name),
                                    sortKeyword(entry.item.sort),
                                    entry.item.index);
            return text + ")";
        }

        DefinitionText operator()(binary::AliasDefinition const& alias) const
        {
            auto const kind = std::format("({} {})", sortKeyword(alias.sort), _id);
            switch (alias.target)
            {
                case binary::AliasTarget::Export:
                    return std::format("(alias export {} {} {})", alias.instance, quoted(alias.name), kind);
                case binary::AliasTarget::CoreExport:
                    return std::format(
                        "(alias core export {} {} {})", alias.instance, quoted(alias.name), kind);
                case binary::AliasTarget::Outer: break;
            }
            return std::format("(alias outer {} {} {})", alias.outerCount, alias.outerIndex, kind);
        }

        DefinitionText operator()(binary::TypeDefinition const& definition)
        {
            return std::visit([this](auto const& type) { return typeText(type); }, definition.type);
        }

        DefinitionText operator()(binary::CanonDefinition const& canon) const
        {
            return canonText(canon, _index);
        }

        DefinitionText operator()(binary::ImportDefinition const& import) const
        {
            return std::format("(import {} {})", nameText(import.name), externDescText(import.type, _index));
        }

        DefinitionText operator()(binary::ExportDefinition const& exported) const
        {
            auto text = std::format("(export {} {} ({} {})",
                                    _id,
                                    nameText(exported.name),
                                    sortKeyword(exported.item.sort),
                                    exported.item.index);
            if (exported.type)
                text += " " + externDescText(*exported.type, std::nullopt);
            return text + ")";
        }

        DefinitionText operator()(binary::ExportDeclaration const& declaration) const
        {
            return std::format(
                "(export {} {})", nameText(declaration.name), externDescText(declaration.type, _index));
        }

        DefinitionText operator()(binary::CustomDefinition const& custom) const
        {
            auto const payload = _bytes.subspan(custom.offset, custom.size);
            return std::format(
                "(@custom {} {})",
                quoted(custom.name),
                quoted(std::string_view { reinterpret_cast<char const*>(payload.data()), payload.size() }));
        }

      private:
        [[nodiscard]] std::string typeText(binary::DefinedValueType const& type) const
        {
            return std::format("(type {} {})", _id, definedTypeText(type));
        }

        [[nodiscard]] std::string typeText(binary::FunctionTypeDefinition const& type) const
        {
            return std::format("(type {} {})", _id, functionTypeText(type));
        }

        [[nodiscard]] std::string typeText(binary::ResourceTypeDefinition const& type) const
        {
            auto text = std::format("(type {} (resource (rep {})",
                                    _id,
                                    CoreValueKeywords[static_cast<std::size_t>(type.rep)].keyword);
            if (type.destructor)
                text += std::format(" (dtor (core func {}))", *type.destructor);
            return text + "))";
        }

        std::string typeText(binary::ComponentTypeDefinition const& type)
        {
            return openType("component", type.scope);
        }

        std::string typeText(binary::InstanceTypeDefinition const& type)
        {
            return openType("instance", type.scope);
        }

        /// Opens a component or instance type, its declarations a scope of the pool.
        std::string openType(std::string_view keyword, uint32_t scope)
        {
            _output.line(_level, std::format("(type {} ({}", _id, keyword));
            _opened = Frame { .scope = scope, .next = 0, .counters = {}, .closing = "))", .level = _level };
            return {};
        }

        isoline::text::WriterOptions const& _options; ///< The layout and the output limit.
        FeatureSet _coreFeatures;                     ///< What core modules decode under.
        Output& _output;                              ///< Where lines of their own go.
        std::span<std::byte const> _bytes;            ///< The pool's bytes.
        uint32_t& _ordinal;                           ///< The next core module's ordinal.
        uint32_t _level;                              ///< The definition's indentation level.
        uint32_t _index;                              ///< Its index in its sort.
        std::string _id;                              ///< Its `(;n;)` comment.
        std::optional<Frame> _opened;                 ///< The scope it opens, if any.
    };
} // namespace

std::expected<std::string, ComponentWriteError> ComponentWriter::write(
    binary::ComponentPool const& pool) const
{
    auto output = Output { _options };
    auto const bytes = std::span<std::byte const> { *pool.bytes };
    auto ordinal = uint32_t { 0 };
    auto stack = std::vector<Frame> {};
    output.line(0, "(component");
    stack.push_back(Frame { .scope = 0, .next = 0, .counters = {}, .closing = ")", .level = 0 });
    while (!stack.empty() && !output.tooLarge())
    {
        auto& frame = stack.back();
        auto const& definitions = pool.components[frame.scope].definitions;
        if (frame.next == definitions.size())
        {
            output.line(frame.level, frame.closing);
            stack.pop_back();
            continue;
        }
        auto const& definition = definitions[frame.next++];
        auto const sort = binary::introducedSort(definition);
        auto const index = sort ? frame.counters[static_cast<std::size_t>(*sort)]++ : 0;
        auto const level = frame.level + 1;
        auto writer = DefinitionWriter { _options, _coreFeatures, output, bytes, ordinal, level, index };
        auto const text = std::visit(writer, definition);
        if (!text)
            return std::unexpected { text.error() };
        if (!text->empty())
            output.line(level, *text);
        if (auto& opened = writer.opened())
            stack.push_back(std::move(*opened));
    }
    if (output.tooLarge())
        return std::unexpected { isoline::text::WriteError::TextTooLarge };
    return output.take();
}

} // namespace isoline::component::text
