#pragma once

#include <isoline/Contracts.hpp>

namespace isoline::api
{
struct Store
{
    [[nodiscard]] int pages() const noexcept ISOLINE_PRE(true);
};
} // namespace isoline::api
