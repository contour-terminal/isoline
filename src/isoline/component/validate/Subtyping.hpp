// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/component/Limits.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/Names.hpp>

namespace isoline::component::validate
{

/// Which resource of the super arena stands for which resource of the sub arena, before any is bound
/// by a component's imports or exports: a linker binds its host resources so. Within one arena a
/// resource left unmapped stands for itself; across arenas it stands for none.
using ResourceMapping = types::ResourceSubstitution;

/// The bytes of names compared that cost a TypeRelation one step beyond the comparison's own.
inline constexpr auto SubtypingBytesPerStep = std::size_t { 64 };

/// Why two types are not related: the mismatch wasm-tools reports last, or the step cap.
///
/// The enumerators are the rows of RelationErrorMessages, in the same order.
enum class RelationErrorCode : uint8_t
{
    KindMismatch,
    PrimitiveMismatch,
    ResourceMismatch,
    ElementsMismatch,
    FieldCount,
    CaseCount,
    TypeCount,
    FixedListLength,
    PayloadMissing,
    PayloadPresent,
    FieldName,
    CaseName,
    CaseTypeMissing,
    CaseTypePresent,
    Asyncness,
    ParameterCount,
    ParameterName,
    ResultMissing,
    ResultPresent,
    MissingExport,
    MissingExtern,
    MissingImport,
    CoreMismatch,
    StepLimit,
};

/// One row of RelationErrorMessages: a code and its text, a std::format string of up to two strings.
struct RelationErrorRow
{
    RelationErrorCode code; ///< The code; equal to the row's index.
    std::string_view text;  ///< Its text.
};

/// Every relation error, in RelationErrorCode's order: wasm-tools 1.263.0's texts (wasmparser 0.263.0
/// src/validator/component_types.rs:3554-4440). StepLimit is Isoline's own, the limit's message.
inline constexpr auto RelationErrorMessages = std::to_array<RelationErrorRow>({
    { .code = RelationErrorCode::KindMismatch, .text = "expected {0}, found {1}" },
    { .code = RelationErrorCode::PrimitiveMismatch,
      .text = "expected primitive `{0}` found primitive `{1}`" },
    { .code = RelationErrorCode::ResourceMismatch, .text = "resource types are not the same" },
    { .code = RelationErrorCode::ElementsMismatch, .text = "mismatch in {0} elements" },
    { .code = RelationErrorCode::FieldCount, .text = "expected {0} fields, found {1}" },
    { .code = RelationErrorCode::CaseCount, .text = "expected {0} cases, found {1}" },
    { .code = RelationErrorCode::TypeCount, .text = "expected {0} types, found {1}" },
    { .code = RelationErrorCode::FixedListLength, .text = "expected fixed-length {0}, found size {1}" },
    { .code = RelationErrorCode::PayloadMissing, .text = "expected {0} type, but found none" },
    { .code = RelationErrorCode::PayloadPresent, .text = "expected {0} type to not be present" },
    { .code = RelationErrorCode::FieldName, .text = "expected field name `{0}`, found `{1}`" },
    { .code = RelationErrorCode::CaseName, .text = "expected case named `{0}`, found `{1}`" },
    { .code = RelationErrorCode::CaseTypeMissing, .text = "expected case `{0}` to have a type, found none" },
    { .code = RelationErrorCode::CaseTypePresent, .text = "expected case `{0}` to have no type" },
    { .code = RelationErrorCode::Asyncness, .text = "expected {0} function, found {1} function" },
    { .code = RelationErrorCode::ParameterCount, .text = "expected {0} parameters, found {1}" },
    { .code = RelationErrorCode::ParameterName, .text = "expected parameter named `{0}`, found `{1}`" },
    { .code = RelationErrorCode::ResultMissing, .text = "expected a result, found none" },
    { .code = RelationErrorCode::ResultPresent, .text = "expected no result, found one" },
    { .code = RelationErrorCode::MissingExport, .text = "missing expected export `{0}`" },
    { .code = RelationErrorCode::MissingExtern, .text = "missing {0} named `{1}`" },
    { .code = RelationErrorCode::MissingImport, .text = "missing expected import `{0}::{1}`" },
    // A core entity's mismatch: its context and CoreMismatchMessages' row, formatted where it is found.
    { .code = RelationErrorCode::CoreMismatch, .text = "{0}" },
    { .code = RelationErrorCode::StepLimit, .text = "{0}" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: RelationErrorMessages)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(RelationErrorCode::StepLimit) + 1;
    }(),
    "RelationErrorMessages must hold one row per RelationErrorCode, at the index of its enumerator");

/// Where a mismatch was found, as the line wasm-tools adds on its way out (with_context).
///
/// The enumerators are the rows of RelationContextMessages, in the same order.
enum class RelationContext : uint8_t
{
    RecordField,
    VariantCase,
    TupleField,
    MapKey,
    MapValue,
    OkVariant,
    ErrVariant,
    FutureElement,
    StreamElement,
    FunctionParameter,
    FunctionResult,
    InstanceExport,
    Extern,
    CoreImport,
    CoreExport,
};

/// One row of RelationContextMessages: a context and its text, a std::format string as above.
struct RelationContextRow
{
    RelationContext context; ///< The context; equal to the row's index.
    std::string_view text;   ///< Its text.
};

/// Every context line, in RelationContext's order: wasm-tools 1.263.0's texts (same source).
inline constexpr auto RelationContextMessages = std::to_array<RelationContextRow>({
    { .context = RelationContext::RecordField, .text = "type mismatch in record field `{0}`" },
    { .context = RelationContext::VariantCase, .text = "type mismatch in variant case `{0}`" },
    { .context = RelationContext::TupleField, .text = "type mismatch in tuple field {0}" },
    { .context = RelationContext::MapKey, .text = "type mismatch in map key" },
    { .context = RelationContext::MapValue, .text = "type mismatch in map value" },
    { .context = RelationContext::OkVariant, .text = "type mismatch in ok variant" },
    { .context = RelationContext::ErrVariant, .text = "type mismatch in err variant" },
    { .context = RelationContext::FutureElement, .text = "type mismatch in future" },
    { .context = RelationContext::StreamElement, .text = "type mismatch in stream" },
    { .context = RelationContext::FunctionParameter, .text = "type mismatch in function parameter `{0}`" },
    { .context = RelationContext::FunctionResult, .text = "type mismatch with result type" },
    { .context = RelationContext::InstanceExport, .text = "type mismatch in instance export `{0}`" },
    { .context = RelationContext::Extern, .text = "type mismatch for {0} `{1}`" },
    { .context = RelationContext::CoreImport, .text = "type mismatch in import `{0}::{1}`" },
    { .context = RelationContext::CoreExport, .text = "type mismatch in export `{0}`" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: RelationContextMessages)
        {
            if (static_cast<std::size_t>(row.context) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(RelationContext::CoreExport) + 1;
    }(),
    "RelationContextMessages must hold one row per RelationContext, at the index of its enumerator");

/// Two types that are not related: why, and wasm-tools' message.
struct RelationError
{
    RelationErrorCode code = RelationErrorCode::KindMismatch; ///< The mismatch found, or StepLimit.
    /// wasm-tools' message: its contexts, outermost first, then the mismatch, one per line; or the
    /// limit's message.
    std::string message = {};
};

/// Decides subtyping between the extern types of two arenas, and equality between their value types,
/// as wasm-tools does (SubtypeCx, wasmparser 0.263.0 src/validator/component_types.rs:3482-4440):
/// functions and value types are equal or not, structurally (wasm-tools' intended divergence from the
/// specification's subtyping, which this follows); an instance type with more exports is a subtype of
/// one with fewer; component types are contravariant in their imports and covariant in their exports,
/// their resources bound through them as wasm-tools' open_instance_type binds them; resource types are
/// equal only to themselves, or to what a binding maps them to. A binding records what the supplied
/// resource stands for under the bindings already in force, as wasm-tools remaps the supplied types
/// before it binds (component_types.rs:3655-3664).
///
/// One relation serves one validation (or one linker's resolve): it memoises every pair it found
/// related, under the resource bindings in force when it did (or under any, for a pair naming no
/// resource), so a pair is decided once however often a graph shares it, and deciding over two DAGs
/// takes O(pairs visited), never exponential time. Every pair started and every name looked up or
/// compared is a step, plus one per SubtypingBytesPerStep bytes of names compared, charged against
/// ComponentLimits::maxSubtypingSteps. The memo holds at most one entry per step, so its ceiling is
/// maxSubtypingSteps entries per relation. The walk is an explicit stack of frames, never recursion.
///
/// The relation reads both arenas, which must outlive it and not change while it is used.
class TypeRelation
{
  public:
    /// A relation of the types of @p sub to those of @p super.
    /// @param sub The arena of the candidate subtypes (the provider: a supplied argument, a definition).
    /// @param super The arena of the supertypes (the requirement: an import's type). May be @p sub.
    /// @param resources The resources of @p super already bound to resources of @p sub.
    /// @param limits Its cap: maxSubtypingSteps.
    TypeRelation(types::TypeArena const& sub,
                 types::TypeArena const& super,
                 ResourceMapping const& resources,
                 ComponentLimits const& limits);

    /// Whether @p sub, of the sub arena, is a subtype of @p super, of the super arena (wasm-tools'
    /// component_entity_type).
    /// @param sub The provided extern type.
    /// @param super The required one.
    /// @return Nothing when it is, else why not.
    [[nodiscard]] std::expected<void, RelationError> isSubtype(types::ExternType const& sub,
                                                               types::ExternType const& super);

    /// Whether two value types are equal (wasm-tools' component_val_type, which is equality).
    /// @param sub A value type of the sub arena.
    /// @param super A value type of the super arena.
    /// @return Nothing when they are, else why not.
    [[nodiscard]] std::expected<void, RelationError> isEqual(types::TypeId sub, types::TypeId super);

    /// Whether named arguments of the sub arena satisfy the imports of a component type of the super
    /// arena, as instantiating it with them requires (wasm-tools' open_instance_type of the imports):
    /// each of the component's imported resources is bound to the resource its argument supplies, then
    /// every import must have an argument of its name, and each argument must be a subtype of its
    /// import with those bindings applied.
    /// @param arguments The arguments, by name.
    /// @param component The component type instantiated.
    /// @return The binding of each of the component's imported resources to its argument's resource,
    ///         for its instance type (instantiatedExports()), or why the arguments do not satisfy it.
    [[nodiscard]] std::expected<types::ResourceSubstitution, RelationError> satisfyImports(
        std::span<types::NamedExtern const> arguments, types::ComponentTypeId component);

    /// The steps taken so far, every query together: the measure of the relation's work, which tests
    /// bound.
    /// @return The count, at most maxSubtypingSteps.
    [[nodiscard]] std::size_t steps() const noexcept { return _steps; }

  private:
    /// One side of the relation.
    enum class Side : uint8_t
    {
        Sub,
        Super,
    };

    /// What a frame of the walk compares.
    enum class FrameKind : uint8_t
    {
        Value,
        Function,
        Instance,
        Component,
        Open,
    };

    /// How far a Component frame is: its imports are opened first, then its exports.
    enum class ComponentPhase : uint8_t
    {
        Imports,
        Exports,
        Finished,
    };

    /// How a primitive is named when it is the type expected: wasm-tools words a mismatch against a
    /// primitive written inline ("expected u32") apart from one defined as a type ("expected
    /// primitive"). The arena does not keep the two apart, so the outermost value type of a type
    /// binding is taken as defined and every other as inline.
    enum class PrimitiveSpelling : uint8_t
    {
        Inline,
        Defined,
    };

    /// One pair being compared, and how far.
    struct Frame
    {
        FrameKind kind = FrameKind::Value;                ///< What it compares.
        Side actualSide = Side::Sub;                      ///< The arena of the provided type.
        uint32_t actual = 0;                              ///< The provided type, by its index.
        uint32_t expected = 0;                            ///< The required type, by its index.
        std::size_t next = 0;                             ///< The next child to compare.
        ComponentPhase phase = ComponentPhase::Imports;   ///< A Component frame's phase.
        std::span<types::NamedExtern const> arguments {}; ///< An Open frame's provided externs.
        ExternKind externKind = ExternKind::Import;       ///< An Open frame's side of the component.
        std::vector<uint32_t> matched {};                 ///< Per expected extern, its provider.
        std::size_t undoMark = 0;                         ///< A component's bindings start here.
        uint32_t savedEpoch = 0;                          ///< The epoch a component started in.
        uint32_t memoEpoch = 0;                           ///< The epoch its result is memoised in.
    };

    /// What starting a pair, or advancing a frame, did.
    enum class Progress : uint8_t
    {
        Done,   ///< Decided: related (a frame advanced: it is finished).
        Pushed, ///< A frame was pushed to decide it (a frame advanced: it has more to do).
    };

    /// Where a failure was found: by the frame on top itself, or by a child it started, for which it
    /// then adds its context.
    enum class FailureSite : uint8_t
    {
        Frame,
        Child,
    };

    /// A failure, and where it was found.
    struct Failure
    {
        RelationError error {};
        FailureSite site = FailureSite::Frame;
    };

    template <typename T>
    using Outcome = std::expected<T, Failure>;

    /// A memoised pair: related under the bindings of its epoch (0: under any bindings).
    struct MemoKey
    {
        FrameKind kind = FrameKind::Value;
        Side actualSide = Side::Sub;
        uint32_t actual = 0;
        uint32_t expected = 0;
        uint32_t epoch = 0;

        auto operator<=>(MemoKey const&) const = default;
    };

    /// What a resource stands for: a resource of one side's arena.
    struct ResourceIdentity
    {
        Side side = Side::Sub;
        uint32_t resource = 0;

        bool operator==(ResourceIdentity const&) const = default;
    };

    /// One binding made, to be undone.
    struct Undo
    {
        Side side = Side::Sub;
        uint32_t resource = 0;
        std::optional<ResourceIdentity> previous {};
    };

    [[nodiscard]] static constexpr Side opposite(Side side) noexcept
    {
        return side == Side::Sub ? Side::Super : Side::Sub;
    }

    [[nodiscard]] types::TypeArena const& arena(Side side) const noexcept;
    [[nodiscard]] Outcome<void> step(std::size_t count = 1);
    [[nodiscard]] Outcome<void> compareNames(std::size_t comparisons, std::size_t bytes);
    [[nodiscard]] bool memoised(MemoKey const& key) const { return _memo.contains(key); }
    [[nodiscard]] ResourceIdentity identity(Side side, types::ResourceTypeId resource) const;
    [[nodiscard]] bool sameResource(Side actualSide,
                                    types::ResourceTypeId actual,
                                    types::ResourceTypeId expected) const;
    void bind(Side side, uint32_t resource, ResourceIdentity image);
    void restore(std::size_t mark, uint32_t epoch);

    [[nodiscard]] Outcome<void> run(Outcome<Progress> started);
    [[nodiscard]] Outcome<Progress> advance(Frame& frame);
    [[nodiscard]] Outcome<Progress> advanceValue(Frame& frame);
    [[nodiscard]] Outcome<Progress> advanceFunction(Frame& frame);
    [[nodiscard]] std::optional<std::string> context(Frame const& frame) const;
    void finish(Frame const& frame);

    [[nodiscard]] Outcome<Progress> startEntity(Side actualSide,
                                                types::ExternType const& actual,
                                                types::ExternType const& expected);
    [[nodiscard]] Outcome<Progress> startAny(Side actualSide,
                                             types::AnyTypeId actual,
                                             types::AnyTypeId expected);
    [[nodiscard]] Outcome<Progress> startValue(Side actualSide,
                                               types::TypeId actual,
                                               types::TypeId expected,
                                               PrimitiveSpelling spelling);
    [[nodiscard]] Outcome<Progress> startFunction(Side actualSide,
                                                  types::FunctionTypeId actual,
                                                  types::FunctionTypeId expected);
    [[nodiscard]] Outcome<Progress> startInstance(Side actualSide,
                                                  types::InstanceTypeId actual,
                                                  types::InstanceTypeId expected);
    [[nodiscard]] Outcome<Progress> startComponent(Side actualSide,
                                                   types::ComponentTypeId actual,
                                                   types::ComponentTypeId expected);
    [[nodiscard]] Outcome<void> startModule(Side actualSide,
                                            types::CoreModuleTypeId actual,
                                            types::CoreModuleTypeId expected);
    [[nodiscard]] Outcome<void> matchCore(Side subSide,
                                          types::CoreEntityType const& sub,
                                          types::CoreEntityType const& required,
                                          std::string const& context);
    [[nodiscard]] bool coreEquivalent(Side subSide, uint32_t sub, uint32_t super);
    [[nodiscard]] Outcome<void> buildJointCoreIds();
    /// Which list of an arena a name index orders.
    enum class IndexedList : uint8_t
    {
        InstanceExports,
        ComponentImports,
        ComponentExports,
    };

    /// The list of one side's arena a cached name index orders.
    struct IndexKey
    {
        Side side = Side::Sub;
        IndexedList list = IndexedList::InstanceExports;
        uint32_t id = 0;

        auto operator<=>(IndexKey const&) const = default;
    };

    [[nodiscard]] Outcome<Progress> startOpen(Side actualSide,
                                              std::span<types::NamedExtern const> arguments,
                                              types::ComponentTypeId expected,
                                              ExternKind kind,
                                              std::optional<IndexKey> argumentsKey);
    [[nodiscard]] Outcome<std::vector<uint32_t> const*> cachedNameIndex(
        IndexKey key, std::span<types::NamedExtern const> externs);
    [[nodiscard]] Outcome<std::optional<uint32_t>> scan(std::span<types::NamedExtern const> externs,
                                                        std::string const& name);
    [[nodiscard]] Outcome<std::vector<uint32_t>> nameIndex(std::span<types::NamedExtern const> externs);
    [[nodiscard]] Outcome<std::optional<uint32_t>> lookup(std::span<types::NamedExtern const> externs,
                                                          std::vector<uint32_t> const& index,
                                                          std::string const& name);
    [[nodiscard]] uint32_t memoEpoch(Side actualSide,
                                     types::AnyTypeId actual,
                                     types::AnyTypeId expected) const;

    types::TypeArena const& _sub;
    types::TypeArena const& _super;
    bool _isSameArena;
    std::size_t _maxSteps;
    std::size_t _steps = 0;
    /// Per side: its bound resources, each to what it stands for.
    std::array<std::map<uint32_t, ResourceIdentity>, 2> _bindings {};
    std::vector<Undo> _undo {};
    uint32_t _epoch = 1;
    uint32_t _nextEpoch = 2;
    std::set<MemoKey> _memo {};
    std::vector<Frame> _frames {};
    /// The name index of every list compared so far, built and charged once (cachedNameIndex()).
    std::map<IndexKey, std::vector<uint32_t>> _nameIndices {};
    /// The step limit a cross-arena core type comparison reached, which its bool result cannot carry.
    std::optional<Failure> _coreFailure {};
    /// Across two arenas, the joint canonical ids of their core function types, the sub arena's then
    /// the super arena's (binary::jointCanonicalTypeIds()): built on the first comparison, charged a
    /// step per type, and rebuilt only if an arena gained core types since.
    std::vector<uint32_t> _jointCoreIds {};
};

/// Mints a fresh resource type for every resource a component type defines, as each instantiation of
/// it does (Explainer, "generative" resource types; wasm-tools' fresh_defined_resources): two
/// instances of one component type never share a defined resource.
/// @param arena The arena of the component type, which receives the new resources.
/// @param instantiated The component type instantiated.
/// @return Each defined resource mapped to its fresh copy, or ResourceTypes once maxResourceTypes are
///         held.
[[nodiscard]] std::expected<types::ResourceSubstitution, types::TypeLimitError> freshResourcesFor(
    types::TypeArena& arena, types::ComponentTypeInfo const& instantiated);

/// The instance type an instantiation of a component type has: its exports, with @p substitution
/// applied (its imported resources bound to the arguments', its defined ones fresh).
/// @param arena The arena of the component type, which receives the instance type.
/// @param component The component type instantiated.
/// @param substitution satisfyImports()'s bindings and freshResourcesFor()'s, together.
/// @return The instance type, which defines no resources, or the cap rewriting it exceeds.
[[nodiscard]] std::expected<types::InstanceTypeId, types::TypeLimitError> instantiatedExports(
    types::TypeArena& arena,
    types::ComponentTypeId component,
    types::ResourceSubstitution const& substitution);

} // namespace isoline::component::validate
