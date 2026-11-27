#pragma once

namespace isoline::api
{
namespace detail
{
    enum class Row
    {
#define ROW(name) name,
#include <isoline/api/Rows.def>
#undef ROW
    };
} // namespace detail

#define ROW(name) inline constexpr int name = 0;
#include <isoline/api/Rows.def>
#undef ROW

enum class Column
{
#define COLUMN(name) name,
#include <isoline/api/Columns.def>
#undef COLUMN
};

#define COLUMN(name) inline constexpr int name = 1;
#include <isoline/api/Columns.def>
#undef COLUMN
} // namespace isoline::api
