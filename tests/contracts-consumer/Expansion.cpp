// SPDX-License-Identifier: Apache-2.0
//
// Preprocessed, never compiled (tests/cmake/contracts-expansion.cmake): what ISOLINE_PRE and ISOLINE_POST
// expand to, once through the installed <isoline/Contracts.hpp> and once through the proving one, whose
// noexcept proof is emptied here. The two must read the same, so that the proof checks the declarations as
// they are installed. The lines after the marker are what the test compares.
#include <isoline/Contracts.hpp>

#if defined(ISOLINE_DETAIL_NOEXCEPT_PROOF)
    #undef ISOLINE_DETAIL_NOEXCEPT_PROOF
    #define ISOLINE_DETAIL_NOEXCEPT_PROOF
#endif

// clang-format off
isoline_expansion_begin
mode ISOLINE_DECLARATION_CONTRACTS
ISOLINE_PRE(index >= 0)
ISOLINE_POST(result, result >= index);
// clang-format on
