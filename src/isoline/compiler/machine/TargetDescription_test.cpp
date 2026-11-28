// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>

#include <isoline/compiler/machine/Target.hpp>

using isoline::compiler::machine::Architecture;
using isoline::compiler::machine::CpuFeature;
using isoline::compiler::machine::CpuFeatureCount;
using isoline::compiler::machine::CpuFeatures;
using isoline::compiler::machine::CpuFeatureTable;
using isoline::compiler::machine::extendsOnly;
using isoline::compiler::machine::OperatingSystemAbi;
using isoline::compiler::machine::TargetDescription;

TEST_CASE("CpuFeatures.setOperations")
{
    constexpr auto None = CpuFeatures {};
    constexpr auto Rounding = None.with(CpuFeature::Sse41);
    constexpr auto Bits = Rounding.with(CpuFeature::Popcnt).with(CpuFeature::Lzcnt);
    static_assert(Rounding.contains(CpuFeature::Sse41) && !None.contains(CpuFeature::Sse41));
    for (auto const index: std::views::iota(std::size_t { 0 }, CpuFeatureCount))
    {
        auto const feature = static_cast<CpuFeature>(index);
        INFO("feature " << index);
        CHECK_FALSE(None.contains(feature));
        CHECK(None.with(feature).contains(feature));
        CHECK(None.with(feature).containsAll(None));
        CHECK(None.with(feature) == None.with(feature).with(feature));
        for (auto const other: std::views::iota(std::size_t { 0 }, CpuFeatureCount))
            if (other != index)
                CHECK_FALSE(None.with(feature).contains(static_cast<CpuFeature>(other)));
    }
    CHECK(Bits.containsAll(Rounding));
    CHECK_FALSE(Rounding.containsAll(Bits));
    CHECK(Bits.containsAll(Bits));
    CHECK(None.containsAll(None));
    CHECK(Bits != Rounding);
}

TEST_CASE("TargetDescription.equality")
{
    auto const base = TargetDescription { .architecture = Architecture::X64,
                                          .abi = OperatingSystemAbi::SystemV,
                                          .features = CpuFeatures {}.with(CpuFeature::Bmi1) };
    CHECK(base == base);
    auto architecture = base;
    architecture.architecture = Architecture::Arm64;
    CHECK(architecture != base);
    auto abi = base;
    abi.abi = OperatingSystemAbi::Apple;
    CHECK(abi != base);
    auto features = base;
    features.features = CpuFeatures {};
    CHECK(features != base);
}

TEST_CASE("CpuFeatures.eachExtendsOneArchitecture")
{
    // x86-64's features, then arm64's: a set of one architecture's extends only it.
    for (auto const& row: CpuFeatureTable)
    {
        INFO("feature " << static_cast<int>(row.feature));
        auto const alone = CpuFeatures {}.with(row.feature);
        CHECK(extendsOnly(alone, row.architecture));
        auto const other = row.architecture == Architecture::X64 ? Architecture::Arm64 : Architecture::X64;
        CHECK_FALSE(extendsOnly(alone, other));
    }
    CHECK(CpuFeatureTable[static_cast<std::size_t>(CpuFeature::Cssc)].architecture == Architecture::Arm64);
    CHECK(extendsOnly(CpuFeatures {}, Architecture::X64));
    CHECK(extendsOnly(CpuFeatures {}, Architecture::Arm64));
}
