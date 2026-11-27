#pragma once

namespace isoline::api
{
template <typename Element, int Size, template <typename> class Holder>
struct Array
{
    Holder<Element> elements[Size];
};

template <typename Value>
Value first(Value value)
{
    return value;
}
} // namespace isoline::api
