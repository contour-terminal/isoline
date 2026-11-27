/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The installed C API, used from C through the package's component c (isoline::c, the shared library), by a
 * project of C alone: find_package(isoline) asks it for no C++ compiler, whatever the package's contracts
 * took. Exits 0 if and only if the library is of the ABI the headers describe and makes and deletes a value
 * type; otherwise it names the check that failed.
 */

#include <stdio.h>

#include <isoline/c/isoline.h>

int main(void)
{
    wasm_valtype_t* type = NULL;
    if (!isoline_abi_compatible(ISOLINE_ABI_VERSION))
    {
        fputs("c-only-consumer: check failed: isoline_abi_compatible(ISOLINE_ABI_VERSION)\n", stderr);
        return 1;
    }
    type = wasm_valtype_new(WASM_I64);
    if (type == NULL || wasm_valtype_kind(type) != WASM_I64)
    {
        fputs("c-only-consumer: check failed: wasm_valtype_kind(wasm_valtype_new(WASM_I64)) == WASM_I64\n",
              stderr);
        return 1;
    }
    wasm_valtype_delete(type);
    return 0;
}
