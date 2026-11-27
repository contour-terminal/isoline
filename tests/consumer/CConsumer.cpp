// SPDX-License-Identifier: Apache-2.0
//
// The installed C API, used through the package's component c (isoline::c, the shared library): its
// headers compile as C++ too, so the consumer project, a C++ one, builds it as it is. Exits 0 if and only
// if the library is of the ABI the headers describe; otherwise it names the check that failed.

#include <cstdio>

#include <isoline/c/isoline.h>

int main()
{
    if (!isoline_abi_compatible(ISOLINE_ABI_VERSION))
    {
        std::fputs("c-consumer: check failed: isoline_abi_compatible(ISOLINE_ABI_VERSION)\n", stderr);
        return 1;
    }
    return 0;
}
