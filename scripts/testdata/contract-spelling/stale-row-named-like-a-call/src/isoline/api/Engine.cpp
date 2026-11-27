#include <core/Assert.hpp>

#include <vector>

namespace isoline::api
{
// at() is defined elsewhere: here it is only called, by its name alone, through `.` and through `->`,
// and followed by what follows calls: an operator, a `:` of a conditional, a `;`.
int start(std::vector<int> const& values, std::vector<int> const* pointer, bool first)
{
    Require(!values.empty());
    Require(pointer != nullptr);
    auto const masked = (at(values, 0) & 1) + (at(values, 1) && first ? 1 : 0);
    return (first ? at(values, 0) : values.at(0)) + pointer->at(0) + masked;
}

/// An item.
struct Item
{
    int value; ///< Its value.
};

int firstValue(std::vector<Item const*> const& items, std::vector<Item const*> const* pointer)
{
    return items.at(0)->value + pointer->at(0)->value + format(0)->value;
}
} // namespace isoline::api
