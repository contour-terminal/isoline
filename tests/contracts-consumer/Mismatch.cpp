// SPDX-License-Identifier: Apache-2.0
//
// Must not link (tests/CMakeLists.txt, contracts-mode-mismatch; where contracts are native): a translation
// unit compiled against the fallback copy of <isoline/Contracts.hpp>, as a fallback package's consumer is,
// linked with a native isoline_core. Each mode's header names its own symbol
// (isoline::meta::detail::declaration_contracts_<mode>::declarationContractsMode()), and only the library's
// mode defines it.

#include <isoline/api/Engine.hpp>

// It reads nothing of the header itself: the header's own inline variable is what references the symbol.
int main()
{
    return 0;
}
