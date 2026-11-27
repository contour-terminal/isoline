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

/// Why two types are not related.
enum class RelationFailure : uint8_t
{
    Mismatch,  ///< They are not: the message says where, as wasm-tools does.
    StepLimit, ///< Deciding it took more than ComponentLimits::maxSubtypingSteps steps.
};

/// Two types that are not related: why, and wasm-tools' message.
struct RelationError
{
    RelationFailure failure = RelationFailure::Mismatch; ///< Why.
    /// wasm-tools 1.263.0's message (wasmparser 0.263.0 src/validator/component_types.rs:3554-4440):
    /// its contexts, outermost first, then the mismatch, one per line; or the limit's message.
    std::string message = {};
};

/// Decides subtyping between the extern types of two arenas, and equality between their value types,
/// as wasm-tools does (SubtypeCx, wasmparser 0.263.0 src/validator/component_types.rs:3482-4440):
/// functions and value types are equal or not, structurally (wasm-tools' intended divergence from the
/// specification's subtyping, which this follows); an instance type with more exports is a subtype of
/// one with fewer; component types are contravariant in their imports and covariant in their exports,
/// their resources bound through them as wasm-tools' open_instance_type binds them; resource types are
/// equal only to themselves, or to what a binding maps them to.
///
/// One relation serves one validation (or one linker's resolve): it memoises every pair it found
/// related, under the resource bindings in force when it did, so a pair is decided once however often
/// a graph shares it, and deciding over two DAGs takes O(pairs visited), never exponential time. Every
/// pair started and every name looked up is a step, charged against ComponentLimits::maxSubtypingSteps,
/// which therefore bounds the memo's size too. The walk is an explicit stack of frames, never
/// recursion.
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
        uint32_t phase = 0;                               ///< A component's phase.
        std::span<types::NamedExtern const> arguments {}; ///< An Open frame's provided externs.
        ExternKind externKind = ExternKind::Import;       ///< An Open frame's side of the component.
        std::vector<uint32_t> matched {};                 ///< Per expected extern, its provider.
        std::size_t undoMark = 0;                         ///< A component's bindings start here.
        uint32_t savedEpoch = 0;                          ///< The epoch a component started in.
        uint32_t memoEpoch = 0;                           ///< The epoch its result is memoised in.
    };

    /// What starting a pair did.
    enum class Progress : uint8_t
    {
        Done,   ///< Decided at once: related.
        Pushed, ///< A frame was pushed to decide it.
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

    /// One binding made, to be undone.
    struct Undo
    {
        Side side = Side::Sub;
        uint32_t resource = 0;
        std::optional<uint32_t> previous {};
    };

    [[nodiscard]] static constexpr Side opposite(Side side) noexcept
    {
        return side == Side::Sub ? Side::Super : Side::Sub;
    }

    [[nodiscard]] types::TypeArena const& arena(Side side) const noexcept;
    [[nodiscard]] Outcome<void> step(std::size_t count = 1);
    [[nodiscard]] bool memoised(MemoKey const& key) const { return _memo.contains(key); }
    [[nodiscard]] std::pair<Side, uint32_t> identity(Side side, types::ResourceTypeId resource) const;
    [[nodiscard]] bool sameResource(Side actualSide,
                                    types::ResourceTypeId actual,
                                    types::ResourceTypeId expected) const;
    void bind(Side side, uint32_t resource, uint32_t image);
    void restore(std::size_t mark, uint32_t epoch);

    [[nodiscard]] Outcome<void> run(Outcome<Progress> started);
    [[nodiscard]] Outcome<bool> advance(Frame& frame);
    [[nodiscard]] Outcome<bool> advanceValue(Frame& frame);
    [[nodiscard]] Outcome<bool> advanceFunction(Frame& frame);
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
    [[nodiscard]] Outcome<Progress> startOpen(Side actualSide,
                                              std::span<types::NamedExtern const> arguments,
                                              types::ComponentTypeId expected,
                                              ExternKind kind);
    [[nodiscard]] Outcome<std::optional<uint32_t>> lookup(std::span<types::NamedExtern const> externs,
                                                          std::string const& name);
    [[nodiscard]] Outcome<std::vector<uint32_t>> nameIndex(std::span<types::NamedExtern const> externs);
    [[nodiscard]] Outcome<std::optional<uint32_t>> lookup(std::span<types::NamedExtern const> externs,
                                                          std::vector<uint32_t> const& index,
                                                          std::string const& name);

    types::TypeArena const& _sub;
    types::TypeArena const& _super;
    bool _sameArena;
    std::size_t _maxSteps;
    std::size_t _steps = 0;
    std::array<std::map<uint32_t, uint32_t>, 2> _bindings {}; ///< Per side: its resource → the other's.
    std::vector<Undo> _undo {};
    uint32_t _epoch = 1;
    uint32_t _nextEpoch = 2;
    std::set<MemoKey> _memo {};
    std::vector<Frame> _frames {};
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
