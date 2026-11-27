#pragma once

namespace isoline::api
{
struct Store;

int open(int pages);
int open(Store const& store);
int open(Store const& store, int pages);

struct Store
{
    int pages;
};
} // namespace isoline::api
