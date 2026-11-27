#pragma once

#include <isoline/runtime/Counter.hpp>

namespace isoline::api
{
struct Store
{
    int pages;
};

static int pageCount(Store const& store);

static int pageCount(Store const& store)
{
    return store.pages;
}

template <typename Value>
static Value twice(Value value)
{
    return value + value;
}
} // namespace isoline::api
