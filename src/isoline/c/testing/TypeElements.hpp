// SPDX-License-Identifier: Apache-2.0
//
// Real elements of the vector families whose element type exists (the types), made through the C API as a
// caller makes them: what the C API's C++ tests of vector ownership fill their vectors with.
#pragma once

#include <isoline/c/isoline.h>

namespace isoline::c::testing
{

/// A real element of type @p Element, made through the C API as a caller makes one.
template <typename Element>
[[nodiscard]] auto makeElement() -> Element;

template <>
inline auto makeElement<wasm_valtype_t*>() -> wasm_valtype_t*
{
    return wasm_valtype_new(ISOLINE_VALKIND_EXNREF);
}

template <>
inline auto makeElement<wasm_functype_t*>() -> wasm_functype_t*
{
    return wasm_functype_new_1_1(wasm_valtype_new(WASM_I32), wasm_valtype_new(ISOLINE_VALKIND_V128));
}

template <>
inline auto makeElement<wasm_globaltype_t*>() -> wasm_globaltype_t*
{
    return wasm_globaltype_new(wasm_valtype_new(WASM_F64), WASM_VAR);
}

template <>
inline auto makeElement<wasm_tabletype_t*>() -> wasm_tabletype_t*
{
    auto const limits = wasm_limits_t { .min = 1, .max = 8 };
    return wasm_tabletype_new(wasm_valtype_new(WASM_FUNCREF), &limits);
}

template <>
inline auto makeElement<wasm_memorytype_t*>() -> wasm_memorytype_t*
{
    auto const limits = wasm_limits_t { .min = 1, .max = wasm_limits_max_default };
    return wasm_memorytype_new(&limits);
}

template <>
inline auto makeElement<wasm_tagtype_t*>() -> wasm_tagtype_t*
{
    return wasm_tagtype_new(wasm_functype_new_1_0(wasm_valtype_new(WASM_I64)));
}

template <>
inline auto makeElement<wasm_externtype_t*>() -> wasm_externtype_t*
{
    return wasm_globaltype_as_externtype(makeElement<wasm_globaltype_t*>());
}

template <>
inline auto makeElement<wasm_importtype_t*>() -> wasm_importtype_t*
{
    auto module = wasm_name_t {};
    auto name = wasm_name_t {};
    wasm_name_new_from_string(&module, "env");
    wasm_name_new_from_string(&name, "table");
    return wasm_importtype_new(
        &module, &name, wasm_tabletype_as_externtype(makeElement<wasm_tabletype_t*>()));
}

template <>
inline auto makeElement<wasm_exporttype_t*>() -> wasm_exporttype_t*
{
    auto name = wasm_name_t {};
    wasm_name_new_from_string(&name, "memory");
    return wasm_exporttype_new(&name, wasm_memorytype_as_externtype(makeElement<wasm_memorytype_t*>()));
}

} // namespace isoline::c::testing
