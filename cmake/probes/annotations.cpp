// SPDX-License-Identifier: Apache-2.0
//
// The annotations probe (cmake/IsolineLanguageFeatures.cmake): an enumerator annotated with a structural
// text value, read back through reflection. A std::string_view member is not structural, so the text is
// a character array, as Isoline's FixedText holds it.

#include <cstddef>
#include <meta>
#include <ranges>
#include <string_view>

namespace probe
{
template <std::size_t N>
struct FixedText
{
    char data[N] {};

    consteval FixedText(char const (&text)[N])
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, N))
            data[index] = text[index];
    }

    [[nodiscard]] constexpr auto view() const noexcept -> std::string_view { return { data, N - 1 }; }
};

enum class Colour : unsigned char
{
    Red[[= FixedText { "x" }]],
};

consteval auto annotationText() -> std::string_view
{
    static constexpr auto annotations = std::define_static_array(std::meta::annotations_of(^^Colour::Red));
    return [:std::meta::constant_of(annotations[0]):].view();
}
} // namespace probe

static_assert(probe::annotationText() == "x");

auto main() -> int
{
    return 0;
}
