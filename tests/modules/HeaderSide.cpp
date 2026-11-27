// SPDX-License-Identifier: Apache-2.0

#include "HeaderSide.hpp"

#include <isoline/Features.hpp>
#include <isoline/api/Imports.hpp>

namespace isoline::modules_test
{

std::expected<isoline::api::Instance, isoline::api::InstantiateError> instantiateAdd(
    isoline::api::Store& store, isoline::api::Module const& module)
{
    return store.instantiate(module, isoline::api::Imports {});
}

void const* featureTableAddressFromHeaders() noexcept
{
    return &isoline::FeatureTable;
}

} // namespace isoline::modules_test
