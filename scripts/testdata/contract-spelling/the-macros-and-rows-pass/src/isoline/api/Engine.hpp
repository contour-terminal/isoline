#pragma once

#include <core/Assert.hpp>

#include <isoline/meta/EnumRows.hpp>

namespace isoline::api
{
/// An engine.
class Engine
{
public:
    /// Loads @p size bytes.
    void load(int size);

    /// Its size.
    [[nodiscard]] int size() const noexcept
    {
        Require(_size >= 0);
        return _size;
    }

private:
    int _size = 0;
};
} // namespace isoline::api
