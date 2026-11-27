// SPDX-License-Identifier: Apache-2.0
//
// The proof that every declaration contract sits on a noexcept member function, where contracts take the
// native path: the umbrella header CMake generates includes every header of the libraries' header sets
// and their private headers (isoline_add_reflection_check), and this target reads them through the
// proving expansion of <isoline/Contracts.hpp> (ContractsProof.hpp.in), whose static_assert names each
// ISOLINE_PRE or ISOLINE_POST on a function that may throw, or on a free function. Compile-only: the
// translation unit defines nothing, and nothing links it, so its expansion never meets the installed one.

#include <isoline/meta/reflection/DeclarationContractsUmbrella.hpp>

static_assert(ISOLINE_DECLARATION_CONTRACTS == 1, "the proof reads the declarations with contracts native");
