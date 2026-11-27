#pragma once

namespace isoline::api
{
struct Store
{
    int pages;

    friend bool operator==(Store const&, Store const&) = default;
    friend int compare(Store const& a, Store const& b) { return a.pages - b.pages; }
    friend void touch(Store& store);
};
} // namespace isoline::api
