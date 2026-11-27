// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/Annotations.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/text/Lexer.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>

namespace isoline::text
{

namespace
{
    using binary::NameKind;
    using binary::NameShape;
    using binary::SectionAnchor;

    using Outcome = std::expected<void, TextError>;

    /// One word of an `@custom` placement that names a side.
    struct SideWord
    {
        std::string_view word; ///< The word.
        PlacementSide side;    ///< The side it names.
    };

    /// The words of a placement's side (handler_custom.ml's parse_direction).
    inline constexpr auto SideWords = std::to_array<SideWord>({
        { .word = "before", .side = PlacementSide::Before },
        { .word = "after", .side = PlacementSide::After },
    });

    /// The error a second name of one entity is refused with, by the shape of its subsection
    /// (handler_name.ml's merge functions); indexed by NameShape.
    inline constexpr auto MultipleNameErrors = std::to_array<TextErrorCode>({
        TextErrorCode::NameMultipleModule,
        TextErrorCode::NameMultipleFunctionNames,
        TextErrorCode::NameMultipleLocalNames,
    });

    static_assert(MultipleNameErrors.size() == std::to_underlying(NameShape::Indirect) + std::size_t { 1 },
                  "MultipleNameErrors holds one error per NameShape");

    /// What reading a module's annotations works on.
    struct Context
    {
        SExpressionTree const& tree;  ///< The tree.
        AnnotationSites const& sites; ///< Where the reader accepted annotations.
        binary::Module& module;       ///< The module.
        binary::NameSection& names;   ///< Its names.
    };

    /// An error at a node.
    [[nodiscard]] TextError errorAt(SExpressionTree const& tree, NodeId node, TextErrorCode code)
    {
        return TextError { code, tree.positionOf(node) };
    }

    /// Whether @p node is a String atom.
    [[nodiscard]] bool isString(SExpressionTree const& tree, NodeId node) noexcept
    {
        auto const& entry = tree.nodes()[node];
        return entry.kind == NodeKind::Atom && entry.tokenKind == TokenKind::String;
    }

    /// The bytes a String atom denotes.
    [[nodiscard]] std::string bytesOf(SExpressionTree const& tree, NodeId node)
    {
        auto bytes = decodeString(tree.source(), tree.tokenOf(node));
        Require(bytes.has_value()); // The lexer read it as a String token, so it decodes.
        return std::move(*bytes);
    }

    /// The row of @p words an atom spells.
    /// @return The row, or null when @p node is no atom or spells none.
    template <typename Row, std::size_t Count>
    [[nodiscard]] Row const* wordOf(SExpressionTree const& tree,
                                    NodeId node,
                                    std::array<Row, Count> const& words)
    {
        // Only an atom spells a word: a nested annotation's text is its id, which may spell one too. No
        // string or identifier does, as its text keeps its quotes or `$`.
        if (tree.nodes()[node].kind != NodeKind::Atom)
            return nullptr;
        auto const found = std::ranges::find(words, tree.textOf(node), &Row::word);
        return found == words.end() ? nullptr : &*found;
    }

    /// Reads a placement list: `(before|after section)`.
    [[nodiscard]] std::expected<CustomPlacement, TextError> readPlacement(SExpressionTree const& tree,
                                                                          NodeId list)
    {
        auto const items = tree.children(list);
        auto item = items.begin();
        auto const* side = item == items.end() ? nullptr : wordOf(tree, *item, SideWords);
        if (side == nullptr)
            return std::unexpected { errorAt(tree, list, TextErrorCode::CustomMalformedPlacement) };
        ++item;
        auto const* section = item == items.end() ? nullptr : wordOf(tree, *item, SectionWords);
        if (section == nullptr)
            return std::unexpected { errorAt(tree, list, TextErrorCode::CustomMalformedSectionKind) };
        if (++item != items.end())
            return std::unexpected { errorAt(tree, *item, TextErrorCode::CustomUnexpectedToken) };
        return CustomPlacement { .section = section->section, .side = side->side };
    }

    /// The anchor a placement resolves to in @p module.
    [[nodiscard]] SectionAnchor anchorOf(binary::Module const& module, CustomPlacement placement) noexcept
    {
        // Before a section is after the one before it in section order; before the first, before all.
        auto const anchor = placement.side == PlacementSide::After
                                ? placement.section
                                : static_cast<SectionAnchor>(std::to_underlying(placement.section) - 1);
        return binary::writtenAnchor(module, anchor);
    }

    /// Reads a name annotation: `(@name "name")`.
    [[nodiscard]] std::expected<std::string, TextError> readNameAnnotation(SExpressionTree const& tree,
                                                                           NodeId annotation)
    {
        auto const items = tree.children(annotation);
        auto item = items.begin();
        if (item == items.end() || !isString(tree, *item))
            return std::unexpected { errorAt(tree, annotation, TextErrorCode::NameStringExpected) };
        auto name = decodeName(tree.source(), tree.tokenOf(*item));
        if (!name)
            return std::unexpected { errorAt(tree, *item, TextErrorCode::NameMalformedUtf8) };
        if (++item != items.end())
            return std::unexpected { errorAt(tree, *item, TextErrorCode::NameUnexpectedToken) };
        return std::move(*name);
    }

    // ---------------------------------------------------------------------------------------------
    // One reader per AnnotationKind

    Outcome readCustoms(Context const& context, std::span<NodeId const> annotations)
    {
        auto customs = std::vector<CustomAnnotation> {};
        customs.reserve(annotations.size());
        // The reference's Annot.record prepends, and handler_custom.ml reads the list as it gets it: the
        // last annotation's errors come first.
        for (auto const annotation: annotations | std::views::reverse)
        {
            auto custom = readCustomAnnotation(context.tree, annotation);
            if (!custom)
                return std::unexpected { std::move(custom.error()) };
            // handler_custom.ml refuses one inside any module field: only a field may be one.
            if (!context.sites.moduleFields.contains(annotation))
                return std::unexpected { errorAt(context.tree, annotation, TextErrorCode::MisplacedCustom) };
            customs.push_back(std::move(*custom));
        }
        std::ranges::reverse(customs); // Back in source order, which placement keeps among equals.
        placeCustomSections(std::move(customs), context.module);
        return {};
    }

    Outcome readBranchHints(Context const& context, std::span<NodeId const> annotations)
    {
        auto& hints = context.module.branchHints;
        auto const first = hints.size();      // The hints the module held before these.
        auto hinted = std::vector<NodeId> {}; // The annotation of each hint from first on.
        hinted.reserve(annotations.size());
        hints.reserve(hints.size() + annotations.size());
        for (auto const annotation: annotations)
        {
            // handler_branch_hint.ml reads the payload, the function, the value and the instruction, in
            // that order.
            auto payload = std::string {};
            for (auto const item: context.tree.children(annotation))
            {
                if (!isString(context.tree, item))
                    return std::unexpected { errorAt(
                        context.tree, item, TextErrorCode::BranchHintUnexpectedToken) };
                payload += bytesOf(context.tree, item);
            }
            auto const site = context.sites.hints.find(annotation);
            if (site == context.sites.hints.end())
                return std::unexpected { errorAt(
                    context.tree, annotation, TextErrorCode::BranchHintNotInFunction) };
            // BranchLikelihood's enumerators are the hint bytes.
            if (payload.size() != 1
                || static_cast<uint8_t>(payload.front())
                       > std::to_underlying(binary::BranchLikelihood::Likely))
                return std::unexpected { errorAt(
                    context.tree, annotation, TextErrorCode::BranchHintInvalidValue) };
            if (!site->second.instruction)
                return std::unexpected { errorAt(
                    context.tree, annotation, TextErrorCode::BranchHintInvalidPlacement) };
            hints.push_back(
                binary::BranchHint { .function = site->second.function,
                                     .instruction = *site->second.instruction,
                                     .likelihood = static_cast<binary::BranchLikelihood>(payload.front()) });
            hinted.push_back(annotation);
        }
        // handler_branch_hint.ml reverses the list into source order. Within a function, each hint must
        // name an instruction after the one the hint before it names; a function's fields precede the
        // next one's, so the hints are in function order already.
        for (auto const index: std::views::iota(first + 1, std::max(hints.size(), first + 1)))
            if (hints[index].function == hints[index - 1].function
                && hints[index].instruction <= hints[index - 1].instruction)
                return std::unexpected { errorAt(
                    context.tree, hinted[index - first], TextErrorCode::BranchHintDuplicate) };
        return {};
    }

    /// The names the annotations give one subsection, by key: the index, or an indirect map's outer
    /// index in the high half and its inner one in the low half.
    using NameOverrides = std::map<uint64_t, std::string>;

    /// The key of a target in its subsection's NameOverrides.
    [[nodiscard]] constexpr uint64_t keyOf(NameTarget const& target) noexcept
    {
        return (uint64_t { target.index } << 32U) | target.inner;
    }

    /// Replaces the names of a direct name map's entities that @p overrides name, and adds the others.
    void overrideDirect(std::vector<binary::NameAssoc>& map, NameOverrides& overrides)
    {
        for (auto& entry: map)
            if (auto const found = overrides.find(keyOf({ .index = entry.index })); found != overrides.end())
            {
                entry.name = std::move(found->second);
                overrides.erase(found);
            }
        // encodeNameSection writes every map by ascending index, whatever order it is held in.
        for (auto& [key, name]: overrides)
            map.push_back(
                binary::NameAssoc { .index = static_cast<uint32_t>(key >> 32U), .name = std::move(name) });
    }

    /// Replaces the names of an indirect name map's inner entities that @p overrides name, and adds
    /// the others, with a map of their own for an outer entity that has none.
    void overrideIndirect(std::vector<binary::IndirectNameMap>& maps, NameOverrides& overrides)
    {
        auto positions = std::map<uint32_t, std::size_t> {}; // Each outer index's map.
        for (auto const position: std::views::iota(std::size_t { 0 }, maps.size()))
        {
            auto& map = maps[position];
            positions.emplace(map.index, position);
            for (auto& entry: map.names)
                if (auto const found = overrides.find(keyOf({ .index = map.index, .inner = entry.index }));
                    found != overrides.end())
                {
                    entry.name = std::move(found->second);
                    overrides.erase(found);
                }
        }
        for (auto& [key, name]: overrides)
        {
            auto const outer = static_cast<uint32_t>(key >> 32U);
            auto const [position, isNew] = positions.try_emplace(outer, maps.size());
            if (isNew)
                maps.push_back(binary::IndirectNameMap { .index = outer, .names = {} });
            maps[position->second].names.push_back(
                binary::NameAssoc { .index = static_cast<uint32_t>(key), .name = std::move(name) });
        }
    }

    Outcome readNames(Context const& context, std::span<NodeId const> annotations)
    {
        struct Named
        {
            NodeId annotation; ///< The annotation.
            NameTarget target; ///< What it names.
            std::string name;  ///< The name it gives.
        };
        auto named = std::vector<Named> {};
        named.reserve(annotations.size());
        // As handler_custom.ml's, handler_name.ml's list is the reverse of source order, and its merge
        // folds over it so: the earlier of two names of one entity is the one reported.
        for (auto const annotation: annotations | std::views::reverse)
        {
            auto name = readNameAnnotation(context.tree, annotation);
            if (!name)
                return std::unexpected { std::move(name.error()) };
            auto const site = context.sites.names.find(annotation);
            if (site == context.sites.names.end())
                return std::unexpected { errorAt(context.tree, annotation, TextErrorCode::MisplacedName) };
            named.push_back(
                Named { .annotation = annotation, .target = site->second, .name = std::move(*name) });
        }

        auto overrides = std::array<NameOverrides, binary::NameKindCount> {};
        for (auto& [annotation, target, name]: named)
        {
            auto const kind = std::to_underlying(target.kind);
            if (!overrides[kind].try_emplace(keyOf(target), std::move(name)).second)
                return std::unexpected { errorAt(
                    context.tree,
                    annotation,
                    MultipleNameErrors[std::to_underlying(binary::NameSubsections[kind].shape)]) };
        }
        for (auto const& row: binary::NameSubsections)
        {
            auto& kindOverrides = overrides[std::to_underlying(row.kind)];
            if (kindOverrides.empty())
                continue;
            switch (row.shape)
            {
                case NameShape::Single:
                    context.names.module = std::move(kindOverrides.begin()->second);
                    break;
                case NameShape::Direct:
                    overrideDirect(context.names.direct[std::to_underlying(row.kind)], kindOverrides);
                    break;
                case NameShape::Indirect:
                    overrideIndirect(context.names.indirect[std::to_underlying(row.kind)], kindOverrides);
                    break;
            }
        }
        return {};
    }

    /// One AnnotationKind's reader.
    using AnnotationReader = Outcome (*)(Context const& context, std::span<NodeId const> annotations);

    /// The reader of each AnnotationKind, indexed by it.
    inline constexpr auto AnnotationReaders = std::to_array<AnnotationReader>({
        &readCustoms,
        &readBranchHints,
        &readNames,
    });

    static_assert(AnnotationReaders.size() == KnownAnnotations.size(),
                  "AnnotationReaders holds one reader per AnnotationKind");
} // namespace

std::expected<CustomAnnotation, TextError> readCustomAnnotation(SExpressionTree const& tree,
                                                                NodeId annotation)
{
    // handler_custom.ml: the name, an optional placement, the contents, and nothing else.
    auto const items = tree.children(annotation);
    auto item = items.begin();
    if (item == items.end() || !isString(tree, *item))
        return std::unexpected { errorAt(tree, annotation, TextErrorCode::CustomMissingName) };
    auto name = decodeName(tree.source(), tree.tokenOf(*item));
    if (!name)
        return std::unexpected { errorAt(tree, *item, TextErrorCode::CustomMalformedUtf8) };
    auto custom = CustomAnnotation { .section = binary::CustomSection { .name = std::move(*name),
                                                                        .bytes = {},
                                                                        .after = SectionAnchor::Data },
                                     .placement = CustomPlacement {} };
    ++item;
    if (item != items.end() && tree.nodes()[*item].kind == NodeKind::List)
    {
        auto const placement = readPlacement(tree, *item);
        if (!placement)
            return std::unexpected { placement.error() };
        custom.placement = *placement;
        ++item;
    }
    auto const contents =
        std::ranges::subrange { item, std::ranges::find_if_not(item, items.end(), [&tree](NodeId node) {
                                    return isString(tree, node);
                                }) };
    for (auto const string: contents)
    {
        auto const bytes = bytesOf(tree, string);
        std::ranges::transform(
            bytes, std::back_inserter(custom.section.bytes), [](char byte) { return std::byte(byte); });
    }
    item = contents.end();
    if (item != items.end())
        return std::unexpected { errorAt(tree, *item, TextErrorCode::CustomUnexpectedToken) };
    return custom;
}

void placeCustomSections(std::vector<CustomAnnotation> customs, binary::Module& module)
{
    // The reference sorts sections stably by placement (custom.ml's compare_section), so equal ones keep
    // source order; the anchors then rise with it.
    std::ranges::stable_sort(customs, {}, &CustomAnnotation::placement);
    module.customs.reserve(module.customs.size() + customs.size());
    for (auto& custom: customs)
    {
        custom.section.after = anchorOf(module, custom.placement);
        module.customs.push_back(std::move(custom.section));
    }
}

std::vector<NodeId> knownAnnotationsIn(SExpressionTree const& tree, NodeId first, NodeId end)
{
    auto known = std::vector<NodeId> {};
    auto node = first;
    while (node < end)
    {
        if (tree.nodes()[node].kind != NodeKind::Annotation)
        {
            ++node;
            continue;
        }
        if (tree.annotationKindOf(node))
            known.push_back(node);
        node = tree.subtreeEnd(node);
    }
    return known;
}

std::expected<void, TextError> readAnnotations(SExpressionTree const& tree,
                                               std::span<NodeId const> annotations,
                                               AnnotationSites const& sites,
                                               binary::Module& module,
                                               binary::NameSection& names)
{
    auto const context = Context { .tree = tree, .sites = sites, .module = module, .names = names };
    auto ofKind = std::vector<NodeId> {};
    for (auto const& row: KnownAnnotations)
    {
        ofKind.clear();
        std::ranges::copy_if(annotations, std::back_inserter(ofKind), [&](NodeId annotation) {
            return tree.annotationKindOf(annotation) == row.kind;
        });
        if (auto const read = AnnotationReaders[std::to_underlying(row.kind)](context, ofKind); !read)
            return read;
    }
    return {};
}

} // namespace isoline::text
