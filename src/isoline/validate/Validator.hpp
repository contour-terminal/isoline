// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <vector>

#include <isoline/Features.hpp>
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
/// of the input costs memory linear in it, not native stack.
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
/// @return The shape of each function's frame when the module is valid; otherwise the first error found.
[[nodiscard]] std::expected<ModuleShape, ValidationError> validateModule(binary::Module const& module,
                                                                         FeatureSet const& features);

} // namespace isoline::validate
