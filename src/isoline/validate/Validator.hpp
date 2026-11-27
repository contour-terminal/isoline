// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/validate/ValidationError.hpp>

namespace isoline::validate
{

/// The static shape of one function's frame, as validation finds it: what an engine must hold for it
/// beyond its parameters and locals.
struct FrameShape
{
    /// The most operands the function's operand stack holds at once, Unknown ones of unreachable code
    /// included.
    uint32_t maxOperands = 0;

    /// The deepest nesting of blocks, loops, ifs and try_tables, the function's own label excluded.
    uint32_t maxLabels = 0;

    /// Whether both shapes hold the same high-water marks.
    [[nodiscard]] bool operator==(FrameShape const&) const = default;
};

/// The static shapes of a module's functions.
struct ModuleShape
{
    std::vector<FrameShape> functions; ///< One per code section entry, in order.
};

/// Validates a decoded module against the WebAssembly 3.0 type system, as far as @p features
/// enables its proposals (typed function references, exception handling, memory64, SIMD and the
/// rest), and the 2.0 subset when they are off.
///
/// Function bodies and constant expressions are typed with the spec appendix's validation
/// algorithm: an operand stack whose entries may be Unknown in unreachable code, and a stack of
/// control frames, each recording its operand stack height and whether its remainder is
/// unreachable. Both stacks live on the heap and the algorithm never recurses, so the nesting depth
/// of the input costs memory linear in it, not native stack. Time is linear in a body's size times the
/// widest signature it names. Branches check their label's operands in place and remember the one label
/// type list the top was last checked against, so a run of branches through one label's types checks
/// them once per change of type list: br_if 0 and br_if 1 alternating over two equally wide labels check
/// each time. A br_table checks each block its labels name at most once (docs/embedding.md has the rule
/// and the refusal order).
///
/// Module-level checks run in Isoline's own order (types, imports, tags, functions, result arities,
/// tables and memories, globals, element and data segments, bodies, start, exports, then the memory
/// count). Where that differs from the reference interpreter's, the first error reported for a module
/// with several faults can differ.
///
/// Precondition (checked): @p module is as decodeModule produces it: every body's block structure
/// is balanced and ends with the final end; every br_table, typed select and try_table names a run
/// within its function's side table (labels, select types, catch clauses); and every v128.const and
/// i8x16.shuffle indexes an entry of Module::v128Literals. A hand-built module that breaks this ends the
/// process through Require rather than failing validation. @p features is closed over its prerequisites
/// (checked), as any set decodeModule accepted is.
/// @param module The decoded module.
/// @param features The proposals whose typing rules apply; those decodeModule accepted.
/// @param limits The implementation limits; the validator enforces maxOperandsPerInstruction.
/// @return The shape of each function's frame when the module is valid; otherwise the first error found.
[[nodiscard]] std::expected<ModuleShape, ValidationError> validateModule(
    binary::Module const& module, FeatureSet const& features, binary::DecodeLimits const& limits = {});

/// Watches the work validation does, for a test or a profiler to bound it: never consulted for what is
/// valid. A seam of the validator for its tests, not part of the embedding API.
class IValidationObserver
{
  public:
    virtual ~IValidationObserver(); ///< Defined in Validator.cpp, which anchors the vtable.

    /// Called for every check of one value type against another, the validator's unit of work.
    virtual void typeCompared() noexcept = 0;
};

/// validateModule, reporting its work to @p observer.
/// @param module The decoded module.
/// @param features The proposals whose typing rules apply; those decodeModule accepted.
/// @param limits The implementation limits; the validator enforces maxOperandsPerInstruction.
/// @param observer What is told of each type comparison.
/// @return As validateModule.
[[nodiscard]] std::expected<ModuleShape, ValidationError> validateModule(binary::Module const& module,
                                                                         FeatureSet const& features,
                                                                         binary::DecodeLimits const& limits,
                                                                         IValidationObserver& observer);

} // namespace isoline::validate
