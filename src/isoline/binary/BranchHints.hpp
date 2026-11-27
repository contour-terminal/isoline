// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace isoline::binary
{

struct Module;

/// The name of the custom section that holds a module's branch hints.
inline constexpr auto BranchHintSectionName = std::string_view { "metadata.code.branch_hint" };

/// Whether a hinted branch is expected to be taken.
enum class BranchLikelihood : uint8_t
{
    Unlikely, ///< Hint byte 0x00: the branch is unlikely to be taken.
    Likely,   ///< Hint byte 0x01: the branch is likely to be taken.
};

/// A branch hint of the `metadata.code.branch_hint` section, attached to an instruction of a function
/// body. Only the decoder requires that instruction to be an if or br_if: the text reader attaches an
/// `@metadata.code.branch_hint` annotation to whatever instruction follows it, and the encoder writes
/// it, so that the decoder refuses a hint on anything else, as the reference interpreter's check does.
struct BranchHint
{
    uint32_t function = 0; ///< The hinted function's index in the function index space.
    /// The index of the hinted instruction in that function's Function::body; an if or br_if in every
    /// hint the decoder keeps.
    uint32_t instruction = 0;
    BranchLikelihood likelihood = BranchLikelihood::Unlikely; ///< What the hint says.
};

/// Why a custom section the decoder interprets was refused. Such a refusal never fails the module;
/// the decoder records it in Module::customErrors and drops what the section said.
///
/// The enumerators are the rows of CustomErrorTable, in the same order; adding an error is adding an
/// enumerator here and a row there.
enum class CustomErrorCode : uint8_t
{
    BranchHintMalformed,        ///< The section does not follow its grammar.
    BranchHintOutOfOrder,       ///< A function index or offset does not increase strictly.
    BranchHintInvalidTarget,    ///< A function is not defined, or an offset is not an if or br_if.
    BranchHintDuplicateSection, ///< A module holds a second branch hint section.
};

/// The last CustomErrorCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastCustomErrorCode = CustomErrorCode::BranchHintDuplicateSection;

/// One row of CustomErrorTable.
struct CustomErrorRow
{
    CustomErrorCode code;         ///< The error this row describes; equal to the row's index.
    std::string_view section;     ///< The name of the custom section it refuses.
    std::string_view specMessage; ///< The text the spec suite's assert_invalid_custom matches as a prefix.
};

/// Every custom section error, one row per CustomErrorCode enumerator, in enumerator order.
///
/// Every text starts with the `@metadata.code.branch_hint annotation: ` prefix of the reference
/// interpreter's checks (`interpreter/custom/handler_branch_hint.ml`). "invalid order" and "invalid
/// target" are the reference's own texts, which the spec suite's `assert_invalid_custom` matches. The
/// reference refuses a malformed section while decoding, with texts of its own that no
/// `assert_invalid_custom` names, and decodes every hint section; "malformed" and "duplicate section"
/// are Isoline's.
inline constexpr auto CustomErrorTable = std::to_array<CustomErrorRow>({
    { .code = CustomErrorCode::BranchHintMalformed,
      .section = BranchHintSectionName,
      .specMessage = "@metadata.code.branch_hint annotation: malformed" },
    { .code = CustomErrorCode::BranchHintOutOfOrder,
      .section = BranchHintSectionName,
      .specMessage = "@metadata.code.branch_hint annotation: invalid order" },
    { .code = CustomErrorCode::BranchHintInvalidTarget,
      .section = BranchHintSectionName,
      .specMessage = "@metadata.code.branch_hint annotation: invalid target" },
    { .code = CustomErrorCode::BranchHintDuplicateSection,
      .section = BranchHintSectionName,
      .specMessage = "@metadata.code.branch_hint annotation: duplicate section" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: CustomErrorTable)
            if (static_cast<std::size_t>(row.code) != index++)
                return false;
        return index == static_cast<std::size_t>(LastCustomErrorCode) + 1;
    }(),
    "CustomErrorTable must hold one row per CustomErrorCode, at the index of its enumerator");

/// A custom section the decoder refused: what went wrong, and where.
struct CustomSectionError
{
    /// Builds an error; both are required, so one that forgets its code does not compile.
    /// @param code What went wrong.
    /// @param offset The absolute byte offset in the module it is reported at.
    constexpr CustomSectionError(CustomErrorCode code, std::size_t offset) noexcept:
        code { code }, offset { offset }
    {
    }

    CustomErrorCode code; ///< What went wrong.
    std::size_t offset;   ///< The absolute byte offset in the module it is reported at.

    /// Two errors are equal when their code and offset are.
    /// @return Whether this error and the other are the same refusal.
    [[nodiscard]] constexpr bool operator==(CustomSectionError const&) const noexcept = default;
};

/// The custom section an error of @p code refuses.
/// @param code The error.
/// @return Its row's section name in CustomErrorTable.
[[nodiscard]] constexpr std::string_view customSectionOf(CustomErrorCode code) noexcept
{
    return CustomErrorTable[static_cast<std::size_t>(code)].section;
}

/// The message the spec test suite expects for @p code.
/// @param code The error to describe.
/// @return Its row's message in CustomErrorTable.
[[nodiscard]] constexpr std::string_view specMessage(CustomErrorCode code) noexcept
{
    return CustomErrorTable[static_cast<std::size_t>(code)].specMessage;
}

/// Decodes a `metadata.code.branch_hint` section into @p module's Module::branchHints, against the
/// branch sites its code section recorded.
///
/// The payload is `vec(funcidx vec(offset:u32 size:u32 hint:byte))`: function indices of defined
/// functions and, within each, offsets strictly increasing, every size 1, every hint 0x00 or 0x01,
/// and every offset, relative to the function's locals declaration, that of an if or br_if. On the
/// first violation the error is appended to Module::customErrors and none of the section's hints are
/// kept; the module itself stays as it was. Every count is checked against the bytes left before
/// anything is allocated.
///
/// Precondition: @p module's code section is decoded, so Function::branchSites is complete.
/// @param module The decoded module the section belongs to.
/// @param payload The section's payload after its name.
/// @param payloadOffset The absolute byte offset of @p payload in the module, for error offsets.
void decodeBranchHints(Module& module, std::span<std::byte const> payload, std::size_t payloadOffset);

} // namespace isoline::binary
