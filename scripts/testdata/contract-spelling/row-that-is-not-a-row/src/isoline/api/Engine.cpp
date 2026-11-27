#include <core/Assert.hpp>

namespace isoline::api
{
void start(int const* pointer)
{
    Require(pointer != nullptr);
}
} // namespace isoline::api
