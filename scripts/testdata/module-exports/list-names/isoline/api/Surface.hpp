#pragma once

#include <isoline/runtime/Value.hpp>

namespace isoline::api
{
using runtime::Value;

struct Store
{
    friend bool operator==(Store const&, Store const&) = default;
};

template <typename T>
struct Box
{
};

template <>
struct Box<int>
{
};

template <typename T>
concept Boxed = requires { typename T::Boxed; };

using Pages = unsigned;

template <typename T>
using BoxOf = Box<T>;

enum Kind
{
    KindFunction,
};

enum
{
    AnonymousConstant = 1,
};

inline constexpr unsigned storeLimit = 1;

Store open(int handle);

template <typename T>
Store open(T handle);

bool operator<(Store const&, Store const&);
} // namespace isoline::api
