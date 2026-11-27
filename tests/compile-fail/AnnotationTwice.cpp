// SPDX-License-Identifier: Apache-2.0
//
// Must not compile (tests/CMakeLists.txt, compile-fail-annotation-twice; where annotations are native): a
// table derived from an enum one of whose enumerators has two FixedText annotations. The build names it.

#include <cstdint>

#include <isoline/meta/FixedText.hpp>
#include <isoline/meta/reflection/AnnotatedRows.hpp>

namespace
{

/// An enum whose second enumerator has two texts.
enum class Doubled : uint8_t
{
    Single[[= isoline::meta::FixedText { "single" }]],
    Twice[[= isoline::meta::FixedText { "first" }]][[= isoline::meta::FixedText { "second" }]],
};

} // namespace

/// Derives the texts, which refuses Doubled::Twice.
auto fixtureTextCount() -> std::size_t
{
    return isoline::meta::reflection::annotatedTexts<Doubled>.size();
}
