// SPDX-License-Identifier: Apache-2.0
//
// Must not compile (tests/CMakeLists.txt, compile-fail-c-mirror-constant-without-row; where reflection is
// native): a constant added to a mirrored C enumeration without a row. The build names it.

#include <array>
#include <cstdint>
#include <meta>

#include <isoline/meta/reflection/CApiReflection.hpp>

namespace
{

/// A C enumeration, as wasm.h spells one, whose last constant was added without a row.
enum fixture_kind_enum
{
    FIXTURE_KIND_A,
    FIXTURE_KIND_B,
    FIXTURE_KIND_ADDED,
};

/// One row of the table that converts it.
struct KindRow
{
    uint8_t cKind; ///< The C constant.
};

constexpr auto KindRows =
    std::to_array<KindRow>({ { .cKind = FIXTURE_KIND_A }, { .cKind = FIXTURE_KIND_B } });

constexpr auto NoneWithout = std::array<std::meta::info, 0> {};

ISOLINE_C_MIRROR(KindRows, &KindRow::cKind, NoneWithout, NoneWithout, fixture_kind_enum);

} // namespace
