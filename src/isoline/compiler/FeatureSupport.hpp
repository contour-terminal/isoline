// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

#include <isoline/Features.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/compiler/CompileError.hpp>

namespace isoline::validate
{
class ValidatedModule;
} // namespace isoline::validate

namespace isoline::compiler
{

/// Whether the compiler compiles a feature's modules.
enum class CompilerSupport : uint8_t
{
    Deferred, ///< Not yet: a module that uses it is refused UnsupportedFeature.
    Compiled, ///< Compiled.
};

/// What the compiler supports of one Feature.
struct CompilerFeatureRow
{
    Feature feature;         ///< The feature; equal to the row's index.
    CompilerSupport support; ///< Whether it is compiled.
    std::string_view plan;   ///< The plan that compiles it: "P5", or the roadmap row it is deferred to.
};

/// Every Feature's support by the compiler, in enumerator order. The deferred ones are P5b's
/// ("compiler language completion"); the api refuses to construct a compiled engine whose FeatureSet
/// is not within compiledFeatures().
inline constexpr auto CompilerFeatureTable = std::to_array<CompilerFeatureRow>({
    { .feature = Feature::SignExtension, .support = CompilerSupport::Compiled, .plan = "P5" },
    { .feature = Feature::NonTrappingFloatToInt, .support = CompilerSupport::Compiled, .plan = "P5" },
    { .feature = Feature::MultiValue, .support = CompilerSupport::Compiled, .plan = "P5" },
    { .feature = Feature::ReferenceTypes, .support = CompilerSupport::Compiled, .plan = "P5" },
    { .feature = Feature::BulkMemory, .support = CompilerSupport::Compiled, .plan = "P5" },
    { .feature = Feature::Simd, .support = CompilerSupport::Deferred, .plan = "P5b" },
    { .feature = Feature::RelaxedSimd, .support = CompilerSupport::Deferred, .plan = "P5b" },
    { .feature = Feature::TailCall, .support = CompilerSupport::Deferred, .plan = "P5b" },
    { .feature = Feature::ExtendedConst, .support = CompilerSupport::Compiled, .plan = "P5" },
    { .feature = Feature::TypedFunctionReferences, .support = CompilerSupport::Deferred, .plan = "P5b" },
    { .feature = Feature::MultiMemory, .support = CompilerSupport::Compiled, .plan = "P5" },
    { .feature = Feature::ExceptionHandling, .support = CompilerSupport::Deferred, .plan = "P5b" },
    { .feature = Feature::Memory64, .support = CompilerSupport::Compiled, .plan = "P5" },
    { .feature = Feature::BranchHinting, .support = CompilerSupport::Compiled, .plan = "P5" },
    { .feature = Feature::Annotations, .support = CompilerSupport::Compiled, .plan = "P5" },
    { .feature = Feature::WideArithmetic, .support = CompilerSupport::Deferred, .plan = "P5b" },
    { .feature = Feature::CompactImports, .support = CompilerSupport::Compiled, .plan = "P5" },
    { .feature = Feature::CustomPageSizes, .support = CompilerSupport::Compiled, .plan = "P5" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: CompilerFeatureTable)
        {
            if (static_cast<std::size_t>(row.feature) != index)
                return false;
            ++index;
        }
        return index == FeatureCount;
    }(),
    "CompilerFeatureTable must hold one row per Feature, at the index of its enumerator");

/// The features the compiler compiles: CompilerFeatureTable's Compiled rows. What an embedder's
/// compiled engine may enable, and what Contour's enabled features must lie within.
/// @return The set.
[[nodiscard]] constexpr FeatureSet compiledFeatures() noexcept
{
    auto compiled = FeatureSet::none();
    for (auto const& row: CompilerFeatureTable)
        if (row.support == CompilerSupport::Compiled)
            compiled = compiled.with(row.feature);
    return compiled;
}

/// Whether the compiler compiles @p opcode: whether every feature it needs to decode
/// (binary::OpcodeInfo::required) is compiled.
/// @param opcode The instruction.
/// @param function The code index of the function it is in, for the error.
/// @return Nothing; or UnsupportedFeature naming the first deferred feature it needs, in Feature order.
[[nodiscard]] std::expected<void, CompileError> checkOpcode(binary::Opcode opcode,
                                                            uint32_t function) noexcept;

/// Whether the compiler compiles values of @p type: a v128 needs Simd; a reference to a concrete type
/// needs typed function references (a Ref travels as one word, exact only while the heap's type
/// index is always 0); a reference into the exception hierarchy needs exception handling.
/// @param type The value type, at any position: a parameter, a result, a local, a global, a table's
/// element, a segment's, a block's, a typed select's, ref.null's heap.
/// @param function The code index of the function it is in, for the error; none outside a body.
/// @return Nothing; or UnsupportedFeature naming the feature it needs.
[[nodiscard]] std::expected<void, CompileError> checkValueType(binary::ValueType const& type,
                                                               std::optional<uint32_t> function) noexcept;

/// Whether the compiler compiles @p module: refuses by use, not by the FeatureSet it was validated
/// with. It walks every type position once (the function types, imports included, tables, globals,
/// tags, element segments, every constant expression, and in each body the locals, the block types,
/// typed selects and ref.null's heap) and every body opcode, dead code included, iteratively.
/// @param module The validated module.
/// @return Nothing; or the first UnsupportedFeature in that order: the types, the imports, the tables,
///         the tags, the globals, the element segments, the data segments, then the bodies.
[[nodiscard]] std::expected<void, CompileError> checkModule(validate::ValidatedModule const& module);

} // namespace isoline::compiler
