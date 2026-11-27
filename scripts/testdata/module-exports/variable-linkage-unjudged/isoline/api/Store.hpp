#pragma once

#include <isoline/runtime/Version.hpp>

namespace isoline::api
{
struct Store
{
    int pages;
};

// The variable of an anonymous union, which Clang declares implicitly.
static union {
    int pageSize;
    float pageScale;
};
} // namespace isoline::api
