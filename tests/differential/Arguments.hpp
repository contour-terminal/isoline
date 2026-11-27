// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::differential
{

/// A deterministic stream of 64-bit numbers (SplitMix64): the arguments' source, the same on every host.
class ArgumentStream
{
  public:
    /// The stream of @p seed's bytes, for the call @p ordinal of a run.
    /// @param seed The argument seed.
    /// @param ordinal The call's ordinal in the run.
    ArgumentStream(std::span<std::byte const> seed, uint64_t ordinal) noexcept;

    /// The next number.
    /// @return It.
    [[nodiscard]] uint64_t next() noexcept;

  private:
    uint64_t _state;
};

/// What the arguments of a call may aim at: the size of the instance's memory 0, so that an address is
/// near its bounds.
struct ArgumentContext
{
    uint64_t memoryBytes = 0; ///< Memory 0's size in bytes; 0 when the instance has none.
};

/// The arguments of one call, derived from @p stream: per parameter, one of random bits, an edge value of
/// its type (the tables of Arguments.cpp: zero, the extremes, NaNs of each sign and payload, conversion
/// bounds), an address at most 16 bytes from memory 0's end, or a count that reaches the store's limits
/// (call depth, steps), so that a generated program reaches the hard edges. A reference parameter is the
/// null of its heap.
/// @param stream The numbers.
/// @param params The parameter types.
/// @param context What an address aims at.
/// @return The arguments, one per parameter.
[[nodiscard]] std::vector<runtime::Value> argumentsFor(ArgumentStream& stream,
                                                       std::span<binary::ValueType const> params,
                                                       ArgumentContext const& context);

} // namespace isoline::differential
