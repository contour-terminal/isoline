// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>

namespace isoline::c::detail
{

/// The root of every wasm.h object a caller holds through an `own` pointer: what lets a vector of them
/// (wasm_valtype_vec_t and its siblings), and a wasm_val_t holding a reference, delete and deep-copy an
/// element without knowing what it is.
///
/// Deleting an object, through the pointer of any of its wasm.h types, runs its whole destructor; copying
/// it (clone()) gives an object of the same dynamic type, owned by the caller. Objects are neither
/// assigned nor moved: a wasm.h object is reached only through its pointer.
class Object
{
  public:
    virtual ~Object() = default;
    Object& operator=(Object const&) = delete;
    Object& operator=(Object&&) = delete;

    /// A copy, of this object's dynamic type: what wasm.h's wasm_<name>_copy() gives. For a type, a deep
    /// copy; for a reference or an extern, a new handle to the same runtime object, which the store still
    /// owns.
    /// @return The copy, owned by the caller. Throws std::bad_alloc when the host has no memory for it.
    [[nodiscard]] virtual auto clone() const -> std::unique_ptr<Object> = 0;

  protected:
    Object() = default;
    Object(Object const&) = default;
    Object(Object&&) = default;
};

/// A copy of @p object (Object::clone()), as the type it is held as.
/// @param object The object; its dynamic type is @p T or one derived from it, as is its clone's.
/// @return The copy, owned by the caller. Throws std::bad_alloc when the host has no memory for it.
template <typename T>
[[nodiscard]] auto cloneOf(T const& object) -> std::unique_ptr<T>
{
    // clone() gives an object of the dynamic type of @p object (Object's contract), which is a T.
    return std::unique_ptr<T> { static_cast<T*>(object.clone().release()) };
}

} // namespace isoline::c::detail
