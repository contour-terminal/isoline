#include <isoline/api/Engine.hpp>
#include <isoline/meta/Check.hpp>

#include <vector>

namespace isoline::api
{
void start(int const* pointer) noexcept
{
    ISOLINE_PRECONDITION(pointer != nullptr);
    ISOLINE_INVARIANT(*pointer > 0);
}

void Engine::load(int size)
{
    Require(size > 0); // a Require( in a comment is not counted
    auto const text = "Require(";
    _size = static_cast<int>(std::vector<char>(static_cast<std::size_t>(size), *text).size());
}
} // namespace isoline::api
