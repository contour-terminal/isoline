#include <core/Assert.hpp>

#include <vector>

namespace isoline::api
{
std::vector<int> start(int const* pointer)
{
    Require(pointer != nullptr);
    Guarantee(*pointer > 0);
    return { *pointer };
}
} // namespace isoline::api
