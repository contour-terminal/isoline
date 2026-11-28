// SPDX-License-Identifier: Apache-2.0
#include <isoline/fixture/Api.hpp>

int isoline::fixture::Api::at(int index) const noexcept
{
    return index;
}

int isoline::meta::detail::declarationContractsMode() noexcept
{
    return ISOLINE_DECLARATION_CONTRACTS;
}
