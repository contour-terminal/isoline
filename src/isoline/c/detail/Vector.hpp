// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

// Through the umbrella, never <wasm.h> alone: export.h gives WASM_API_EXTERN the C API's export annotation
// first, which a definition after a bare <wasm.h> would lack (and be hidden).
#include <isoline/c/detail/Handles.hpp>
#include <isoline/c/isoline.h>

// The operations of wasm.h's vectors (WASM_DECLARE_VEC), one template per operation for every family.
// WasmVectors.cpp, generated from detail/VectorRows.def by scripts/c-api-vectors.py, defines each family's
// five functions as one call of these each.
//
// A vector's size comes from the host, so it is checked before anything is allocated or read: no vector holds
// more than PTRDIFF_MAX bytes, a size past that names no array there could be, and a vector's data is
// allocated here and freed here only. What wasm.h leaves open is defined, and never an abort:
//
//   - a size that cannot be allocated (too large, or the host is out of memory) gives the empty vector,
//     { 0, NULL }, wasm.h's own;
//   - vec_new() takes the elements it is given, as wasm.h's `own` says, even when it refuses (no vector to
//     make, or none of that size can be allocated): it deletes them then. Elements it cannot walk, a size
//     past MaxVectorSize or a nonzero size with no data, it leaves untouched;
//   - a vector with a size and no data is the empty vector to copy() and to vec_new(), and nothing to delete;
//   - delete leaves the empty vector behind, so deleting a vector again, or a zeroed one, is a no-op;
//   - where no vector is given (out is NULL) nothing is written.
//
// A vector owns its elements as wasm.h says: delete deletes them, copy deep-copies them, and vec_new takes
// the ones it is given (ElementOwnership).

namespace isoline::c::detail
{

/// What a vector's elements own, and so what copying and deleting the vector does to them.
enum class ElementOwnership : uint8_t
{
    Plain, ///< Nothing: bytes, copied by value and gone with the vector.
    Value, ///< A wasm_val_t, which owns the reference it holds when it is of a reference kind.
    Owned, ///< An own pointer to an Object, or null for a slot not yet filled: deleted with the vector,
           ///< deep-copied with it.
};

/// The kinds of wasm_val_t whose of.ref is a reference the value owns. A kind that is no row (a number, or
/// one no task knows) owns nothing, so its bits are never taken for a pointer.
inline constexpr auto ReferenceValueKinds = std::to_array<wasm_valkind_t>({ WASM_EXTERNREF, WASM_FUNCREF });

/// Whether a value of kind @p kind owns the reference of.ref holds.
/// @param kind The value's kind.
/// @return Whether @p kind is one of ReferenceValueKinds.
[[nodiscard]] constexpr auto holdsReference(wasm_valkind_t kind) noexcept -> bool
{
    return std::ranges::find(ReferenceValueKinds, kind) != ReferenceValueKinds.end();
}

/// The element type of the vector type @p Vector: what its data points to.
template <typename Vector>
using ElementOf = std::remove_pointer_t<decltype(Vector::data)>;

/// The most elements of type @p Element a vector holds: PTRDIFF_MAX bytes of them, so that no size of an
/// allocated vector overflows a byte count or a pointer difference.
template <typename Element>
inline constexpr auto MaxVectorSize =
    static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(Element);

/// A copy of @p element that owns what an element of its ownership owns: a deep copy of the object or the
/// reference it points to, or the element itself.
/// @param element The element.
/// @return The copy. Throws std::bad_alloc when the host has no memory for it.
template <ElementOwnership Ownership, typename Element>
[[nodiscard]] auto copyElement(Element const& element) -> Element
{
    if constexpr (Ownership == ElementOwnership::Owned)
        return element == nullptr ? nullptr : cloneOf(*element).release();
    else if constexpr (Ownership == ElementOwnership::Value)
    {
        auto copy = element;
        if (holdsReference(element.kind) && element.of.ref != nullptr)
            copy.of.ref = cloneOf(*element.of.ref).release();
        return copy;
    }
    else
        return element;
}

/// Deletes what @p element owns, and leaves it owning nothing.
/// @param element The element.
template <ElementOwnership Ownership, typename Element>
void destroyElement(Element& element) noexcept
{
    if constexpr (Ownership == ElementOwnership::Owned)
    {
        delete element;
        element = nullptr;
    }
    else if constexpr (Ownership == ElementOwnership::Value)
    {
        if (holdsReference(element.kind))
        {
            delete element.of.ref;
            element.of.ref = nullptr;
        }
    }
}

/// The bytes @p size elements of type @p Element take, checked: none when the size is past MaxVectorSize, so
/// the product never wraps.
/// @param size The elements.
/// @return Their bytes, or std::nullopt.
template <typename Element>
[[nodiscard]] constexpr auto vectorBytes(std::size_t size) noexcept -> std::optional<std::size_t>
{
    if (size > MaxVectorSize<Element>)
        return std::nullopt;
    return size * sizeof(Element);
}

/// What the elements of freshly allocated vector data hold.
enum class FreshElements : uint8_t
{
    Zeroed,    ///< Each element zero (null, for pointers), owning nothing.
    Unwritten, ///< Indeterminate: for data whose every element is written before anything reads or deletes
               ///< it.
};

/// The data of a vector of @p size elements: allocated here, and freed by freeVectorData() only.
/// @param size The elements; 0, a size vectorBytes() refuses, or one the host cannot allocate gives none.
/// @param fresh What the elements hold: zero, owning nothing, or nothing written yet.
/// @return The data, or null.
template <typename Element>
[[nodiscard]] auto allocateVectorData(std::size_t size, FreshElements fresh) noexcept -> Element*
{
    // Bytes, null pointers and wasm_val_t are implicit-lifetime types, which the allocation itself creates.
    static_assert(std::is_trivially_default_constructible_v<Element>
                  && std::is_trivially_destructible_v<Element>);
    auto const bytes = vectorBytes<Element>(size);
    if (size == 0 || !bytes.has_value())
        return nullptr;
    auto* const data = static_cast<Element*>(::operator new(*bytes, std::nothrow));
    if (data != nullptr && fresh == FreshElements::Zeroed)
        std::uninitialized_value_construct_n(data, size);
    return data;
}

/// Frees what allocateVectorData() gave.
/// @param data The data; null is nothing to free.
template <typename Element>
void freeVectorData(Element* data) noexcept
{
    ::operator delete(static_cast<void*>(data));
}

/// An array of elements being filled, which deletes its elements and itself unless it is released: what a
/// copy holds while it can still fail.
template <ElementOwnership Ownership, typename Element>
class ElementArray
{
  public:
    /// Allocates @p size elements.
    /// @param size The elements wanted; one more than MaxVectorSize, or more than the host can allocate,
    ///             gives no array (empty()).
    /// @param fresh What they hold: zero, owning nothing; or nothing yet, in which case every element is
    ///              written before the array is released or destroyed, which deletes what they own.
    ElementArray(std::size_t size, FreshElements fresh) noexcept:
        _data { allocateVectorData<Element>(size, fresh) }, _size { _data == nullptr ? 0 : size }
    {
    }

    ~ElementArray()
    {
        for (auto& element: elements())
            destroyElement<Ownership>(element);
        freeVectorData(_data);
    }

    ElementArray(ElementArray const&) = delete;
    ElementArray(ElementArray&&) = delete;
    ElementArray& operator=(ElementArray const&) = delete;
    ElementArray& operator=(ElementArray&&) = delete;

    /// Whether no array was allocated.
    [[nodiscard]] auto empty() const noexcept -> bool { return _data == nullptr; }

    /// The elements.
    [[nodiscard]] auto elements() const noexcept -> std::span<Element> { return { _data, _size }; }

    /// Hands the array, and what its elements own, to @p out.
    /// @param out The vector that owns them from now on.
    template <typename Vector>
    void releaseInto(Vector& out) noexcept
    {
        out.size = std::exchange(_size, 0);
        out.data = std::exchange(_data, nullptr);
    }

  private:
    Element* _data;    ///< The array, or null.
    std::size_t _size; ///< Its elements.
};

/// Makes @p out the empty vector, { 0, NULL }.
/// @param out The vector; null is nothing to do.
template <typename Vector>
void emptyVector(Vector* out) noexcept
{
    if (out == nullptr)
        return;
    out->size = 0;
    out->data = nullptr;
}

/// wasm_<name>_vec_new_uninitialized(): a vector of @p size elements, each zero (null, for pointers), which
/// own nothing until the caller fills them.
/// @param out The vector made; null is nothing to do.
/// @param size Its size; one that cannot be allocated gives the empty vector.
template <ElementOwnership Ownership, typename Vector>
void newUninitializedVector(Vector* out, std::size_t size) noexcept
{
    if (out == nullptr)
        return;
    auto array = ElementArray<Ownership, ElementOf<Vector>> { size, FreshElements::Zeroed };
    array.releaseInto(*out);
}

/// Deletes what the @p size elements at @p data own: what a function that takes elements does with them when
/// it refuses. Elements it cannot walk (a size past MaxVectorSize, or null data) it leaves untouched.
/// @param size The elements.
/// @param data Their array, the caller's, which itself is not freed.
template <ElementOwnership Ownership, typename Element>
void destroyGivenElements(std::size_t size, Element const* data) noexcept
{
    if (data == nullptr || size > MaxVectorSize<Element>)
        return;
    for (auto element: std::span { data, size })
        destroyElement<Ownership>(element);
}

/// wasm_<name>_vec_new(): a vector of the @p size elements at @p data, which it takes: what they own is the
/// vector's from now on, or, when it refuses, deleted.
/// @param out The vector made; null makes none, and deletes the elements.
/// @param size Its size; one that cannot be allocated gives the empty vector, and deletes the elements. One
///             past MaxVectorSize leaves them untouched: no array of that size can be walked.
/// @param data The elements; null, with a nonzero size, gives the empty vector.
template <ElementOwnership Ownership, typename Vector>
void newVector(Vector* out, std::size_t size, ElementOf<Vector> const* data) noexcept
{
    // Every element is written at once, from the caller's, so nothing is zeroed first.
    auto array = ElementArray<Ownership, ElementOf<Vector>> { out == nullptr || data == nullptr ? 0 : size,
                                                              FreshElements::Unwritten };
    if (array.empty())
    {
        destroyGivenElements<Ownership>(size, data);
        emptyVector(out);
        return;
    }
    // The array exists, so its size was checked, and the caller's has as many elements. Copying them cannot
    // throw, so no element is left unwritten when the array is released.
    std::ranges::copy(std::span { data, size }, array.elements().begin());
    array.releaseInto(*out);
}

/// wasm_<name>_vec_copy(): a copy of @p source, each element copied as its ownership says.
/// @param out The copy; null is nothing to do. It may be @p source.
/// @param source The vector copied; null, or one with no data, gives the empty vector.
/// Throws std::bad_alloc when an element cannot be copied, having deleted what it copied so far and left
/// @p out as it was (the wasm_ function's boundary() then makes it the empty vector).
template <ElementOwnership Ownership, typename Vector>
void copyVector(Vector* out, Vector const* source)
{
    if (out == nullptr)
        return;
    if (source == nullptr || source->data == nullptr)
    {
        emptyVector(out);
        return;
    }
    // Zeroed, so that a copy failing midway deletes only the elements copied so far.
    auto array = ElementArray<Ownership, ElementOf<Vector>> { source->size, FreshElements::Zeroed };
    if (array.empty())
    {
        emptyVector(out);
        return;
    }
    std::ranges::transform(std::span { source->data, source->size },
                           array.elements().begin(),
                           [](auto const& element) { return copyElement<Ownership>(element); });
    array.releaseInto(*out);
}

/// wasm_<name>_vec_delete(): deletes @p vector's elements, as their ownership says, and its data, and leaves
/// the empty vector behind.
/// @param vector The vector; null, or one with no data, is nothing to delete.
template <ElementOwnership Ownership, typename Vector>
void deleteVector(Vector* vector) noexcept
{
    if (vector == nullptr)
        return;
    if (vector->data != nullptr)
    {
        for (auto& element: std::span { vector->data, vector->size })
            destroyElement<Ownership>(element);
        freeVectorData(vector->data);
    }
    emptyVector(vector);
}

} // namespace isoline::c::detail
