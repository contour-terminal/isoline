// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <variant>

#include <isoline/runtime/Trap.hpp>

namespace isoline::runtime
{

/// The address of a tag instance in its store. Tags are compared by address: two imports of one tag
/// are the same tag, and two instances of one module's tag are two.
using TagAddr = uint32_t;

/// The address of an exception instance in its store, which an exnref refers to.
using ExnAddr = uint32_t;

/// An exception thrown out of a function: no handler on the way caught it.
struct Thrown
{
    ExnAddr exception = 0; ///< The exception, in the store that executed the function.

    bool operator==(Thrown const&) const = default;
};

/// How execution ended abruptly: by a trap, or by an exception no handler caught.
using Abrupt = std::variant<Trap, Thrown>;

} // namespace isoline::runtime
