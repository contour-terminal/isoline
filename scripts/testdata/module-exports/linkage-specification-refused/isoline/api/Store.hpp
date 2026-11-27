#pragma once

#include <isoline/runtime/Clock.hpp>

namespace isoline::api
{
extern "C"
{
    struct Store
    {
        int pages;
    };

    int pageCount(Store const* store);
    static int pageLimit = 0;

    extern "C++"
    {
        int const storeLimit = 2;
    }
}

// An unbraced linkage specification declares as if `extern` were written: external linkage.
extern "C" int const defaultPages = 1;
} // namespace isoline::api
