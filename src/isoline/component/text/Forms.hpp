// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/component/Features.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Encoder.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

// The component text format's vocabulary, which ComponentReader reads and ComponentWriter writes from
// the same rows, and the top-level form that compiles a `(component …)` text (D13).
namespace isoline::component::text
{

/// Every gated feature on: what component text is read under, the reader keeping no gate of its own.
/// @return The set of every ComponentFeature.
[[nodiscard]] constexpr ComponentFeatureSet everyComponentFeature() noexcept
{
    auto features = ComponentFeatureSet::defaults();
    for (auto const& row: ComponentFeatureTable)
        features = features.with(row.feature);
    return features;
}

/// Compiles a `(component …)` node to a component binary: ComponentReader::read, under every gated
/// feature and ComponentLimits' defaults, then binary::encodeComponent.
///
/// `(component binary "…"*)` compiles to its bytes verbatim, as wasm-tools compiles it, leaving the
/// decoder to report what is wrong with them. The component's bytes, its embedded core modules
/// together, are capped by TextLimits::maxModuleBytes, the cap compileText() puts on what it produces.
/// @param tree The tree; its source must outlive the call.
/// @param form The `(component …)` list.
/// @param limits The caps on reading text.
/// @return The component's bytes, or the first error; OutputTooLarge past TextLimits::maxModuleBytes.
[[nodiscard]] std::expected<std::vector<std::byte>, isoline::text::TextError> compileComponent(
    isoline::text::SExpressionTree const& tree,
    isoline::text::NodeId form,
    isoline::text::TextLimits const& limits);

/// The forms a text may hold when the component model is linked: isoline::text::CoreForms with the
/// component row read rather than refused (D13).
inline constexpr auto Forms = std::to_array<isoline::text::TopLevelForm>({
    { .keyword = "module", .read = &isoline::text::compileCoreModule },
    { .keyword = "component", .read = &compileComponent },
});

/// P10's canonical form of a core module: isoline::binary::canonicalize under every core feature, which
/// canonicalizeComponent() applies to each embedded module. It is idempotent, so canonicalizing a
/// canonical component returns it unchanged.
class P10CoreModules final: public binary::ICoreModuleCanonicalizer
{
  public:
    /// The canonical bytes of one embedded core module.
    /// @param module The module's bytes, from its preamble on.
    /// @return Its canonical bytes, or the core decoder's message for a module that does not decode.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> canonicalize(
        std::span<std::byte const> module) override;
};

/// How a sort's keyword is spelled: alone, or after `core`.
enum class SortPrefix : uint8_t
{
    None, ///< `func`, `type`, …
    Core, ///< `core func`, `core module`, …
};

/// One row of SortTexts: how text names a sort, and how the `component-name` section encodes it.
struct SortTextRow
{
    binary::Sort sort;                   ///< The sort, at its enumerator's index.
    SortPrefix prefix;                   ///< Whether its keyword follows `core`.
    std::string_view keyword;            ///< Its keyword: `func` for both func and core func.
    std::string_view category;           ///< What messages call one of it: "core func".
    uint8_t nameByte;                    ///< Its `sort` byte in a name map's subsection.
    std::optional<uint8_t> coreNameByte; ///< The core sort byte after nameByte 0x00, for a core sort.
};

/// Every sort, in binary::Sort's order: the text format's keywords (Explainer.md), and the `sort`
/// bytes of the `component-name` section as wasm-encoder 0.263.0 writes them (component/names.rs).
inline constexpr auto SortTexts = std::to_array<SortTextRow>({
    { .sort = binary::Sort::CoreFunc,
      .prefix = SortPrefix::Core,
      .keyword = "func",
      .category = "core func",
      .nameByte = 0x00,
      .coreNameByte = 0x00 },
    { .sort = binary::Sort::CoreTable,
      .prefix = SortPrefix::Core,
      .keyword = "table",
      .category = "core table",
      .nameByte = 0x00,
      .coreNameByte = 0x01 },
    { .sort = binary::Sort::CoreMemory,
      .prefix = SortPrefix::Core,
      .keyword = "memory",
      .category = "core memory",
      .nameByte = 0x00,
      .coreNameByte = 0x02 },
    { .sort = binary::Sort::CoreGlobal,
      .prefix = SortPrefix::Core,
      .keyword = "global",
      .category = "core global",
      .nameByte = 0x00,
      .coreNameByte = 0x03 },
    { .sort = binary::Sort::CoreTag,
      .prefix = SortPrefix::Core,
      .keyword = "tag",
      .category = "core tag",
      .nameByte = 0x00,
      .coreNameByte = 0x04 },
    { .sort = binary::Sort::CoreType,
      .prefix = SortPrefix::Core,
      .keyword = "type",
      .category = "core type",
      .nameByte = 0x00,
      .coreNameByte = 0x10 },
    { .sort = binary::Sort::CoreModule,
      .prefix = SortPrefix::Core,
      .keyword = "module",
      .category = "core module",
      .nameByte = 0x00,
      .coreNameByte = 0x11 },
    { .sort = binary::Sort::CoreInstance,
      .prefix = SortPrefix::Core,
      .keyword = "instance",
      .category = "core instance",
      .nameByte = 0x00,
      .coreNameByte = 0x12 },
    { .sort = binary::Sort::Func,
      .prefix = SortPrefix::None,
      .keyword = "func",
      .category = "func",
      .nameByte = 0x01,
      .coreNameByte = std::nullopt },
    { .sort = binary::Sort::Value,
      .prefix = SortPrefix::None,
      .keyword = "value",
      .category = "value",
      .nameByte = 0x02,
      .coreNameByte = std::nullopt },
    { .sort = binary::Sort::Type,
      .prefix = SortPrefix::None,
      .keyword = "type",
      .category = "type",
      .nameByte = 0x03,
      .coreNameByte = std::nullopt },
    { .sort = binary::Sort::Component,
      .prefix = SortPrefix::None,
      .keyword = "component",
      .category = "component",
      .nameByte = 0x04,
      .coreNameByte = std::nullopt },
    { .sort = binary::Sort::Instance,
      .prefix = SortPrefix::None,
      .keyword = "instance",
      .category = "instance",
      .nameByte = 0x05,
      .coreNameByte = std::nullopt },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: SortTexts)
        {
            if (static_cast<std::size_t>(row.sort) != index
                || (row.prefix == SortPrefix::Core) != row.coreNameByte.has_value())
                return false;
            ++index;
        }
        return index == binary::SortCount;
    }(),
    "SortTexts must hold one row per Sort, at the index of its enumerator, core sorts with a core byte");

/// The row of SortTexts that describes @p sort.
/// @param sort A sort.
/// @return Its row.
[[nodiscard]] constexpr SortTextRow const& sortText(binary::Sort sort) noexcept
{
    return SortTexts[static_cast<std::size_t>(sort)];
}

/// How one index immediate of a canonical built-in is written in text, and the index space it names.
enum class IndexSyntax : uint8_t
{
    None,      ///< No immediate: the padding after a row's last index.
    Type,      ///< `t` or `(type t)`: a type, as resource.new names its resource.
    Func,      ///< `(func f)`: a component function, as lower takes it.
    CoreFunc,  ///< `(core func f)`: a core function, as lift takes it.
    CoreType,  ///< `t` or `(core type t)`: a core function type.
    CoreTable, ///< `t` or `(core table t)`: a core table.
    Memory,    ///< `(memory m)` or `(memory (core memory m))`: a core memory.
    Slot,      ///< A plain `u32`: context.get's slot, an index of no index space.
    LiftType,  ///< `(func $id? (type t))` after a lift's options: the type of the function it defines.
};

/// One row of IndexSyntaxSpaces: an IndexSyntax and the index space it names.
struct IndexSyntaxRow
{
    IndexSyntax syntax;               ///< The syntax, at its enumerator's index.
    std::optional<binary::Sort> sort; ///< The index space it names; none for None and Slot.
};

/// Every IndexSyntax, in its order, with the index space it names.
inline constexpr auto IndexSyntaxSpaces = std::to_array<IndexSyntaxRow>({
    { .syntax = IndexSyntax::None, .sort = std::nullopt },
    { .syntax = IndexSyntax::Type, .sort = binary::Sort::Type },
    { .syntax = IndexSyntax::Func, .sort = binary::Sort::Func },
    { .syntax = IndexSyntax::CoreFunc, .sort = binary::Sort::CoreFunc },
    { .syntax = IndexSyntax::CoreType, .sort = binary::Sort::CoreType },
    { .syntax = IndexSyntax::CoreTable, .sort = binary::Sort::CoreTable },
    { .syntax = IndexSyntax::Memory, .sort = binary::Sort::CoreMemory },
    { .syntax = IndexSyntax::Slot, .sort = std::nullopt },
    { .syntax = IndexSyntax::LiftType, .sort = binary::Sort::Type },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: IndexSyntaxSpaces)
            if (static_cast<std::size_t>(row.syntax) != index++)
                return false;
        return index == static_cast<std::size_t>(IndexSyntax::LiftType) + 1;
    }(),
    "IndexSyntaxSpaces must hold one row per IndexSyntax, at the index of its enumerator");

/// The syntaxes of a built-in's index immediates, in the order of binary::Immediate::Index in its shape,
/// padded with IndexSyntax::None.
using IndexSyntaxes = std::array<IndexSyntax, 3>;

/// One row of CanonTextTable: how a built-in's index immediates are written. Every other immediate has
/// one syntax: a vector of options, `(result t)?`, a core value type, `async?` and `shared?`.
struct CanonTextRow
{
    binary::CanonBuiltin builtin; ///< The built-in, at its enumerator's index.
    IndexSyntaxes indices;        ///< Its index immediates, in shape order.
};

/// Every canonical built-in's index syntax, in binary::CanonBuiltin's order (Explainer.md's canon forms,
/// as wast 263 reads them). The text name of each is its binary::CanonBuiltinTable row's.
inline constexpr auto CanonTextTable = std::to_array<CanonTextRow>({
    { .builtin = binary::CanonBuiltin::Lift, .indices = { IndexSyntax::CoreFunc, IndexSyntax::LiftType } },
    { .builtin = binary::CanonBuiltin::Lower, .indices = { IndexSyntax::Func } },
    { .builtin = binary::CanonBuiltin::ResourceNew, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::ResourceDrop, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::ResourceRep, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::BackpressureInc, .indices = {} },
    { .builtin = binary::CanonBuiltin::BackpressureDec, .indices = {} },
    { .builtin = binary::CanonBuiltin::TaskReturn, .indices = {} },
    { .builtin = binary::CanonBuiltin::TaskCancel, .indices = {} },
    { .builtin = binary::CanonBuiltin::ContextGet, .indices = { IndexSyntax::Slot } },
    { .builtin = binary::CanonBuiltin::ContextSet, .indices = { IndexSyntax::Slot } },
    { .builtin = binary::CanonBuiltin::SubtaskCancel, .indices = {} },
    { .builtin = binary::CanonBuiltin::SubtaskDrop, .indices = {} },
    { .builtin = binary::CanonBuiltin::StreamNew, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::StreamRead, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::StreamWrite, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::StreamCancelRead, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::StreamCancelWrite, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::StreamDropReadable, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::StreamDropWritable, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::StreamForward, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::FutureNew, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::FutureRead, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::FutureWrite, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::FutureCancelRead, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::FutureCancelWrite, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::FutureDropReadable, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::FutureDropWritable, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::FutureForward, .indices = { IndexSyntax::Type } },
    { .builtin = binary::CanonBuiltin::ErrorContextNew, .indices = {} },
    { .builtin = binary::CanonBuiltin::ErrorContextDebugMessage, .indices = {} },
    { .builtin = binary::CanonBuiltin::ErrorContextDrop, .indices = {} },
    { .builtin = binary::CanonBuiltin::WaitableSetNew, .indices = {} },
    { .builtin = binary::CanonBuiltin::WaitableSetWait, .indices = { IndexSyntax::Memory } },
    { .builtin = binary::CanonBuiltin::WaitableSetPoll, .indices = { IndexSyntax::Memory } },
    { .builtin = binary::CanonBuiltin::WaitableSetDrop, .indices = {} },
    { .builtin = binary::CanonBuiltin::WaitableJoin, .indices = {} },
    { .builtin = binary::CanonBuiltin::ThreadIndex, .indices = {} },
    { .builtin = binary::CanonBuiltin::ThreadNewIndirect,
      .indices = { IndexSyntax::CoreType, IndexSyntax::CoreTable } },
    { .builtin = binary::CanonBuiltin::ThreadResumeLater, .indices = {} },
    { .builtin = binary::CanonBuiltin::ThreadSuspend, .indices = {} },
    { .builtin = binary::CanonBuiltin::ThreadYield, .indices = {} },
    { .builtin = binary::CanonBuiltin::ThreadSuspendThenResume, .indices = {} },
    { .builtin = binary::CanonBuiltin::ThreadYieldThenResume, .indices = {} },
    { .builtin = binary::CanonBuiltin::ThreadSuspendThenPromote, .indices = {} },
    { .builtin = binary::CanonBuiltin::ThreadYieldThenPromote, .indices = {} },
    { .builtin = binary::CanonBuiltin::ThreadSpawnRef, .indices = { IndexSyntax::CoreType } },
    { .builtin = binary::CanonBuiltin::ThreadSpawnIndirect,
      .indices = { IndexSyntax::CoreType, IndexSyntax::CoreTable } },
    { .builtin = binary::CanonBuiltin::ThreadAvailableParallelism, .indices = {} },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: CanonTextTable)
        {
            if (static_cast<std::size_t>(row.builtin) != index)
                return false;
            // As many index syntaxes as the built-in's shape has index immediates.
            auto const& shape =
                std::ranges::find(binary::CanonBuiltinTable, row.builtin, &binary::CanonBuiltinRow::builtin)
                    ->shape;
            auto const indices = std::ranges::count(shape, binary::Immediate::Index);
            auto const syntaxes = std::ranges::count_if(
                row.indices, [](IndexSyntax syntax) { return syntax != IndexSyntax::None; });
            if (indices != syntaxes)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(binary::LastCanonBuiltin) + 1;
    }(),
    "CanonTextTable must hold one row per CanonBuiltin, at its enumerator's index, one syntax per index");

/// The CanonTextTable row of @p builtin.
/// @param builtin A built-in.
/// @return Its row.
[[nodiscard]] constexpr CanonTextRow const& canonText(binary::CanonBuiltin builtin) noexcept
{
    return CanonTextTable[static_cast<std::size_t>(builtin)];
}

/// The index space a canonical option's index names, by binary::CanonOptionKind: a core memory for
/// `memory`, a core function for `realloc`, `post-return` and `callback`, none for the others.
inline constexpr auto CanonOptionSpaces = std::to_array<std::optional<binary::Sort>>({
    std::nullopt,
    std::nullopt,
    std::nullopt,
    binary::Sort::CoreMemory,
    binary::Sort::CoreFunc,
    binary::Sort::CoreFunc,
    std::nullopt,
    binary::Sort::CoreFunc,
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: binary::CanonOptionTable)
            if ((row.immediate == binary::OptionImmediate::Index) != CanonOptionSpaces[index++].has_value())
                return false;
        return index == CanonOptionSpaces.size();
    }(),
    "CanonOptionSpaces must name an index space for exactly the options that take an index");

/// One core value type's keyword, as context.get names its slot's type.
struct CoreValueKeyword
{
    std::string_view keyword;   ///< The keyword.
    binary::CoreValueKind kind; ///< The kind, at its enumerator's index.
};

/// The core value types a built-in names, in binary::CoreValueKind's order.
inline constexpr auto CoreValueKeywords = std::to_array<CoreValueKeyword>({
    { .keyword = "i32", .kind = binary::CoreValueKind::I32 },
    { .keyword = "i64", .kind = binary::CoreValueKind::I64 },
    { .keyword = "f32", .kind = binary::CoreValueKind::F32 },
    { .keyword = "f64", .kind = binary::CoreValueKind::F64 },
});

static_assert(CoreValueKeywords.size() == binary::CoreValueKindTable.size(),
              "CoreValueKeywords holds one keyword per CoreValueKind");

} // namespace isoline::component::text
