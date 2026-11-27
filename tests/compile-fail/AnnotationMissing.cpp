// SPDX-License-Identifier: Apache-2.0
//
// Must not compile (tests/CMakeLists.txt, compile-fail-annotation-missing; where annotations are native):
// a table derived from an enum one of whose enumerators has no FixedText annotation. The build names it.

#include <cstdint>

#include <isoline/meta/FixedText.hpp>
#include <isoline/meta/reflection/AnnotatedRows.hpp>

namespace
{

/// An enum whose second enumerator lacks its text.
enum class Unannotated : uint8_t
{
    Annotated[[= isoline::meta::FixedText { "annotated" }]],
    Bare,
};

} // namespace

/// Derives the texts, which refuses Unannotated::Bare.
auto fixtureTextCount() -> std::size_t
{
    return isoline::meta::reflection::annotatedTexts<Unannotated>.size();
}
