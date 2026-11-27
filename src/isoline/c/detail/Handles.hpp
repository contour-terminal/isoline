// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <type_traits>

// Through the umbrella, never <wasm.h> alone: export.h gives WASM_API_EXTERN the C API's export annotation
// first, which a definition after a bare <wasm.h> would lack (and be hidden).
#include <isoline/c/isoline.h>

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

/// A deep copy of @p object, as the type it is held as.
/// @param object The object; its dynamic type is @p T or one derived from it, as is its clone's.
/// @return The copy, owned by the caller. Throws std::bad_alloc when the host has no memory for it.
template <typename T>
[[nodiscard]] auto cloneOf(T const& object) -> std::unique_ptr<T>
{
    // clone() gives an object of the dynamic type of @p object (Object's contract), which is a T.
    return std::unique_ptr<T> { static_cast<T*>(object.clone().release()) };
}

} // namespace isoline::c::detail

// The wasm.h objects a vector holds, or a wasm_val_t refers to. What each one carries is its own task's: the
// types (wasm_valtype_t … wasm_exporttype_t), references and externs, and frames add their members and their
// clone() where they are made. Until then each is abstract, so none can be made by mistake.
//
// The hierarchy is wasm.h's: a cast between two of its types (wasm_functype_as_externtype(),
// wasm_extern_as_ref()) gives the same object, so a type is derived from what it can be cast to.

/// A reference to a runtime object: what a reference value (wasm_val_t of a reference kind) holds.
struct wasm_ref_t: isoline::c::detail::Object
{
};

/// A function, global, table, memory or tag of a store.
struct wasm_extern_t: wasm_ref_t
{
};

/// A frame of a trap's trace. Isoline records no backtraces, so none is ever made (plan P17a, D2).
struct wasm_frame_t: isoline::c::detail::Object
{
};

/// The type of a value.
struct wasm_valtype_t: isoline::c::detail::Object
{
};

/// The type of an extern: one of the five below, as wasm_externtype_kind() tells.
struct wasm_externtype_t: isoline::c::detail::Object
{
};

/// The type of a function.
struct wasm_functype_t: wasm_externtype_t
{
};

/// The type of a global.
struct wasm_globaltype_t: wasm_externtype_t
{
};

/// The type of a table.
struct wasm_tabletype_t: wasm_externtype_t
{
};

/// The type of a memory.
struct wasm_memorytype_t: wasm_externtype_t
{
};

/// The type of a tag.
struct wasm_tagtype_t: wasm_externtype_t
{
};

/// An import of a module: its module name, its name and its type.
struct wasm_importtype_t: isoline::c::detail::Object
{
};

/// An export of a module: its name and its type.
struct wasm_exporttype_t: isoline::c::detail::Object
{
};

// The bases are abstract: no object of one alone is ever made, and so none is copied from a derived object
// by slicing (copying a wasm_externtype_t out of a wasm_functype_t would lose what makes it a function type).
static_assert(std::is_abstract_v<isoline::c::detail::Object>);
static_assert(std::is_abstract_v<wasm_ref_t>);
static_assert(std::is_abstract_v<wasm_extern_t>);
static_assert(std::is_abstract_v<wasm_externtype_t>);
static_assert(std::is_abstract_v<wasm_frame_t>);
