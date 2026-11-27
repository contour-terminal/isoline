// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cfenv>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/FloatEnvironment.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/FloatEnvironment.hpp>
#include <isoline/validate/ValidatedModule.hpp>

using isoline::binary::FuncType;
using isoline::binary::testing::code;
using isoline::binary::testing::localGet;
using isoline::binary::testing::ModuleBuilder;
using isoline::interpreter::ReferenceInterpreter;
using isoline::runtime::F32;
using isoline::runtime::FloatControlWord;
using isoline::runtime::FloatEnvironmentScope;
using isoline::runtime::governedBits;
using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::hostFloatControlLayout;
using isoline::runtime::ieeeDefaults;
using isoline::runtime::IRoundingMode;
using isoline::runtime::libraryRoundingMode;
using isoline::runtime::readFloatControl;
using isoline::runtime::Store;
using isoline::runtime::V128;
using isoline::runtime::Value;
using isoline::runtime::testing::departFromIeeeDefaults;
using isoline::runtime::testing::HostFloatEnvironment;

namespace
{

/// The smallest positive subnormal f32.
constexpr auto SmallestSubnormal = uint32_t { 0x0000'0001 };

/// The largest finite f32: doubling it overflows.
constexpr auto Largest = uint32_t { 0x7F7F'FFFF };

/// f32 positive infinity.
constexpr auto Infinity = uint32_t { 0x7F80'0000 };

/// The positive canonical f32 NaN.
constexpr auto CanonicalNan = uint32_t { 0x7FC0'0000 };

/// f32 1.0.
constexpr auto One = uint32_t { 0x3F80'0000 };

/// f32 2^-30: added to 1.0, it rounds to 1.0 to nearest and to the next float up upward.
constexpr auto Tiny = uint32_t { 0x3080'0000 };

/// The bits of @p word the scope governs: control, not the sticky exception flags.
FloatControlWord governed(FloatControlWord word)
{
    return governedBits(hostFloatControlLayout(), word);
}

/// A module of f32.add (function 0), f32x4.eq (function 1) and f32.div (function 2), instantiated in a store
/// of its own.
class FloatModule
{
  public:
    FloatModule(): _store { {}, isoline::runtime::StoreServices { .allocator = _allocator } }
    {
        constexpr auto TypeF32 = isoline::binary::F32;
        constexpr auto TypeV128 = isoline::binary::V128;
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = { TypeF32, TypeF32 }, .results = { TypeF32 } })
            .type(FuncType { .params = { TypeV128, TypeV128 }, .results = { TypeV128 } })
            .function(0, {}, code({ localGet(0), localGet(1), { 0x92 } }))           // f32.add
            .function(1, {}, code({ localGet(0), localGet(1), { 0xFD }, { 0x41 } })) // f32x4.eq
            .function(0, {}, code({ localGet(0), localGet(1), { 0x95 } }));          // f32.div
        auto validated = isoline::validate::ValidatedModule::load(
            builder.build(), isoline::FeatureSet::all(), isoline::binary::DecodeLimits {});
        REQUIRE(validated.has_value());
        auto const address = isoline::runtime::instantiate(_store, _interpreter, std::move(*validated), {});
        REQUIRE(address.has_value());
        _address = *address;
    }

    /// f32.add of @p lhs and @p rhs, as the guest computes it.
    [[nodiscard]] uint32_t add(uint32_t lhs, uint32_t rhs)
    {
        return std::get<F32>(call(0, F32 { lhs }, F32 { rhs })).bits;
    }

    /// f32.div of @p lhs by @p rhs, as the guest computes it.
    [[nodiscard]] uint32_t divide(uint32_t lhs, uint32_t rhs)
    {
        return std::get<F32>(call(2, F32 { lhs }, F32 { rhs })).bits;
    }

    /// f32x4.eq of @p lhs and @p rhs, splatted to every lane, as the guest computes it: its lowest lane.
    [[nodiscard]] uint32_t equal(uint32_t lhs, uint32_t rhs)
    {
        auto const splat = [](uint32_t bits) {
            auto const pair = (uint64_t { bits } << 32U) | bits;
            return V128 { .lanes = { pair, pair } };
        };
        return static_cast<uint32_t>(std::get<V128>(call(1, splat(lhs), splat(rhs))).lanes[0]);
    }

  private:
    Value call(uint32_t index, Value lhs, Value rhs)
    {
        auto const args = std::vector { lhs, rhs };
        auto results = std::vector<Value>(1);
        auto const address = _store.moduleInstance(_address).functions.at(index);
        auto const invoked = isoline::runtime::invokeChecked(_store, _interpreter, address, args, results);
        REQUIRE(invoked.outcome.has_value());
        return results.front();
    }

    HeapMemoryAllocator _allocator;
    Store _store;
    ReferenceInterpreter _interpreter;
    uint32_t _address = 0;
};

} // namespace

TEST_CASE("FloatEnvironment.layoutRowIsConsistent")
{
    auto const& layout = hostFloatControlLayout();
    CHECK((layout.cleared & layout.set) == 0);
    CHECK((layout.flushes & ~layout.cleared) == 0);
    auto const flushing = FloatControlWord { layout.flushes | layout.cleared };
    CHECK((ieeeDefaults(layout, flushing) & layout.cleared) == 0);
    CHECK((ieeeDefaults(layout, flushing) & layout.set) == layout.set);
    CHECK(ieeeDefaults(layout, ieeeDefaults(layout, flushing)) == ieeeDefaults(layout, flushing));
    CHECK((layout.traps & ~(layout.cleared | layout.set)) == 0);
    CHECK((layout.flags & (layout.cleared | layout.set)) == 0);
}

TEST_CASE("FloatEnvironment.scopeRestoresTheHostEnvironment")
{
    auto const host = HostFloatEnvironment {};
    departFromIeeeDefaults();
    auto const departed = readFloatControl();
    {
        auto const scope = FloatEnvironmentScope { libraryRoundingMode() };
        CHECK(std::fegetround() == FE_TONEAREST);
        CHECK(governed(readFloatControl()) == governed(ieeeDefaults(hostFloatControlLayout(), departed)));
        {
            // Nested: the defaults stay, and so does the outer scope's restore.
            auto const nested = FloatEnvironmentScope { libraryRoundingMode() };
            CHECK(std::fegetround() == FE_TONEAREST);
        }
        CHECK(std::fegetround() == FE_TONEAREST);
    }
    CHECK(std::fegetround() == FE_UPWARD);
    CHECK(governed(readFloatControl()) == governed(departed));
}

TEST_CASE("FloatEnvironment.scopeRestoresAFlushingHost")
{
    // The host flushes subnormals but rounds to nearest: only the control register departs.
    auto const host = HostFloatEnvironment {};
    isoline::runtime::writeFloatControl(readFloatControl() | hostFloatControlLayout().flushes);
    // What the register kept: a core without half precision (Cortex-A53) holds FPCR.FZ16 at zero.
    auto const flushing = readFloatControl();
    CHECK(((flushing & hostFloatControlLayout().flushes) != 0) == (hostFloatControlLayout().flushes != 0));
    {
        auto const scope = FloatEnvironmentScope { libraryRoundingMode() };
        CHECK((readFloatControl() & hostFloatControlLayout().flushes) == 0);
    }
    CHECK(std::fegetround() == FE_TONEAREST);
    CHECK(governed(readFloatControl()) == governed(flushing));
}

TEST_CASE("FloatEnvironment.defaultsAreReestablishedOverAFlushingHost")
{
    // What a host function's return does (runtime::HostCallScope): a host that left the rounding mode
    // at nearest but flushes subnormals gets the register's defaults written back all the same.
    auto const host = HostFloatEnvironment {};
    isoline::runtime::writeFloatControl(readFloatControl() | hostFloatControlLayout().flushes);
    auto const flushing = readFloatControl();
    isoline::runtime::establishFloatDefaults(libraryRoundingMode());
    CHECK(std::fegetround() == FE_TONEAREST);
    CHECK((readFloatControl() & hostFloatControlLayout().flushes) == 0);
    CHECK(governed(readFloatControl()) == governed(ieeeDefaults(hostFloatControlLayout(), flushing)));
}

TEST_CASE("FloatEnvironment.scopeLeavesTheDefaultsAlone")
{
    auto const host = HostFloatEnvironment {};
    auto const before = readFloatControl();
    {
        auto const scope = FloatEnvironmentScope { libraryRoundingMode() };
        CHECK(governed(readFloatControl()) == governed(before));
    }
    CHECK(std::fegetround() == FE_TONEAREST);
    CHECK(governed(readFloatControl()) == governed(before));
}

TEST_CASE("FloatEnvironment.entryRestoresIeeeDefaults")
{
    auto module = FloatModule {};
    auto const host = HostFloatEnvironment {};
    departFromIeeeDefaults();
    auto const departed = readFloatControl();

    // Round-to-nearest-even, not upward.
    CHECK(module.add(One, Tiny) == One);
    // Subnormals are neither flushed (FTZ) nor read as zero (DAZ).
    CHECK(module.add(SmallestSubnormal, SmallestSubnormal) == 2 * SmallestSubnormal);
    CHECK(module.equal(SmallestSubnormal, 0) == 0);
    CHECK(module.equal(SmallestSubnormal, SmallestSubnormal) == 0xFFFF'FFFF);

    // The embedder's environment is its own again.
    CHECK(std::fegetround() == FE_UPWARD);
    CHECK(governed(readFloatControl()) == governed(departed));
}

TEST_CASE("FloatEnvironment.guestFlagsStayRaised")
{
    // The guest's overflow is status, not control: it stays raised whether or not the scope had to
    // restore the host's control state.
    auto module = FloatModule {};
    auto const host = HostFloatEnvironment {};
    SECTION("in the default environment")
    {
    }
    SECTION("in a departed environment")
    {
        departFromIeeeDefaults();
    }
    std::feclearexcept(FE_ALL_EXCEPT);
    // Read the flags before the assertions, whose own machinery is no part of what is tested.
    auto const sum = module.add(Largest, Largest);
    auto const overflowed = std::fetestexcept(FE_OVERFLOW) != 0;
    CHECK(sum == Infinity);
    CHECK(overflowed);
}

TEST_CASE("FloatEnvironment.hostTrapsDoNotReachTheGuest")
{
    // The host lets float exceptions trap (where its row can); the guest's 0 / 0 is a NaN, not a signal.
    auto module = FloatModule {};
    auto quotient = uint32_t {};
    {
        auto const host = HostFloatEnvironment {};
        auto const& layout = hostFloatControlLayout();
        isoline::runtime::writeFloatControl(ieeeDefaults(layout, readFloatControl()) ^ layout.traps);
        quotient = module.divide(0, 0);
        // No float operation runs here until the host's environment is back.
    }
    CHECK(quotient == CanonicalNan);
}

namespace
{

/// A C library whose fesetround() rewrites the whole control register, its sticky exception flags
/// cleared, as wine's ucrtbase does: what glibc and Microsoft's own leave alone.
class FlagClearingRoundingMode final: public IRoundingMode
{
  public:
    [[nodiscard]] int get() const noexcept override { return std::fegetround(); }

    void set(int mode) noexcept override
    {
        std::fesetround(mode);
        std::feclearexcept(FE_ALL_EXCEPT);
    }
};

/// A store on FlagClearingRoundingMode with one instance: "add" (f32, f32) -> f32, and
/// "add-then-host", which adds and then calls a host function that rounds upward.
class FlagClearingModule
{
  public:
    FlagClearingModule():
        _store { {}, isoline::runtime::StoreServices { .allocator = _allocator, .roundingMode = &_rounding } }
    {
        constexpr auto TypeF32 = isoline::binary::F32;
        auto const host = _store.addHostFunction(
            FuncType {},
            isoline::runtime::makeHostFunction(
                [](isoline::runtime::HostContext&,
                   std::span<Value const>,
                   std::span<Value>) noexcept -> std::expected<void, isoline::runtime::Abrupt> {
                    // A host that keeps the flags its own fesetround() might clear: only the store's switch
                    // back, through its rounding mode, is under test.
                    auto flags = std::fexcept_t {};
                    std::fegetexceptflag(&flags, FE_ALL_EXCEPT);
                    std::fesetround(FE_UPWARD);
                    std::fesetexceptflag(&flags, FE_ALL_EXCEPT);
                    return {};
                }));
        REQUIRE(host.has_value());
        auto builder = ModuleBuilder {};
        builder.type(FuncType {})
            .type(FuncType { .params = { TypeF32, TypeF32 }, .results = { TypeF32 } })
            .importFunction("env", "round-upward", 0)
            .function(1, {}, code({ localGet(0), localGet(1), { 0x92 } }))                  // f32.add
            .function(1, {}, code({ localGet(0), localGet(1), { 0x92 }, { 0x10, 0x00 } })); // f32.add; call 0
        auto validated = isoline::validate::ValidatedModule::load(
            builder.build(), isoline::FeatureSet::all(), isoline::binary::DecodeLimits {});
        REQUIRE(validated.has_value());
        auto const imports = std::vector { isoline::runtime::ExternValue {
            .kind = isoline::binary::ExternKind::Func, .address = *host } };
        auto const address =
            isoline::runtime::instantiate(_store, _interpreter, std::move(*validated), imports);
        REQUIRE(address.has_value());
        _address = *address;
    }

    /// Calls function @p index (1: add, 2: add then the host function) with @p lhs and @p rhs.
    /// @return The sum's bits.
    [[nodiscard]] uint32_t call(uint32_t index, uint32_t lhs, uint32_t rhs)
    {
        auto const args = std::vector<Value> { F32 { lhs }, F32 { rhs } };
        auto results = std::vector<Value>(1);
        auto const address = _store.moduleInstance(_address).functions.at(index);
        auto const invoked = isoline::runtime::invokeChecked(_store, _interpreter, address, args, results);
        REQUIRE(invoked.outcome.has_value());
        return std::get<F32>(results.front()).bits;
    }

  private:
    FlagClearingRoundingMode _rounding;
    HeapMemoryAllocator _allocator;
    Store _store;
    ReferenceInterpreter _interpreter;
    uint32_t _address = 0;
};

} // namespace

TEST_CASE("FloatEnvironment.guestFlagsSurviveAFlagClearingRoundingSwitch")
{
    // The rounding mode is switched back after the guest ran (the invocation's end, a host function's
    // return) by a C library that clears the flags: the guest's overflow, read from the register before
    // the switch, is written back after it. Rows whose register holds no flags cannot do that.
    if (hostFloatControlLayout().flags == 0)
        SKIP("the flags are not in this row's register: they stay raised only where fesetround() keeps them");
    auto const host = HostFloatEnvironment {};
    auto module = FlagClearingModule {};
    auto function = uint32_t { 0 };
    SECTION("at the invocation's end, to the host's upward rounding")
    {
        std::fesetround(FE_UPWARD);
        function = 1;
    }
    SECTION("at a host function's return, from its upward rounding")
    {
        function = 2;
    }
    std::feclearexcept(FE_ALL_EXCEPT);
    auto const sum = module.call(function, Largest, Largest);
    auto const overflowed = std::fetestexcept(FE_OVERFLOW) != 0;
    CHECK(sum == Infinity);
    CHECK(overflowed);
}
