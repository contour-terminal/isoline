// SPDX-License-Identifier: Apache-2.0
//
// The reflection probe (cmake/IsolineLanguageFeatures.cmake): what Isoline's reflection checks use, in
// the forms they use it. Enumerators into a static constexpr array iterated by `template for`, their
// identifiers, a namespace's members, a member's offset, and whether the enclosing function is noexcept.
// It compiles an expression of each: no feature macro or header presence decides (<meta> compiles
// without -freflection).

#include <cstddef>
#include <meta>

namespace probe
{
enum class Colour : unsigned char
{
    Red,
    Green,
};

struct Pair
{
    int first;
    int second;
};

int const answer = 42;

consteval auto countEnumerators() -> std::size_t
{
    static constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^Colour));
    auto count = std::size_t { 0 };
    template for (constexpr auto enumerator: enumerators)
    {
        if (std::meta::identifier_of(enumerator).size() > 0)
            ++count;
    }
    return count;
}

consteval auto countNamespaceMembers() -> std::size_t
{
    return std::meta::members_of(^^probe, std::meta::access_context::unchecked()).size();
}

auto checked() noexcept -> int
{
    static_assert(std::meta::is_noexcept(std::meta::access_context::current().scope()));
    return 0;
}
} // namespace probe

static_assert(probe::countEnumerators() == 2);
static_assert(std::meta::identifier_of(^^probe::Colour::Green) == "Green");
static_assert(probe::countNamespaceMembers() > 0);
static_assert(std::meta::offset_of(std::meta::nonstatic_data_members_of(
                                       ^^probe::Pair, std::meta::access_context::unchecked())[1])
                  .bytes
              == sizeof(int));

auto main() -> int
{
    return probe::checked();
}
