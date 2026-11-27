#pragma once

#include <isoline/runtime/Limits.hpp>

namespace isoline::api
{
struct Store
{
    int pages;
};

template <typename Value>
struct Box
{
    Value value;
};

using Constant = int const;

// Internal linkage: a non-inline variable of const-qualified type, its constness spelled every way.
constexpr int probe = 1;
int const limit = 2;
int* const cursor = nullptr;
Constant aliased = 3; // const only once desugared
int const sizes[2] = { 1, 2 };
Store const emptyStore {};
int (*const callback)(int*) = nullptr;
constexpr int const* constexprPointer = nullptr;
Constant table[3] = {}; // an array of an alias of a const type: printed `Constant[3]`
int* const cursors[2] = {};
auto const onLoad = [](int& value, int* other) noexcept {
    return value + *other;
};

// A redeclaration has the linkage of the first declaration.
static int pageCount(Store const& store);
int pageCount(Store const& store);

// External linkage: inline, extern before, volatile, const only below the top (behind a pointer, in a
// reference, a parameter or a template argument), a template.
inline constexpr int storeLimit = 16;
extern int const declaredLimit;
int const declaredLimit = 4;
int const volatile sharedLimit = 5;
int const* readOnly = nullptr;
int const& limitReference = storeLimit;
auto onStore = [](int const& value, int const* other) noexcept {
    return value + *other;
};
int (*handler)(int const) = nullptr;
Box<int const> boxed {};
template <typename Value>
constexpr Value zero = Value {};
} // namespace isoline::api
