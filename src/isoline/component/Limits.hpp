// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace isoline::component
{

/// The caps the component layer enforces on everything a component may make it hold (spec §7):
/// exceeding one is a refusal at load or a trap at run time, never host memory exhaustion. Each
/// field is one row of LimitTable, which names, describes and words it.
struct ComponentLimits
{
    uint64_t maxComponentBytes = uint64_t { 256 } << 20; ///< Bytes of one encoded component.
    /// Scopes open at once: components, component and instance types, nested scopes (binary and text).
    uint32_t maxNesting = 100;
    uint32_t maxTypeDepth = 100; ///< Nesting depth of one type, as wasm-tools measures it.
    /// Effective type size of one type, and of the types aliased into one component from outside it
    /// (wasm-tools' rules); refused when reached.
    uint32_t maxTypeSize = 1'000'000;
    uint32_t maxTypes = 1'000'000;       ///< Per component, all index spaces of sort type.
    uint32_t maxDefinitions = 1'000'000; ///< Per component, all sections.
    /// Per decoded component, all nested components together: definitions, nested components and the
    /// elements of every vector inside a definition. Bounds what the pool holds whatever the input's
    /// shape: at most about maxDecodedItems x the largest item's size x 2 (vector growth), plus the
    /// copy of the input (at most maxComponentBytes) and the names copied out of it (at most as many
    /// bytes again). The largest item is a nested component, a Definition and a Component pool entry:
    /// 88 + 24 = 112 bytes with libstdc++ on x86-64, so the defaults bound the pool at about
    /// 1'000'000 x 112 x 2 = 224 MB beyond 2 x 256 MiB. The sizes differ with libc++ and the MSVC STL.
    uint32_t maxDecodedItems = 1'000'000;
    uint32_t maxNameBytes = 100'000;            ///< Bytes of one name.
    uint32_t maxCoreInstances = 10'000;         ///< Per ComponentStore.
    uint32_t maxComponentInstances = 10'000;    ///< Per ComponentStore.
    uint32_t maxInstantiationSteps = 1'000'000; ///< Plan steps executed by one instantiate().
    /// Runtime resource types per ComponentStore (generative); also the resource types one validation
    /// mints.
    uint32_t maxResourceTypes = 100'000;
    /// Per ComponentStore: the slots of all handle tables together (a freed slot stays its table's, for
    /// reuse), plus the handles lent to calls in progress; each table also hands out indices up to
    /// 2^28 - 1.
    uint32_t maxHandles = 1'000'000;
    /// Per ComponentStore: the host bytes of every live Values.
    uint64_t maxLiveValueBytes = uint64_t { 256 } << 20;
    /// Steps of one validate::TypeRelation: one per pair of types it compares and per name it looks
    /// up or compares, plus one per validate::SubtypingBytesPerStep bytes of the names compared. Its memo
    /// holds at most one entry per step, so this also bounds the memo: at most maxSubtypingSteps entries.
    uint32_t maxSubtypingSteps = 1'000'000;
    /// Per TypeArena: the units its resource substitutions charge, one per type they create plus one per
    /// type it names (each instantiation, and each import or export of an instance type that defines
    /// resources, rewrites the types that name them; names are shared, never copied).
    uint32_t maxSubstitutedTypes = 1'000'000;
    /// Per component, its core module index space (wasm-tools' MAX_WASM_MODULES).
    uint32_t maxModules = 1'000;
    /// Per component, its component index space (MAX_WASM_COMPONENTS).
    uint32_t maxComponents = 1'000;
    /// Per component, its core and component instance index spaces together (MAX_WASM_INSTANCES).
    uint32_t maxInstances = 4'096;
    /// Per component, its core and component function index spaces together (MAX_WASM_FUNCTIONS).
    uint32_t maxFunctions = 1'000'000;
    /// Per component, its exports (MAX_WASM_EXPORTS).
    uint32_t maxExports = 1'000'000;
    /// Per component, its core table index space (MAX_CORE_INDEX_SPACE_ITEMS).
    uint32_t maxCoreTables = 1'000'000;
    /// Per component, its core memory index space (MAX_CORE_INDEX_SPACE_ITEMS).
    uint32_t maxCoreMemories = 1'000'000;
    /// Per component, its core global index space (MAX_CORE_INDEX_SPACE_ITEMS).
    uint32_t maxCoreGlobals = 1'000'000;
    /// Per component, its core tag index space (MAX_CORE_INDEX_SPACE_ITEMS).
    uint32_t maxCoreTags = 1'000'000;
    /// Per validation, all core instantiations together: one step per import looked up and matched,
    /// and per export or inline export measured, plus one per 64 bytes of the names its lookups
    /// compare (a name's length times the depth of the search). wasm-tools bounds none of it; a crafted
    /// component of 4'096 instantiations of a module of 100'000 imports would take 4 x 10^8 lookups. Ten
    /// million covers a thousand instantiations of ten thousand imports, far past what toolchains emit.
    uint32_t maxCoreMatchingSteps = 10'000'000;
    /// Per ComponentStore: the invocations in progress at once in its core store, the outermost call and
    /// every hop a lowered import makes back into a guest; the core store nests at most the lower of this
    /// and the engine's maxNestedInvocations (100 by default), so it only ever tightens the engine's. Each
    /// hop takes host stack (docs/embedding.md measures it: 4.6 KB with clang, 5.4 KB with MSVC, release
    /// builds), so 80 hops fit a 1 MiB thread stack with a 2x margin.
    uint32_t maxNestedCalls = 80;
    /// Per ComponentStore: the owns the host holds (R12), each lifted to the host or made by
    /// hostResource(); their table is charged to maxLiveValueBytes too.
    uint32_t maxHostOwns = 1'000'000;

    /// Two sets of limits are equal when every limit is.
    bool operator==(ComponentLimits const&) const = default;
};

/// One cap of ComponentLimits, by name. The enumerators are the rows of LimitTable, in the same
/// order; adding a cap is adding a field there, an enumerator here and a row to the table.
enum class ComponentLimit : uint8_t
{
    ComponentBytes,
    Nesting,
    TypeDepth,
    TypeSize,
    Types,
    Definitions,
    DecodedItems,
    NameBytes,
    CoreInstances,
    ComponentInstances,
    InstantiationSteps,
    ResourceTypes,
    Handles,
    LiveValueBytes,
    SubtypingSteps,
    SubstitutedTypes,
    Modules,
    Components,
    Instances,
    Functions,
    Exports,
    CoreTables,
    CoreMemories,
    CoreGlobals,
    CoreTags,
    CoreMatchingSteps,
    NestedCalls,
    HostOwns,
};

/// The last ComponentLimit enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastComponentLimit = ComponentLimit::HostOwns;

/// The number of ComponentLimit enumerators, as LastComponentLimit counts them.
inline constexpr auto ComponentLimitCount = static_cast<std::size_t>(LastComponentLimit) + 1;

/// One row of LimitTable: a cap, what it counts, and how its refusal reads.
struct ComponentLimitRow
{
    ComponentLimit limit;  ///< The cap this row describes; equal to the row's index.
    std::string_view name; ///< Its ComponentLimits field.
    std::string_view rule; ///< What it counts, and where it is enforced.
    /// The refusal's text, a std::format string of the cap's value; wasm-tools' own where it has one.
    std::string_view message;
    uint64_t (*value)(ComponentLimits const&) noexcept; ///< Reads the cap from a ComponentLimits.
};

/// Every cap, one row per ComponentLimit enumerator, in enumerator order.
inline constexpr auto LimitTable = std::to_array<ComponentLimitRow>({
    { .limit = ComponentLimit::ComponentBytes,
      .name = "maxComponentBytes",
      .rule = "bytes of one encoded component, refused before the decoder copies it",
      .message = "component size exceeds the limit of {} bytes",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxComponentBytes; } },
    { .limit = ComponentLimit::Nesting,
      .name = "maxNesting",
      .rule = "scopes open at once, the outermost component counted: nested components, component and "
              "instance types, nested scopes, in the binary and the text format",
      .message = "nesting exceeds the limit of {} levels",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxNesting; } },
    { .limit = ComponentLimit::TypeDepth,
      .name = "maxTypeDepth",
      .rule = "nesting depth of one value, function, component or instance type, refused where it is "
              "defined",
      .message = "type nesting is too deep",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxTypeDepth; } },
    { .limit = ComponentLimit::TypeSize,
      .name = "maxTypeSize",
      .rule = "effective type size of one type, and the sum of the effective sizes of the types aliased "
              "into one component from outside it, as wasm-tools measures them; refused when reached",
      .message = "effective type size exceeds the limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxTypeSize; } },
    { .limit = ComponentLimit::Types,
      .name = "maxTypes",
      .rule = "entries of every index space of sort type, per component",
      .message = "types count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxTypes; } },
    { .limit = ComponentLimit::Definitions,
      .name = "maxDefinitions",
      .rule = "definitions of every section, custom sections included, per component",
      .message = "definitions count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxDefinitions; } },
    // A per-component count leaves the pool unbounded: siblings and nested components each fill their
    // own. This one is charged before each item is read, so a 4-byte alias, which the pool holds in
    // a definition many times its size, can be made at most this many times in all.
    { .limit = ComponentLimit::DecodedItems,
      .name = "maxDecodedItems",
      .rule = "items of one decoded component and every component nested in it: definitions, nested "
              "components, and the elements of the vectors inside definitions (arguments, inline exports, "
              "attributes), charged before each is read",
      .message = "decoded items count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxDecodedItems; } },
    { .limit = ComponentLimit::NameBytes,
      .name = "maxNameBytes",
      .rule = "bytes of one name, attribute or custom section name, refused before it is copied",
      .message = "string size out of bounds",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxNameBytes; } },
    { .limit = ComponentLimit::CoreInstances,
      .name = "maxCoreInstances",
      .rule = "core instances of one ComponentStore",
      .message = "core instances count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxCoreInstances; } },
    { .limit = ComponentLimit::ComponentInstances,
      .name = "maxComponentInstances",
      .rule = "component instances of one ComponentStore",
      .message = "component instances count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t {
          return limits.maxComponentInstances;
      } },
    { .limit = ComponentLimit::InstantiationSteps,
      .name = "maxInstantiationSteps",
      .rule = "plan steps one instantiate() executes",
      .message = "instantiation steps exceed the limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t {
          return limits.maxInstantiationSteps;
      } },
    { .limit = ComponentLimit::ResourceTypes,
      .name = "maxResourceTypes",
      .rule = "runtime resource types of one ComponentStore, one per resource definition executed; and the "
              "resource types one validation mints, one per resource definition and `(sub resource)` "
              "import or export",
      .message = "resource types count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxResourceTypes; } },
    { .limit = ComponentLimit::Handles,
      .name = "maxHandles",
      .rule = "handle slots of every handle table of one ComponentStore together, each kept by its table "
              "for reuse once freed, plus the handles lent to calls in progress; one table hands out "
              "indices up to 2^28 - 1",
      .message = "handles count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxHandles; } },
    { .limit = ComponentLimit::LiveValueBytes,
      .name = "maxLiveValueBytes",
      .rule = "host bytes of every live Values of one ComponentStore",
      .message = "live value bytes exceed the limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxLiveValueBytes; } },
    // Isoline's own: wasm-tools bounds neither, and walks its types recursively.
    { .limit = ComponentLimit::SubtypingSteps,
      .name = "maxSubtypingSteps",
      .rule =
          "steps of one type relation: a whole validation's subtype checks together (instantiations and "
          "ascribed exports share one relation), or one linker resolve; one per pair of types compared and "
          "per name looked up or compared, plus one per 64 bytes of the names compared; it bounds the "
          "relation's memo at as many entries",
      .message = "subtyping steps exceed the limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxSubtypingSteps; } },
    { .limit = ComponentLimit::SubstitutedTypes,
      .name = "maxSubstitutedTypes",
      .rule = "units the resource substitutions of one type arena charge, one per type created plus one per "
              "type it names: each instantiation, and each import or export of an instance type that defines "
              "resources, rewrites the types naming them, sharing their names",
      .message = "substituted types exceed the limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t {
          return limits.maxSubstitutedTypes;
      } },
    // wasm-tools' index space caps (wasmparser 0.263.0 src/limits.rs:22-95), in its wording (check_max,
    // src/validator.rs:72-86).
    { .limit = ComponentLimit::Modules,
      .name = "maxModules",
      .rule = "entries of one component's core module index space: embedded, imported, aliased and exported "
              "modules",
      .message = "modules count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxModules; } },
    { .limit = ComponentLimit::Components,
      .name = "maxComponents",
      .rule = "entries of one component's component index space: nested, imported, aliased and exported "
              "components",
      .message = "components count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxComponents; } },
    { .limit = ComponentLimit::Instances,
      .name = "maxInstances",
      .rule = "entries of one component's core and component instance index spaces together",
      .message = "instances count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxInstances; } },
    { .limit = ComponentLimit::Functions,
      .name = "maxFunctions",
      .rule = "entries of one component's core and component function index spaces together",
      .message = "functions count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxFunctions; } },
    { .limit = ComponentLimit::Exports,
      .name = "maxExports",
      .rule = "exports of one component",
      .message = "exports count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxExports; } },
    { .limit = ComponentLimit::CoreTables,
      .name = "maxCoreTables",
      .rule = "entries of one component's core table index space, each an alias of a core instance's export",
      .message = "tables count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxCoreTables; } },
    { .limit = ComponentLimit::CoreMemories,
      .name = "maxCoreMemories",
      .rule = "entries of one component's core memory index space, each an alias of a core instance's export",
      .message = "memories count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxCoreMemories; } },
    { .limit = ComponentLimit::CoreGlobals,
      .name = "maxCoreGlobals",
      .rule = "entries of one component's core global index space, each an alias of a core instance's export",
      .message = "globals count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxCoreGlobals; } },
    { .limit = ComponentLimit::CoreTags,
      .name = "maxCoreTags",
      .rule = "entries of one component's core tag index space, each an alias of a core instance's export",
      .message = "tags count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxCoreTags; } },
    // Isoline's own: wasm-tools matches every core instantiation without a bound.
    { .limit = ComponentLimit::CoreMatchingSteps,
      .name = "maxCoreMatchingSteps",
      .rule =
          "steps of all core instantiations of one validation: one per import looked up and matched, and per "
          "export or inline export measured, plus one per 64 bytes of the names its lookups compare",
      .message = "core matching steps exceed the limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t {
          return limits.maxCoreMatchingSteps;
      } },
    // Isoline's own, below the core default: a component hop takes more host stack than a core one.
    { .limit = ComponentLimit::NestedCalls,
      .name = "maxNestedCalls",
      .rule = "invocations in progress at once in one ComponentStore's core store: the outermost component "
              "call and each hop back into a guest; one more traps CallStackExhausted",
      .message = "nested calls exceed the limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxNestedCalls; } },
    // Isoline's own: the host's side of R12, which no guest may grow without bound.
    { .limit = ComponentLimit::HostOwns,
      .name = "maxHostOwns",
      .rule =
          "owns one ComponentStore's host holds at once, each lifted to a host function or a call's caller "
          "or made by hostResource(); one more fails the call ResourceExhausted",
      .message = "host-held owns exceed the limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxHostOwns; } },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: LimitTable)
        {
            if (static_cast<std::size_t>(row.limit) != index)
                return false;
            ++index;
        }
        return index == ComponentLimitCount;
    }(),
    "LimitTable must hold one row per ComponentLimit, at the index of its enumerator");

/// The table row describing @p limit.
/// @param limit The cap to look up.
/// @return Its row in LimitTable.
[[nodiscard]] constexpr ComponentLimitRow const& limitRow(ComponentLimit limit) noexcept
{
    return LimitTable[static_cast<std::size_t>(limit)];
}

/// The value of one cap.
/// @param limits The caps.
/// @param limit The cap to read.
/// @return Its value in @p limits.
[[nodiscard]] constexpr uint64_t limitValue(ComponentLimits const& limits, ComponentLimit limit) noexcept
{
    return limitRow(limit).value(limits);
}

/// The text of a refusal of @p limit.
/// @param limit The cap exceeded.
/// @param value The cap's value, which the text names where its row's message does.
/// @return The row's message, formatted with @p value.
[[nodiscard]] inline std::string limitMessage(ComponentLimit limit, uint64_t value)
{
    return std::vformat(limitRow(limit).message, std::make_format_args(value));
}

} // namespace isoline::component
