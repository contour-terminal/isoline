// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "FuzzTargets.hpp"

// Seeds the load and text targets start from: the shapes linear lowering must stay linear on (C1), and
// modules of the generator. The unit tests run the hostile shapes at full size; isoline-fuzz-seeds writes
// them scaled down to a fuzzer's -max_len, from where libFuzzer mutates them.

namespace isoline::fuzz
{

/// How large a hostile shape is built.
enum class SeedScale : uint8_t
{
    Full,   ///< At the decode limits: what the unit test lowers.
    Corpus, ///< Within the load target's -max_len (a few hundred KiB): a seed to mutate.
};

/// One hostile shape: its name, its binary at a scale, and what the load target must make of it at full size.
struct HostileSeed
{
    std::string_view name;        ///< What it is shaped as.
    std::vector<std::byte> bytes; ///< Its binary.
    LoadVerdict expected;         ///< runLoad()'s verdict at SeedScale::Full, within LoadCompileBudget.
};

/// Linear lowering's hostile shapes (C1), one row each: 50'000 locals, a 7 MB body, 100'000 nested blocks,
/// loops and ifs, 1'000-value blocks branched to and re-entered, a br_table of the most labels, exnref locals
/// and nested try_tables around calls, and a lying count.
/// @param scale How large.
/// @return The shapes.
[[nodiscard]] std::vector<HostileSeed> hostileSeeds(SeedScale scale);

/// @p count modules of the generator, from fixed entropy: seeds of the load and text targets.
/// @param count How many.
/// @param mode Whom they are for.
/// @return Their binaries.
[[nodiscard]] std::vector<std::vector<std::byte>> generatedSeeds(uint32_t count, GeneratorMode mode);

} // namespace isoline::fuzz
