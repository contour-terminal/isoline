#pragma once

namespace isoline::api
{
struct Store
{
    int pages;
};

inline constexpr int storeLimit = 16;

int open(Store const& store);
} // namespace isoline::api
