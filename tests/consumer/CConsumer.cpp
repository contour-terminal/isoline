// SPDX-License-Identifier: Apache-2.0
//
// The installed C API, used through the package's component c (isoline::c, the shared library): its
// headers compile as C++ too, so the consumer project, a C++ one, builds it as it is. Exits 0 if and only
// if the library is of the ABI the headers describe and makes and deletes a value type; otherwise it names
// the check that failed.

#include <cstdio>

#include <isoline/c/isoline.h>

int main()
{
    if (!isoline_abi_compatible(ISOLINE_ABI_VERSION))
    {
        std::fputs("c-consumer: check failed: isoline_abi_compatible(ISOLINE_ABI_VERSION)\n", stderr);
        return 1;
    }
    auto* const type = wasm_valtype_new(WASM_I64);
    if (type == nullptr || wasm_valtype_kind(type) != WASM_I64)
    {
        std::fputs("c-consumer: check failed: wasm_valtype_kind(wasm_valtype_new(WASM_I64)) == WASM_I64\n",
                   stderr);
        return 1;
    }
    wasm_valtype_delete(type);
    return 0;
}
