// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include <isoline/runtime/Trap.hpp>

namespace isoline::runtime
{

/// The address of a tag instance in its store. Tags are compared by address: two imports of one tag
/// are the same tag, and two instances of one module's tag are two.
using TagAddr = uint32_t;

/// The address of an exception instance in its store, which an exnref refers to.
using ExnAddr = uint32_t;

/// How values crossing the host boundary fail to be of the types they must be (checkValues).
enum class MismatchKind : uint8_t
{
    Count,            ///< There are more or fewer values than types.
    Kind,             ///< A value is not of its type's kind: a number where a reference is due, say.
    Heap,             ///< A reference is outside its type's heap: an externref for a funcref, a function
                      ///< of another type for a (ref $t), a null of another hierarchy.
    NotNullable,      ///< A null reference where the type admits none.
    ForeignReference, ///< A reference to a function or exception this store does not hold.
};

/// The first value that is not of its type: what is wrong, and where.
struct ValueMismatch
{
    MismatchKind kind = MismatchKind::Count; ///< What is wrong with it.
    /// Its position among the values; for Count, the first position one of the two lists lacks.
    uint32_t index = 0;

    /// Whether both name the same mismatch at the same position.
    bool operator==(ValueMismatch const&) const = default;
};

/// An exception thrown out of a function: no handler on the way caught it.
struct Thrown
{
    ExnAddr exception = 0; ///< The exception, in the store that executed the function.

    bool operator==(Thrown const&) const = default;
};

/// Why a host function failed (HostFailure).
enum class HostFailureKind : uint8_t
{
    Embedder,         ///< The host function failed on its own account; HostFailure::code says why.
    ResultMismatch,   ///< A host function's results, or the exception it threw, are not of its type.
    ArgumentMismatch, ///< A host passed a call or an exception values not of their types, or an address
                      ///< the store does not hold.
};

/// A host function's failure: one it reported, or one the host boundary found in what it handed over.
/// It ends the invocation, as a trap does, and no handler catches it.
struct HostFailure
{
    HostFailureKind kind = HostFailureKind::Embedder; ///< Whose failure, and of what.
    uint32_t code = 0; ///< The embedder's own code, for Embedder; 0 for the others.
    /// The first value that is not of its type, for a mismatch of values; nullopt for Embedder, for an
    /// address the store does not hold, and for a thrown exception the store never allocated.
    std::optional<ValueMismatch> mismatch;

    /// Whether both are the same failure.
    bool operator==(HostFailure const&) const = default;
};

/// How execution ended abruptly: by a trap, by an exception no handler caught, or by a host
/// function's failure.
using Abrupt = std::variant<Trap, Thrown, HostFailure>;

} // namespace isoline::runtime
