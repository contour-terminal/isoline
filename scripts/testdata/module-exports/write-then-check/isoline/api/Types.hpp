#pragma once

#include <isoline/runtime/Value.hpp>

namespace isoline::api
{
using runtime::Value;

enum class Row
{
#define ROW(name) name,
#include <isoline/api/Rows.def>
#undef ROW
};
} // namespace isoline::api
