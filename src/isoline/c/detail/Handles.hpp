// SPDX-License-Identifier: Apache-2.0
//
// The wasm.h objects a caller holds through an `own` pointer, and that a vector holds or a wasm_val_t refers
// to: the types (wasm_valtype_t … wasm_exporttype_t), references and externs, and frames. Each is an Object
// (detail/Object.hpp), so that a vector deletes and copies it without knowing what it is.
//
// The hierarchy is wasm.h's: a cast between two of its types (wasm_functype_as_externtype(),
// wasm_extern_as_ref()) gives the same object, so a type is derived from what it can be cast to. A base is
// abstract, and an object a caller gets is of a final type, so nothing is ever sliced. What references,
// externs and frames carry is their own task's: until then each is abstract, so none can be made by mistake.
//
// A type is immutable once made, and owns what it is made of: a function type its parameters' and results'
// value types, an import its names and its extern type. Copying one copies all of it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

#include <isoline/binary/Types.hpp>
#include <isoline/c/detail/Object.hpp>
#include <isoline/c/detail/Vector.hpp>
// Through the umbrella, never <wasm.h> alone: export.h gives WASM_API_EXTERN the C API's export annotation
// first, which a definition after a bare <wasm.h> would lack (and be hidden).
#include <isoline/c/isoline.h>

namespace isoline::c::detail
{

/// The vector of value types a function type owns.
using ValueTypeVector = OwnedVector<ElementOwnership::Owned, wasm_valtype_vec_t>;

/// The byte vector of a name an import or export type owns.
using NameVector = OwnedVector<ElementOwnership::Plain, wasm_name_t>;

/// @p limits as wasm_limits_t holds them: a size past UINT32_MAX, and a maximum there is none of, are
/// wasm_limits_max_default. The one clamp to 32 bits; the types hold its result for wasm.h's accessors.
[[nodiscard]] constexpr auto toCLimits(binary::Limits const& limits) noexcept -> wasm_limits_t
{
    constexpr auto Largest = uint64_t { wasm_limits_max_default };
    return wasm_limits_t { .min = static_cast<uint32_t>(std::min(limits.min, Largest)),
                           .max = static_cast<uint32_t>(std::min(limits.max.value_or(Largest), Largest)) };
}

} // namespace isoline::c::detail

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

/// The type of a value: any value type a module may declare (binary::ValueType), the bottom heap excepted.
struct wasm_valtype_t final: isoline::c::detail::Object
{
  public:
    /// @param type The type; its heap, if it is a reference type, is one of detail::HeapRows.
    explicit wasm_valtype_t(isoline::binary::ValueType const& type) noexcept: _type { type } {}

    /// The type.
    [[nodiscard]] auto type() const noexcept -> isoline::binary::ValueType const& { return _type; }

    [[nodiscard]] auto clone() const -> std::unique_ptr<isoline::c::detail::Object> override
    {
        return std::make_unique<wasm_valtype_t>(*this);
    }

  private:
    isoline::binary::ValueType _type; ///< The type.
};

/// The type of an extern: one of the five below, as wasm_externtype_kind() tells.
struct wasm_externtype_t: isoline::c::detail::Object
{
  public:
    /// What kind of extern it is the type of: what its final type is.
    [[nodiscard]] virtual auto kind() const noexcept -> isoline::binary::ExternKind = 0;
};

/// The type of a function: its parameters' and its results' value types.
struct wasm_functype_t final: wasm_externtype_t
{
  public:
    /// Takes the elements of @p params and @p results, well-formed vectors of value types, none null, and
    /// leaves them empty.
    wasm_functype_t(wasm_valtype_vec_t& params, wasm_valtype_vec_t& results) noexcept:
        _params { params }, _results { results }
    {
    }

    /// Takes @p params and @p results, vectors of value types, none null.
    wasm_functype_t(isoline::c::detail::ValueTypeVector&& params,
                    isoline::c::detail::ValueTypeVector&& results) noexcept:
        _params { std::move(params) }, _results { std::move(results) }
    {
    }

    /// The parameters' types, owned by the function type.
    [[nodiscard]] auto params() const noexcept -> wasm_valtype_vec_t const& { return _params.get(); }
    /// The results' types, owned by the function type.
    [[nodiscard]] auto results() const noexcept -> wasm_valtype_vec_t const& { return _results.get(); }

    [[nodiscard]] auto kind() const noexcept -> isoline::binary::ExternKind override
    {
        return isoline::binary::ExternKind::Func;
    }

    [[nodiscard]] auto clone() const -> std::unique_ptr<isoline::c::detail::Object> override
    {
        return std::make_unique<wasm_functype_t>(*this);
    }

  private:
    isoline::c::detail::ValueTypeVector _params;  ///< The parameters' types.
    isoline::c::detail::ValueTypeVector _results; ///< The results' types.
};

/// The type of a global: its value type and its mutability.
struct wasm_globaltype_t final: wasm_externtype_t
{
  public:
    /// @param content The type of its value, never null, which it takes. A pointer, not a
    ///                std::unique_ptr: the global type takes it only once it exists, so a caller whose
    ///                allocation of it fails keeps its argument.
    /// @param mutability Whether it may be assigned.
    wasm_globaltype_t(wasm_valtype_t* content, isoline::binary::Mutability mutability) noexcept:
        _content { content }, _mutability { mutability }
    {
    }

    /// A deep copy of @p other. Throws std::bad_alloc when the host has no memory for it.
    wasm_globaltype_t(wasm_globaltype_t const& other):
        wasm_externtype_t(other),
        _content { isoline::c::detail::cloneOf(*other._content) },
        _mutability { other._mutability }
    {
    }
    wasm_globaltype_t(wasm_globaltype_t&&) = delete;
    ~wasm_globaltype_t() override = default;

    /// The type of its value, owned by the global type.
    [[nodiscard]] auto content() const noexcept -> wasm_valtype_t const& { return *_content; }
    /// Whether it may be assigned.
    [[nodiscard]] auto mutability() const noexcept -> isoline::binary::Mutability { return _mutability; }

    [[nodiscard]] auto kind() const noexcept -> isoline::binary::ExternKind override
    {
        return isoline::binary::ExternKind::Global;
    }

    [[nodiscard]] auto clone() const -> std::unique_ptr<isoline::c::detail::Object> override
    {
        return std::make_unique<wasm_globaltype_t>(*this);
    }

  private:
    std::unique_ptr<wasm_valtype_t> _content; ///< The type of its value.
    isoline::binary::Mutability _mutability;  ///< Whether it may be assigned.
};

/// The type of a table: its elements' reference type and its size bounds, at any address type.
struct wasm_tabletype_t final: wasm_externtype_t
{
  public:
    /// @param element Its elements' type, a reference type, never null, which it takes.
    /// @param limits Its size bounds, valid for their address type (detail::checkTableLimits()).
    wasm_tabletype_t(wasm_valtype_t* element, isoline::binary::Limits const& limits) noexcept:
        _element { element }, _limits { limits }, _clampedLimits { isoline::c::detail::toCLimits(limits) }
    {
    }

    /// A deep copy of @p other. Throws std::bad_alloc when the host has no memory for it.
    wasm_tabletype_t(wasm_tabletype_t const& other):
        wasm_externtype_t(other),
        _element { isoline::c::detail::cloneOf(*other._element) },
        _limits { other._limits },
        _clampedLimits { other._clampedLimits }
    {
    }
    wasm_tabletype_t(wasm_tabletype_t&&) = delete;
    ~wasm_tabletype_t() override = default;

    /// Its elements' type, owned by the table type.
    [[nodiscard]] auto element() const noexcept -> wasm_valtype_t const& { return *_element; }
    /// Its size bounds, exactly.
    [[nodiscard]] auto limits() const noexcept -> isoline::binary::Limits const& { return _limits; }
    /// Its size bounds as wasm_limits_t holds them, each past UINT32_MAX clamped (detail::toCLimits()).
    [[nodiscard]] auto clampedLimits() const noexcept -> wasm_limits_t const& { return _clampedLimits; }
    [[nodiscard]] auto kind() const noexcept -> isoline::binary::ExternKind override
    {
        return isoline::binary::ExternKind::Table;
    }

    [[nodiscard]] auto clone() const -> std::unique_ptr<isoline::c::detail::Object> override
    {
        return std::make_unique<wasm_tabletype_t>(*this);
    }

  private:
    std::unique_ptr<wasm_valtype_t> _element; ///< Its elements' type.
    isoline::binary::Limits _limits;          ///< Its size bounds.
    wasm_limits_t _clampedLimits;             ///< The same, clamped to 32 bits.
};

/// The type of a memory: its size bounds and page size, at any address type.
struct wasm_memorytype_t final: wasm_externtype_t
{
  public:
    /// @param type The type, valid (detail::checkMemoryType()).
    explicit wasm_memorytype_t(isoline::binary::MemoryType const& type) noexcept:
        _type { type }, _clampedLimits { isoline::c::detail::toCLimits(type.limits) }
    {
    }

    /// The type, exactly.
    [[nodiscard]] auto type() const noexcept -> isoline::binary::MemoryType const& { return _type; }
    /// Its size bounds as wasm_limits_t holds them, each past UINT32_MAX clamped (detail::toCLimits()).
    [[nodiscard]] auto clampedLimits() const noexcept -> wasm_limits_t const& { return _clampedLimits; }

    [[nodiscard]] auto kind() const noexcept -> isoline::binary::ExternKind override
    {
        return isoline::binary::ExternKind::Memory;
    }

    [[nodiscard]] auto clone() const -> std::unique_ptr<isoline::c::detail::Object> override
    {
        return std::make_unique<wasm_memorytype_t>(*this);
    }

  private:
    isoline::binary::MemoryType _type; ///< The type.
    wasm_limits_t _clampedLimits;      ///< Its size bounds, clamped to 32 bits.
};

/// The type of a tag: the function type whose parameters are its exceptions' payload, of no results.
struct wasm_tagtype_t final: wasm_externtype_t
{
  public:
    /// @param functype The function type, of no results, never null, which it takes.
    explicit wasm_tagtype_t(wasm_functype_t* functype) noexcept: _functype { functype } {}

    /// A deep copy of @p other. Throws std::bad_alloc when the host has no memory for it.
    wasm_tagtype_t(wasm_tagtype_t const& other):
        wasm_externtype_t(other), _functype { isoline::c::detail::cloneOf(*other._functype) }
    {
    }
    wasm_tagtype_t(wasm_tagtype_t&&) = delete;
    ~wasm_tagtype_t() override = default;

    /// The function type, owned by the tag type.
    [[nodiscard]] auto functype() const noexcept -> wasm_functype_t const& { return *_functype; }

    [[nodiscard]] auto kind() const noexcept -> isoline::binary::ExternKind override
    {
        return isoline::binary::ExternKind::Tag;
    }

    [[nodiscard]] auto clone() const -> std::unique_ptr<isoline::c::detail::Object> override
    {
        return std::make_unique<wasm_tagtype_t>(*this);
    }

  private:
    std::unique_ptr<wasm_functype_t> _functype; ///< The function type.
};

/// An import of a module: its module name, its name and its type.
struct wasm_importtype_t final: isoline::c::detail::Object
{
  public:
    /// Takes @p module and @p name, well-formed byte vectors, leaving them empty, and @p type, never null.
    wasm_importtype_t(wasm_name_t& module, wasm_name_t& name, wasm_externtype_t* type) noexcept:
        _module { module }, _name { name }, _type { type }
    {
    }

    /// A deep copy of @p other. Throws std::bad_alloc when the host has no memory for it.
    wasm_importtype_t(wasm_importtype_t const& other):
        Object(other),
        _module { other._module },
        _name { other._name },
        _type { isoline::c::detail::cloneOf(*other._type) }
    {
    }
    wasm_importtype_t(wasm_importtype_t&&) = delete;
    ~wasm_importtype_t() override = default;

    /// The name of the module it imports from, owned by the import type.
    [[nodiscard]] auto module() const noexcept -> wasm_name_t const& { return _module.get(); }
    /// Its name within that module, owned by the import type.
    [[nodiscard]] auto name() const noexcept -> wasm_name_t const& { return _name.get(); }
    /// The type it requires, owned by the import type.
    [[nodiscard]] auto type() const noexcept -> wasm_externtype_t const& { return *_type; }

    [[nodiscard]] auto clone() const -> std::unique_ptr<isoline::c::detail::Object> override
    {
        return std::make_unique<wasm_importtype_t>(*this);
    }

  private:
    isoline::c::detail::NameVector _module;   ///< The module's name.
    isoline::c::detail::NameVector _name;     ///< Its name.
    std::unique_ptr<wasm_externtype_t> _type; ///< Its type.
};

/// An export of a module: its name and its type.
struct wasm_exporttype_t final: isoline::c::detail::Object
{
  public:
    /// Takes @p name, a well-formed byte vector, leaving it empty, and @p type, never null.
    wasm_exporttype_t(wasm_name_t& name, wasm_externtype_t* type) noexcept: _name { name }, _type { type } {}

    /// A deep copy of @p other. Throws std::bad_alloc when the host has no memory for it.
    wasm_exporttype_t(wasm_exporttype_t const& other):
        Object(other), _name { other._name }, _type { isoline::c::detail::cloneOf(*other._type) }
    {
    }
    wasm_exporttype_t(wasm_exporttype_t&&) = delete;
    ~wasm_exporttype_t() override = default;

    /// Its name, owned by the export type.
    [[nodiscard]] auto name() const noexcept -> wasm_name_t const& { return _name.get(); }
    /// The type of what it exports, owned by the export type.
    [[nodiscard]] auto type() const noexcept -> wasm_externtype_t const& { return *_type; }

    [[nodiscard]] auto clone() const -> std::unique_ptr<isoline::c::detail::Object> override
    {
        return std::make_unique<wasm_exporttype_t>(*this);
    }

  private:
    isoline::c::detail::NameVector _name;     ///< Its name.
    std::unique_ptr<wasm_externtype_t> _type; ///< Its type.
};

// The bases are abstract: no object of one alone is ever made, and so none is copied from a derived object
// by slicing (copying a wasm_externtype_t out of a wasm_functype_t would lose what makes it a function type).
// What a caller holds is of a final type.
static_assert(std::is_abstract_v<isoline::c::detail::Object>);
static_assert(std::is_abstract_v<wasm_ref_t>);
static_assert(std::is_abstract_v<wasm_extern_t>);
static_assert(std::is_abstract_v<wasm_externtype_t>);
static_assert(std::is_abstract_v<wasm_frame_t>);
static_assert(std::is_final_v<wasm_valtype_t> && std::is_final_v<wasm_functype_t>
              && std::is_final_v<wasm_globaltype_t> && std::is_final_v<wasm_tabletype_t>
              && std::is_final_v<wasm_memorytype_t> && std::is_final_v<wasm_tagtype_t>
              && std::is_final_v<wasm_importtype_t> && std::is_final_v<wasm_exporttype_t>);
