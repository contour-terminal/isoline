// SPDX-License-Identifier: Apache-2.0
//
// Must not compile (tests/CMakeLists.txt, compile-fail-c-layout-member-without-row; where reflection is
// native): a member added to a C struct without its ISOLINE_LAYOUT_FIELD row. The build names it.

#include <array>
#include <cstdint>
#include <meta>

#include <isoline/meta/reflection/CApiReflection.hpp>

namespace
{

/// A C struct whose last member was added without a field row.
struct fixture_limits_t
{
    uint64_t minimum; ///< A bound.
    uint64_t added;   ///< The member without a row.
};

constexpr auto Structs = std::to_array<std::meta::info>({ ^^fixture_limits_t });

constexpr auto Fields = std::to_array<isoline::meta::reflection::LayoutFieldRow>({
    { .type = ^^fixture_limits_t, .field = "minimum" },
});

constexpr auto Types = std::to_array<isoline::meta::reflection::LayoutTypeRow>({
    { .type = ^^fixture_limits_t, .path = "minimum", .fieldType = ^^uint64_t },
    { .type = ^^fixture_limits_t, .path = "added", .fieldType = ^^uint64_t },
});

static_assert(isoline::meta::reflection::layoutProblem(Structs, Fields, Types).empty(),
              isoline::meta::reflection::layoutProblem(Structs, Fields, Types));

} // namespace
