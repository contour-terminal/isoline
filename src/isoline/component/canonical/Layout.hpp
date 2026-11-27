// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include <isoline/component/binary/Component.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::canonical
{

/// The most core parameters a synchronous function passes flat (definitions.py's MAX_FLAT_PARAMS);
/// more spill to one pointer.
inline constexpr auto MaxFlatParams = 16U;

/// The most core results a synchronous function returns flat (definitions.py's MAX_FLAT_RESULTS);
/// more are returned through memory.
inline constexpr auto MaxFlatResults = 1U;

/// The most core parameters an async lowering passes flat (definitions.py's MAX_FLAT_ASYNC_PARAMS).
/// P12 reads it; P11 flattens synchronous functions only.
inline constexpr auto MaxFlatAsyncParams = 4U;

/// The most labels a `flags` type has at the pin (definitions.py's alignment_flags asserts
/// `0 < n <= 32`), all of them in one i32.
inline constexpr auto MaxFlagLabels = 32U;

/// The byte size every value type stays below, with 64-bit pointers (wasmparser 0.263.0's
/// MAX_WASM_VALUE_TYPE_BYTE_SIZE, src/limits.rs:82; CanonicalABI.md, "Element Size"): a spec
/// constant, not an embedder limit.
inline constexpr auto MaxValueByteSize = uint32_t { 1 } << 28U;

/// Whether a value type's flattening fits a synchronous function's flat parameters.
enum class FlatShape : uint8_t
{
    Fits,      ///< At most MaxFlatParams core values: Layout::flat holds all of them.
    Overflows, ///< More than MaxFlatParams: Layout::flat holds the first MaxFlatParams + 1 of them.
};

/// Whether a value type holds a pointer somewhere inside it (wasm-tools' contains_ptr): a string, a
/// list or a map, whose lifting and lowering need a memory.
enum class PointerContent : uint8_t
{
    None, ///< No pointer.
    Held, ///< A string, list or map somewhere inside it, itself included.
};

/// How the canonical ABI lays out one value type (CanonicalABI.md, "Alignment", "Element Size" and
/// "Flattening"), with 32-bit pointers unless a member says otherwise (D14).
///
/// A compound's flat list is built from its children's and stops after MaxFlatParams + 1 entries, so
/// a type of exponentially many leaves costs a constant; its size and alignment are still exact.
struct Layout
{
    uint32_t size = 0;                              ///< definitions.py's elem_size(t, 'i32').
    uint32_t alignment = 1;                         ///< definitions.py's alignment(t, 'i32').
    std::vector<binary::CoreValueKind> flat {};     ///< flatten_type(t), up to MaxFlatParams + 1 entries.
    FlatShape flatShape = FlatShape::Fits;          ///< Whether flat holds all of flatten_type(t).
    uint8_t discriminantBytes = 0;                  ///< A variant's discriminant width (1, 2 or 4); else 0.
    std::vector<uint32_t> fieldOffsets {};          ///< A record's or a tuple's field offsets, in order.
    uint32_t payloadOffset = 0;                     ///< A variant's payload offset; else 0.
    uint32_t size64 = 0;                            ///< elem_size(t, 'i64'): only for the size limit (D14).
    uint32_t alignment64 = 1;                       ///< alignment(t, 'i64'), which size64 is built from.
    PointerContent pointers = PointerContent::None; ///< Whether a pointer is inside it.

    /// Two layouts are equal when every member is.
    bool operator==(Layout const&) const = default;
};

/// Why a value type has no layout.
enum class LayoutErrorCode : uint8_t
{
    ExceedsMaximumByteSize, ///< Its size with 64-bit pointers reaches MaxValueByteSize.
    FlagsLabelCount,        ///< A `flags` of no label or of more than MaxFlagLabels.
};

/// One row of LayoutErrorMessages: an error and its text.
struct LayoutErrorRow
{
    LayoutErrorCode code;  ///< The error; equal to the row's index.
    std::string_view text; ///< Its message.
};

/// Every layout error, one row per LayoutErrorCode enumerator, in enumerator order. The size refusal
/// is wasm-tools 1.263.0's text, which the suites' `assert_invalid` expects.
inline constexpr auto LayoutErrorMessages = std::to_array<LayoutErrorRow>({
    { .code = LayoutErrorCode::ExceedsMaximumByteSize,
      .text = "value type's maximum in-memory size exceeds maximum byte size" },
    { .code = LayoutErrorCode::FlagsLabelCount, .text = "flags must have between 1 and 32 labels" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: LayoutErrorMessages)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return true;
    }(),
    "LayoutErrorMessages must hold one row per LayoutErrorCode, at the index of its enumerator");

/// A value type the canonical ABI cannot lay out.
struct LayoutError
{
    LayoutErrorCode code = LayoutErrorCode::ExceedsMaximumByteSize; ///< Why.
    types::TypeId type {};                                          ///< The type refused.
    uint64_t value = 0; ///< For ExceedsMaximumByteSize the size reached; else the label count.

    /// Two errors are equal when every member is.
    bool operator==(LayoutError const&) const = default;
};

/// The message of a layout error.
/// @param code An error.
/// @return Its LayoutErrorMessages text.
[[nodiscard]] constexpr std::string_view layoutMessage(LayoutErrorCode code) noexcept
{
    return LayoutErrorMessages[static_cast<std::size_t>(code)].text;
}

/// The layouts of one arena's value types, by TypeId, each computed once from its children's.
///
/// An arena interns a type only after its children, so a type's id is above every child's, and
/// computing the layouts in id order is a post-order walk with no stack at all: no recursion, and
/// each type costs its own children and at most MaxFlatParams + 1 flat entries per child, whatever
/// the children share. steps() counts that work.
class LayoutTable
{
  public:
    /// An empty table for the arena @p arena.
    /// @param arena The identity of the arena whose types it lays out.
    explicit LayoutTable(types::TypeArenaId arena) noexcept: _arena { arena } {}

    /// Lays out every type @p arena added since the last call, in id order.
    /// @param arena The arena the table was made for (checked).
    /// @return Nothing, or the first type that has no layout; the table then ends before that type,
    ///         and a later call refuses it again.
    [[nodiscard]] std::expected<void, LayoutError> extend(types::TypeArena const& arena);

    /// The layout of a value type.
    /// @param id A type the table has laid out (checked).
    /// @return Its layout.
    [[nodiscard]] Layout const& operator[](types::TypeId id) const;

    /// Every layout, by TypeId.
    /// @return The layouts made so far.
    [[nodiscard]] std::span<Layout const> all() const noexcept { return _layouts; }

    /// The arena the table lays out.
    /// @return Its identity.
    [[nodiscard]] types::TypeArenaId arena() const noexcept { return _arena; }

    /// The work the layouts took: one step per type, per child read and per flat entry written.
    /// @return The steps so far.
    [[nodiscard]] uint64_t steps() const noexcept { return _steps; }

  private:
    types::TypeArenaId _arena;
    std::vector<Layout> _layouts {};
    uint64_t _steps = 0;
};

/// The layouts of every value type of @p arena.
/// @param arena An arena.
/// @return Its layouts, or the first type that has none.
[[nodiscard]] std::expected<LayoutTable, LayoutError> computeLayouts(types::TypeArena const& arena);

/// Which way a function crosses the canonical ABI (definitions.py's flatten_functype `context`).
enum class FlattenContext : uint8_t
{
    Lift,  ///< `canon lift`: a core function made a component function.
    Lower, ///< `canon lower`: a component function made a core function.
};

/// A core function signature: its parameter and result types.
struct CoreSignature
{
    std::vector<binary::CoreValueKind> params {};  ///< The parameters, in order.
    std::vector<binary::CoreValueKind> results {}; ///< The results, in order.

    /// Two signatures are equal when their parameters and results are.
    bool operator==(CoreSignature const&) const = default;
};

/// Whether a function's parameters fit MaxFlatParams flat core values, or spill to one pointer.
/// @param layouts The layouts of the arena that holds @p function's types.
/// @param function A function type.
/// @return Fits when the parameters' flattenings, concatenated, are at most MaxFlatParams long.
[[nodiscard]] FlatShape flatParamsShape(LayoutTable const& layouts, types::FunctionTypeInfo const& function);

/// The core signature of a synchronous function (definitions.py's flatten_functype without
/// `async`): the parameters flat up to MaxFlatParams, else one pointer; the result flat up to
/// MaxFlatResults, else a pointer, returned by a lift and passed last by a lower.
/// @param layouts The layouts of the arena that holds @p function's types.
/// @param function A synchronous function type (checked; P12 flattens async ones).
/// @param context Which way it crosses.
/// @return Its core signature.
[[nodiscard]] CoreSignature flattenFunction(LayoutTable const& layouts,
                                            types::FunctionTypeInfo const& function,
                                            FlattenContext context);

} // namespace isoline::component::canonical
