#pragma once

namespace isoline::api
{
#define ROW(name) inline constexpr int name = 0;
#include <isoline/api/Rows.def>
#undef ROW
} // namespace isoline::api
