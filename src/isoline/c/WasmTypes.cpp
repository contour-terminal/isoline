// SPDX-License-Identifier: Apache-2.0
//
// wasm.h's types, and val.h's isoline_ functions of them.
//
// What wasm.h leaves open is defined, and never an abort: a NULL where a type is due gives NULL (or the
// value val.h names for it), and a type no module could declare (an unknown kind or mutability, a table of
// numbers, a minimum above its maximum, a size past what the address type reaches, a tag with results) is
// refused. A wasm_ function takes its `own` arguments as wasm.h says, also when it refuses, and then deletes
// them: so wasm.h's inline helpers (wasm_functype_new_1_1(), ...) leak nothing either. What it cannot walk, a
// vector of a size no vector has or of a size and no data, it leaves untouched (isoline.h). An isoline_
// function leaves its arguments untouched on an error. Out of host memory, a type made or copied is NULL.
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>

#include <isoline/binary/Types.hpp>
#include <isoline/c/detail/Boundary.hpp>
#include <isoline/c/detail/Convert.hpp>
#include <isoline/c/detail/Error.hpp>
#include <isoline/c/detail/Handles.hpp>
#include <isoline/c/detail/Object.hpp>
#include <isoline/c/detail/Vector.hpp>
#include <isoline/c/isoline.h>
#include <isoline/c/val.h>

namespace binary = isoline::binary;
namespace detail = isoline::c::detail;

using detail::TypeRefusal;

namespace
{

/// A copy of @p type, owned by the caller: wasm_<name>_copy().
/// @param type The type; null gives null.
/// @return The copy. Throws std::bad_alloc when the host has no memory for it.
template <typename T>
[[nodiscard]] auto copyOf(T const* type) -> T*
{
    return type == nullptr ? nullptr : detail::cloneOf(*type).release();
}

/// @p type as the extern type of kind @p T it is: wasm_externtype_as_<name>().
/// @param type The extern type; null gives null.
/// @return It as a @p T; null when it is of another kind.
template <typename T, typename ExternType>
[[nodiscard]] auto externTypeAs(ExternType* type) noexcept -> T*
{
    return dynamic_cast<T*>(type);
}

/// Whether @p vector is a vector of value types a function type may take: well formed, no element null.
[[nodiscard]] auto isValueTypeList(wasm_valtype_vec_t const& vector) noexcept -> bool
{
    return detail::isWellFormed(vector)
           && std::ranges::none_of(std::span { vector.data, vector.size },
                                   [](wasm_valtype_t const* element) { return element == nullptr; });
}

/// Whether @p type is a reference type: what a table's elements are.
[[nodiscard]] auto isReference(wasm_valtype_t const& type) noexcept -> bool
{
    return type.type().kind == binary::ValueKind::Ref;
}

/// A table type of @p element and @p limits, which takes @p element.
/// @return The type, owned by the caller; the refusal of @p limits, with @p element still the caller's.
[[nodiscard]] auto newTableType(wasm_valtype_t* element, binary::Limits const& limits)
    -> std::expected<wasm_tabletype_t*, TypeRefusal>
{
    if (!isReference(*element))
        return std::unexpected { TypeRefusal::NonReferenceElement };
    return detail::checkTableLimits(limits).transform([&](binary::Limits const& checked) {
        return std::make_unique<wasm_tabletype_t>(element, checked).release();
    });
}

/// A memory type of @p type.
/// @return The type, owned by the caller; the refusal of @p type.
[[nodiscard]] auto newMemoryType(binary::MemoryType const& type)
    -> std::expected<wasm_memorytype_t*, TypeRefusal>
{
    return detail::checkMemoryType(type).transform([](binary::MemoryType const& checked) {
        return std::make_unique<wasm_memorytype_t>(checked).release();
    });
}

/// The isoline_ error of @p outcome: none when it succeeded, having written its value to @p out.
template <typename T>
[[nodiscard]] auto reportInto(std::expected<T*, TypeRefusal> const& outcome, T** out) noexcept
    -> isoline_error_t*
{
    if (!outcome.has_value())
        return detail::typeRefusalError(outcome.error());
    *out = *outcome;
    return nullptr;
}

/// Deletes the vector of value types @p vector, what a refused wasm_ function was given to take; one it
/// cannot walk (detail::isWellFormed()) it leaves untouched.
void discard(wasm_valtype_vec_t* vector) noexcept
{
    if (vector != nullptr && detail::isWellFormed(*vector))
        detail::deleteVector<detail::ElementOwnership::Owned>(vector);
}

/// Deletes the name @p name, as discard() a vector of value types.
void discard(wasm_name_t* name) noexcept
{
    if (name != nullptr && detail::isWellFormed(*name))
        detail::deleteVector<detail::ElementOwnership::Plain>(name);
}

/// Deletes the object @p object, what a refused wasm_ function was given to take; null is nothing to delete.
template <typename T>
    requires std::derived_from<T, detail::Object>
void discard(T* object) noexcept
{
    delete object;
}

/// What a wasm_ function that refuses returns, having deleted what it was given to take (isoline.h).
/// @param given The arguments it takes.
/// @return Null: no type.
template <typename... Given>
[[nodiscard]] auto refuse(Given*... given) noexcept -> std::nullptr_t
{
    (discard(given), ...);
    return nullptr;
}

/// What a type function gives for an out-of-memory host: no type.
constexpr auto NoType = [] {
    return nullptr;
};

} // namespace

// --- value types -------------------------------------------------------------------

void wasm_valtype_delete(wasm_valtype_t* type)
{
    detail::boundary([&] { delete type; }, [] {});
}

wasm_valtype_t* wasm_valtype_copy(wasm_valtype_t const* type)
{
    return detail::boundary([&] { return copyOf(type); }, NoType);
}

wasm_valtype_t* wasm_valtype_new(wasm_valkind_t kind)
{
    return detail::boundary(
        [&]() -> wasm_valtype_t* {
            auto const type = detail::fromCKind(kind);
            return type.has_value() ? std::make_unique<wasm_valtype_t>(*type).release() : nullptr;
        },
        NoType);
}

wasm_valkind_t wasm_valtype_kind(wasm_valtype_t const* type)
{
    return detail::boundary(
        [&] {
            return type == nullptr ? wasm_valkind_t { ISOLINE_VALKIND_INVALID }
                                   : detail::toCKind(type->type());
        },
        [] { return wasm_valkind_t { ISOLINE_VALKIND_INVALID }; });
}

// --- function types ----------------------------------------------------------------

void wasm_functype_delete(wasm_functype_t* type)
{
    detail::boundary([&] { delete type; }, [] {});
}

wasm_functype_t* wasm_functype_copy(wasm_functype_t const* type)
{
    return detail::boundary([&] { return copyOf(type); }, NoType);
}

wasm_functype_t* wasm_functype_new(wasm_valtype_vec_t* params, wasm_valtype_vec_t* results)
{
    return detail::boundary(
        [&]() -> wasm_functype_t* {
            if (params == nullptr || results == nullptr || !isValueTypeList(*params)
                || !isValueTypeList(*results))
                return refuse(params, results);
            return std::make_unique<wasm_functype_t>(*params, *results).release();
        },
        [&] { return refuse(params, results); });
}

wasm_valtype_vec_t const* wasm_functype_params(wasm_functype_t const* type)
{
    return detail::boundary([&] { return type == nullptr ? nullptr : &type->params(); }, NoType);
}

wasm_valtype_vec_t const* wasm_functype_results(wasm_functype_t const* type)
{
    return detail::boundary([&] { return type == nullptr ? nullptr : &type->results(); }, NoType);
}

// --- global types ------------------------------------------------------------------

void wasm_globaltype_delete(wasm_globaltype_t* type)
{
    detail::boundary([&] { delete type; }, [] {});
}

wasm_globaltype_t* wasm_globaltype_copy(wasm_globaltype_t const* type)
{
    return detail::boundary([&] { return copyOf(type); }, NoType);
}

wasm_globaltype_t* wasm_globaltype_new(wasm_valtype_t* content, wasm_mutability_t mutability)
{
    return detail::boundary(
        [&]() -> wasm_globaltype_t* {
            auto const known = detail::fromCMutability(mutability);
            if (content == nullptr || !known.has_value())
                return refuse(content);
            return std::make_unique<wasm_globaltype_t>(content, *known).release();
        },
        [&] { return refuse(content); });
}

wasm_valtype_t const* wasm_globaltype_content(wasm_globaltype_t const* type)
{
    return detail::boundary([&] { return type == nullptr ? nullptr : &type->content(); }, NoType);
}

wasm_mutability_t wasm_globaltype_mutability(wasm_globaltype_t const* type)
{
    return detail::boundary(
        [&] {
            return type == nullptr ? ISOLINE_MUTABILITY_INVALID : detail::toCMutability(type->mutability());
        },
        [] { return ISOLINE_MUTABILITY_INVALID; });
}

// --- table types -------------------------------------------------------------------

void wasm_tabletype_delete(wasm_tabletype_t* type)
{
    detail::boundary([&] { delete type; }, [] {});
}

wasm_tabletype_t* wasm_tabletype_copy(wasm_tabletype_t const* type)
{
    return detail::boundary([&] { return copyOf(type); }, NoType);
}

wasm_tabletype_t* wasm_tabletype_new(wasm_valtype_t* element, wasm_limits_t const* limits)
{
    return detail::boundary(
        [&]() -> wasm_tabletype_t* {
            if (element == nullptr || limits == nullptr)
                return refuse(element);
            auto const made = newTableType(element, detail::fromCLimits(*limits));
            return made.has_value() ? *made : refuse(element);
        },
        [&] { return refuse(element); });
}

wasm_valtype_t const* wasm_tabletype_element(wasm_tabletype_t const* type)
{
    return detail::boundary([&] { return type == nullptr ? nullptr : &type->element(); }, NoType);
}

wasm_limits_t const* wasm_tabletype_limits(wasm_tabletype_t const* type)
{
    return detail::boundary([&] { return type == nullptr ? nullptr : &type->clampedLimits(); }, NoType);
}

// --- memory types ------------------------------------------------------------------

void wasm_memorytype_delete(wasm_memorytype_t* type)
{
    detail::boundary([&] { delete type; }, [] {});
}

wasm_memorytype_t* wasm_memorytype_copy(wasm_memorytype_t const* type)
{
    return detail::boundary([&] { return copyOf(type); }, NoType);
}

wasm_memorytype_t* wasm_memorytype_new(wasm_limits_t const* limits)
{
    return detail::boundary(
        [&]() -> wasm_memorytype_t* {
            if (limits == nullptr)
                return nullptr;
            return newMemoryType(binary::MemoryType { .limits = detail::fromCLimits(*limits) })
                .value_or(nullptr);
        },
        NoType);
}

wasm_limits_t const* wasm_memorytype_limits(wasm_memorytype_t const* type)
{
    return detail::boundary([&] { return type == nullptr ? nullptr : &type->clampedLimits(); }, NoType);
}

// --- tag types ---------------------------------------------------------------------

void wasm_tagtype_delete(wasm_tagtype_t* type)
{
    detail::boundary([&] { delete type; }, [] {});
}

wasm_tagtype_t* wasm_tagtype_copy(wasm_tagtype_t const* type)
{
    return detail::boundary([&] { return copyOf(type); }, NoType);
}

wasm_tagtype_t* wasm_tagtype_new(wasm_functype_t* functype)
{
    return detail::boundary(
        [&]() -> wasm_tagtype_t* {
            // A tag's function type gives its exceptions' payload, and returns nothing.
            if (functype == nullptr || functype->results().size != 0)
                return refuse(functype);
            return std::make_unique<wasm_tagtype_t>(functype).release();
        },
        [&] { return refuse(functype); });
}

wasm_functype_t const* wasm_tagtype_functype(wasm_tagtype_t const* type)
{
    return detail::boundary([&] { return type == nullptr ? nullptr : &type->functype(); }, NoType);
}

// --- extern types ------------------------------------------------------------------

void wasm_externtype_delete(wasm_externtype_t* type)
{
    detail::boundary([&] { delete type; }, [] {});
}

wasm_externtype_t* wasm_externtype_copy(wasm_externtype_t const* type)
{
    return detail::boundary([&] { return copyOf(type); }, NoType);
}

wasm_externkind_t wasm_externtype_kind(wasm_externtype_t const* type)
{
    return detail::boundary(
        [&] { return type == nullptr ? ISOLINE_EXTERNKIND_INVALID : detail::toCExternKind(type->kind()); },
        [] { return ISOLINE_EXTERNKIND_INVALID; });
}

wasm_externtype_t* wasm_functype_as_externtype(wasm_functype_t* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_externtype_t>(type); }, NoType);
}

wasm_externtype_t* wasm_globaltype_as_externtype(wasm_globaltype_t* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_externtype_t>(type); }, NoType);
}

wasm_externtype_t* wasm_tabletype_as_externtype(wasm_tabletype_t* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_externtype_t>(type); }, NoType);
}

wasm_externtype_t* wasm_memorytype_as_externtype(wasm_memorytype_t* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_externtype_t>(type); }, NoType);
}

wasm_externtype_t* wasm_tagtype_as_externtype(wasm_tagtype_t* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_externtype_t>(type); }, NoType);
}

wasm_functype_t* wasm_externtype_as_functype(wasm_externtype_t* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_functype_t>(type); }, NoType);
}

wasm_globaltype_t* wasm_externtype_as_globaltype(wasm_externtype_t* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_globaltype_t>(type); }, NoType);
}

wasm_tabletype_t* wasm_externtype_as_tabletype(wasm_externtype_t* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_tabletype_t>(type); }, NoType);
}

wasm_memorytype_t* wasm_externtype_as_memorytype(wasm_externtype_t* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_memorytype_t>(type); }, NoType);
}

wasm_tagtype_t* wasm_externtype_as_tagtype(wasm_externtype_t* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_tagtype_t>(type); }, NoType);
}

wasm_externtype_t const* wasm_functype_as_externtype_const(wasm_functype_t const* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_externtype_t const>(type); }, NoType);
}

wasm_externtype_t const* wasm_globaltype_as_externtype_const(wasm_globaltype_t const* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_externtype_t const>(type); }, NoType);
}

wasm_externtype_t const* wasm_tabletype_as_externtype_const(wasm_tabletype_t const* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_externtype_t const>(type); }, NoType);
}

wasm_externtype_t const* wasm_memorytype_as_externtype_const(wasm_memorytype_t const* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_externtype_t const>(type); }, NoType);
}

wasm_externtype_t const* wasm_tagtype_as_externtype_const(wasm_tagtype_t const* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_externtype_t const>(type); }, NoType);
}

wasm_functype_t const* wasm_externtype_as_functype_const(wasm_externtype_t const* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_functype_t const>(type); }, NoType);
}

wasm_globaltype_t const* wasm_externtype_as_globaltype_const(wasm_externtype_t const* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_globaltype_t const>(type); }, NoType);
}

wasm_tabletype_t const* wasm_externtype_as_tabletype_const(wasm_externtype_t const* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_tabletype_t const>(type); }, NoType);
}

wasm_memorytype_t const* wasm_externtype_as_memorytype_const(wasm_externtype_t const* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_memorytype_t const>(type); }, NoType);
}

wasm_tagtype_t const* wasm_externtype_as_tagtype_const(wasm_externtype_t const* type)
{
    return detail::boundary([&] { return externTypeAs<wasm_tagtype_t const>(type); }, NoType);
}

// --- import and export types -------------------------------------------------------

void wasm_importtype_delete(wasm_importtype_t* type)
{
    detail::boundary([&] { delete type; }, [] {});
}

wasm_importtype_t* wasm_importtype_copy(wasm_importtype_t const* type)
{
    return detail::boundary([&] { return copyOf(type); }, NoType);
}

wasm_importtype_t* wasm_importtype_new(wasm_name_t* module, wasm_name_t* name, wasm_externtype_t* type)
{
    return detail::boundary(
        [&]() -> wasm_importtype_t* {
            if (module == nullptr || name == nullptr || type == nullptr || !detail::isWellFormed(*module)
                || !detail::isWellFormed(*name))
                return refuse(module, name, type);
            return std::make_unique<wasm_importtype_t>(*module, *name, type).release();
        },
        [&] { return refuse(module, name, type); });
}

wasm_name_t const* wasm_importtype_module(wasm_importtype_t const* type)
{
    return detail::boundary([&] { return type == nullptr ? nullptr : &type->module(); }, NoType);
}

wasm_name_t const* wasm_importtype_name(wasm_importtype_t const* type)
{
    return detail::boundary([&] { return type == nullptr ? nullptr : &type->name(); }, NoType);
}

wasm_externtype_t const* wasm_importtype_type(wasm_importtype_t const* type)
{
    return detail::boundary([&] { return type == nullptr ? nullptr : &type->type(); }, NoType);
}

void wasm_exporttype_delete(wasm_exporttype_t* type)
{
    detail::boundary([&] { delete type; }, [] {});
}

wasm_exporttype_t* wasm_exporttype_copy(wasm_exporttype_t const* type)
{
    return detail::boundary([&] { return copyOf(type); }, NoType);
}

wasm_exporttype_t* wasm_exporttype_new(wasm_name_t* name, wasm_externtype_t* type)
{
    return detail::boundary(
        [&]() -> wasm_exporttype_t* {
            if (name == nullptr || type == nullptr || !detail::isWellFormed(*name))
                return refuse(name, type);
            return std::make_unique<wasm_exporttype_t>(*name, type).release();
        },
        [&] { return refuse(name, type); });
}

wasm_name_t const* wasm_exporttype_name(wasm_exporttype_t const* type)
{
    return detail::boundary([&] { return type == nullptr ? nullptr : &type->name(); }, NoType);
}

wasm_externtype_t const* wasm_exporttype_type(wasm_exporttype_t const* type)
{
    return detail::boundary([&] { return type == nullptr ? nullptr : &type->type(); }, NoType);
}

// --- val.h -------------------------------------------------------------------------

isoline_nullability_t ISOLINE_CALL isoline_valtype_nullability(wasm_valtype_t const* type) noexcept
{
    return type == nullptr ? isoline_nullability_t { ISOLINE_NOT_NULLABLE }
                           : detail::toCNullability(type->type());
}

isoline_heap_t ISOLINE_CALL isoline_valtype_heap(wasm_valtype_t const* type) noexcept
{
    return type == nullptr ? isoline_heap_t { ISOLINE_HEAP_NOT_REFERENCE } : detail::toCHeap(type->type());
}

uint32_t ISOLINE_CALL isoline_valtype_concrete_index(wasm_valtype_t const* type) noexcept
{
    if (type == nullptr || !binary::isConcreteRef(type->type()))
        return UINT32_MAX;
    return type->type().heap.typeIndex;
}

isoline_error_t* ISOLINE_CALL isoline_memorytype_new(isoline_limits64_t const* limits,
                                                     uint32_t addressBits,
                                                     uint32_t pageSizeLog2,
                                                     wasm_memorytype_t** outType) noexcept
{
    return detail::boundary(
        [&] {
            if (outType == nullptr)
                return detail::typeRefusalError(TypeRefusal::NullArgument);
            auto const addressType = detail::fromCAddressBits(addressBits);
            if (!addressType.has_value())
                return detail::typeRefusalError(TypeRefusal::AddressBits);
            // A page size whose logarithm no uint8_t holds is none custom-page-sizes admits either.
            if (pageSizeLog2 > UINT8_MAX)
                return detail::typeRefusalError(TypeRefusal::PageSize);
            auto const made =
                detail::fromCLimits64(limits, *addressType).and_then([&](binary::Limits const& checked) {
                    return newMemoryType(binary::MemoryType {
                        .limits = checked, .pageSizeLog2 = static_cast<uint8_t>(pageSizeLog2) });
                });
            return reportInto(made, outType);
        },
        [] { return detail::hostMemoryExhaustedError(); });
}

isoline_error_t* ISOLINE_CALL isoline_memorytype_limits64(wasm_memorytype_t const* type,
                                                          isoline_limits64_t* outLimits) noexcept
{
    if (type == nullptr)
        return detail::typeRefusalError(TypeRefusal::NullArgument);
    auto const written = detail::toCLimits64(type->type().limits, outLimits);
    return written.has_value() ? nullptr : detail::typeRefusalError(written.error());
}

uint32_t ISOLINE_CALL isoline_memorytype_page_size_log2(wasm_memorytype_t const* type) noexcept
{
    return type == nullptr ? UINT32_MAX : uint32_t { type->type().pageSizeLog2 };
}

uint32_t ISOLINE_CALL isoline_memorytype_address_bits(wasm_memorytype_t const* type) noexcept
{
    return type == nullptr ? 0 : detail::toCAddressBits(type->type().limits.addressType);
}

isoline_error_t* ISOLINE_CALL isoline_tabletype_new(wasm_valtype_t* element,
                                                    isoline_limits64_t const* limits,
                                                    uint32_t addressBits,
                                                    wasm_tabletype_t** outType) noexcept
{
    return detail::boundary(
        [&] {
            if (element == nullptr || outType == nullptr)
                return detail::typeRefusalError(TypeRefusal::NullArgument);
            auto const addressType = detail::fromCAddressBits(addressBits);
            if (!addressType.has_value())
                return detail::typeRefusalError(TypeRefusal::AddressBits);
            auto const made =
                detail::fromCLimits64(limits, *addressType).and_then([&](binary::Limits const& checked) {
                    return newTableType(element, checked);
                });
            return reportInto(made, outType);
        },
        [] { return detail::hostMemoryExhaustedError(); });
}

isoline_error_t* ISOLINE_CALL isoline_tabletype_limits64(wasm_tabletype_t const* type,
                                                         isoline_limits64_t* outLimits) noexcept
{
    if (type == nullptr)
        return detail::typeRefusalError(TypeRefusal::NullArgument);
    auto const written = detail::toCLimits64(type->limits(), outLimits);
    return written.has_value() ? nullptr : detail::typeRefusalError(written.error());
}

uint32_t ISOLINE_CALL isoline_tabletype_address_bits(wasm_tabletype_t const* type) noexcept
{
    return type == nullptr ? 0 : detail::toCAddressBits(type->limits().addressType);
}
