// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>

#include <isoline/compiler/machine/TargetDescription.hpp>
#include <isoline/execution/CpuFeatureProbe.hpp>
#include <isoline/execution/testing/CompileOne.hpp>

using isoline::compiler::machine::CpuFeature;
using isoline::compiler::machine::CpuFeatures;
using isoline::execution::cpuid;
using isoline::execution::cpuidFeaturesOf;
using isoline::execution::CpuidRegisters;
using isoline::execution::HostCpuFeatureProbe;
using isoline::execution::ICpuFeatureProbe;
using isoline::execution::testing::hostRunsCompiledCode;

namespace
{

/// A CPU of chosen CPUID words: what the api tests stand in for the running CPU, and what cpuidFeaturesOf()
/// is checked against here.
class FakeCpu
{
  public:
    /// Sets leaf @p leaf, subleaf @p subleaf to @p registers.
    FakeCpu& with(uint32_t leaf, uint32_t subleaf, CpuidRegisters registers)
    {
        _leaves[{ leaf, subleaf }] = registers;
        return *this;
    }

    [[nodiscard]] CpuidRegisters operator()(uint32_t leaf, uint32_t subleaf) const noexcept
    {
        auto const found = _leaves.find({ leaf, subleaf });
        return found == _leaves.end() ? CpuidRegisters {} : found->second;
    }

  private:
    std::map<std::pair<uint32_t, uint32_t>, CpuidRegisters> _leaves;
};

/// A probe that reports a fixed set: what the api tests inject.
class FakeCpuFeatureProbe final: public ICpuFeatureProbe
{
  public:
    explicit FakeCpuFeatureProbe(CpuFeatures features) noexcept: _features { features } {}

    [[nodiscard]] CpuFeatures features() const noexcept override { return _features; }

  private:
    CpuFeatures _features;
};

/// The bit @p bit of @p word.
[[nodiscard]] bool bitOf(uint32_t word, unsigned bit) noexcept
{
    return ((word >> bit) & 1U) != 0;
}

} // namespace

TEST_CASE("CpuFeatureProbe.consistentWithCpuid")
{
    auto const features = HostCpuFeatureProbe {}.features();
    if (!hostRunsCompiledCode())
    {
        // No CPUID: the architecture's baseline, which no selection row needs more than yet.
        CHECK(features == CpuFeatures {});
        CHECK(cpuid(1, 0) == CpuidRegisters {});
        return;
    }
    // The Intel SDM's bits, spelled here rather than read from the table, so that a row of the wrong bit
    // fails.
    auto const basic = cpuid(0, 0)[0];
    auto const leaf1 = cpuid(1, 0);
    CHECK(features.contains(CpuFeature::Sse41) == bitOf(leaf1[2], 19));
    CHECK(features.contains(CpuFeature::Sse42) == bitOf(leaf1[2], 20));
    CHECK(features.contains(CpuFeature::Popcnt) == bitOf(leaf1[2], 23));
    auto const leaf7 = basic >= 7 ? cpuid(7, 0) : CpuidRegisters {};
    CHECK(features.contains(CpuFeature::Bmi1) == bitOf(leaf7[1], 3));
    CHECK(features.contains(CpuFeature::Bmi2) == bitOf(leaf7[1], 8));
    auto const extended = cpuid(0x8000'0000, 0)[0];
    auto const leaf81 = extended >= 0x8000'0001 ? cpuid(0x8000'0001, 0) : CpuidRegisters {};
    CHECK(features.contains(CpuFeature::Lzcnt) == bitOf(leaf81[2], 5));
    // x86-64's baseline is SSE2; no CPU this runs on lacks SSE4.1's predecessor, so the probe saw a real
    // CPUID.
    CHECK(bitOf(cpuid(1, 0)[3], 26));
    CHECK_FALSE(features.contains(CpuFeature::Avx));
    CHECK_FALSE(features.contains(CpuFeature::Avx2));
}

TEST_CASE("CpuFeatureProbe.readsTheTableRows")
{
    SECTION("each row's bit, and nothing else")
    {
        auto const cpu = FakeCpu {}
                             .with(0, 0, { 7, 0, 0, 0 })
                             .with(1, 0, { 0, 0, (1U << 19U) | (1U << 23U), 0 })
                             .with(7, 0, { 0, 1U << 3U, 0, 0 })
                             .with(0x8000'0000, 0, { 0x8000'0001, 0, 0, 0 })
                             .with(0x8000'0001, 0, { 0, 0, 1U << 5U, 0 });
        CHECK(cpuidFeaturesOf(cpu)
              == CpuFeatures {}
                     .with(CpuFeature::Sse41)
                     .with(CpuFeature::Popcnt)
                     .with(CpuFeature::Bmi1)
                     .with(CpuFeature::Lzcnt));
    }
    SECTION("a leaf past the highest the CPU names is not read")
    {
        auto const cpu = FakeCpu {}
                             .with(0, 0, { 1, 0, 0, 0 })
                             .with(7, 0, { 0, ~0U, 0, 0 })
                             .with(0x8000'0000, 0, { 0x8000'0000, 0, 0, 0 })
                             .with(0x8000'0001, 0, { 0, 0, ~0U, 0 });
        CHECK(cpuidFeaturesOf(cpu) == CpuFeatures {});
    }
    SECTION("a fake probe reports what it was given")
    {
        auto const fake = FakeCpuFeatureProbe { CpuFeatures {}.with(CpuFeature::Sse41) };
        ICpuFeatureProbe const& probe = fake;
        CHECK(probe.features() == CpuFeatures {}.with(CpuFeature::Sse41));
    }
}
