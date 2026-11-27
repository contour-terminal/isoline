#pragma once

namespace isoline::api
{
struct Store
{
    int pages();
    static int instances;

    template <typename T>
    T as();
};

inline int Store::pages()
{
    return 0;
}

inline int Store::instances = 0;

template <typename T>
T Store::as()
{
    return T {};
}

namespace detail
{
    int defaultPages();
}

inline int detail::defaultPages()
{
    return 1;
}
} // namespace isoline::api
