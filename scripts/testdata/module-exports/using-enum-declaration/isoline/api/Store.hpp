#pragma once

#include <isoline/runtime/Kind.hpp>

namespace isoline::api
{
struct Store
{
    int pages;
};

// In a public namespace: enumerators no using-declaration of the interface unit names.
using enum isoline::runtime::Kind;
} // namespace isoline::api
