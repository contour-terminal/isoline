#include <core/Assert.hpp>

namespace isoline::api
{
void start(int const* pointer)
{
    Require(pointer != nullptr);
}
// stop(int) is gone: a comment that names it is no function.
} // namespace isoline::api
