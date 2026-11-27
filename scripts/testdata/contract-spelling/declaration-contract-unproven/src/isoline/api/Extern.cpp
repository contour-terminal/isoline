#include <isoline/api/Extern.hpp>

namespace
{
/// A class local to the implementation file, with declaration contracts.
struct Local
{
    int at(int index) const noexcept ISOLINE_PRE(index >= 0);
    int size() const noexcept ISOLINE_POST(result, result >= 0);
    // ISOLINE_PRE in a comment is no contract
};
} // namespace
