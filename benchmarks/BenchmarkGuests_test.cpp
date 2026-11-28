// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>
#include <core/Sha256.hpp>
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "GuestRun.hpp"
#include <isoline/Features.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/testing/EngineRows.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

using isoline::Feature;
using isoline::FeatureSet;
using isoline::api::EngineConfig;
using isoline::api::testing::engineRows;
using isoline::benchmarks::GuestRow;
using isoline::benchmarks::GuestRunError;
using isoline::benchmarks::GuestRunRequest;
using isoline::benchmarks::parseGuestLock;
using isoline::benchmarks::runGuest;
using isoline::benchmarks::usesSimd;

namespace
{

/// The committed benchmarks directory.
std::filesystem::path benchmarksDirectory()
{
    return std::filesystem::path { ISOLINE_SOURCE_DIR } / "benchmarks";
}

/// The iterations every guest runs in the checksum case: few, so that it stays a unit test under a sanitizer,
/// and more than one, so that a guest whose state carries from one iteration to the next is compared too.
constexpr auto ChecksumIterations = uint32_t { 3 };

/// A feature of Contour's extension engines (src/vtextension/wasm/EngineFeatures.hpp, at the Contour commit
/// benchmarks/wamr/patches.lock names) that is On there, and the Isoline feature that is it.
struct ContourFeatureRow
{
    std::string_view contourName; ///< The row's name in EngineFeatures.
    Feature feature;              ///< Isoline's.
};

/// Every feature EngineFeatures has On and Isoline switches; every other row there is Off, and Isoline's
/// features beyond these are off in ContourWamrFeatures. Mutable globals, also On there, are part of the MVP
/// in Isoline, which has no switch for them. WAMR is held to the same table by Contour's build options
/// (benchmarks/wamr/ContourOptions.cmake).
constexpr auto ContourOnFeatures = std::to_array<ContourFeatureRow>({
    { .contourName = "bulk-memory", .feature = Feature::BulkMemory },
    { .contourName = "sign-extension", .feature = Feature::SignExtension },
    { .contourName = "saturating-float-to-int", .feature = Feature::NonTrappingFloatToInt },
    { .contourName = "multi-value", .feature = Feature::MultiValue },
    { .contourName = "reference-types", .feature = Feature::ReferenceTypes },
    { .contourName = "extended-const", .feature = Feature::ExtendedConst },
});

/// The features of Contour's WAMR build: ContourOnFeatures on, everything else off.
FeatureSet contourWamrFeatures()
{
    auto features = FeatureSet::none();
    for (auto const& row: ContourOnFeatures)
        features = features.with(row.feature);
    return features;
}

/// The contents of the file at @p path; empty when it cannot be read. Read at its size in one read: slurped
/// through an istreambuf_iterator instead, GCC 14's release build reports a null dereference in streambuf.
std::string readText(std::filesystem::path const& path)
{
    auto file = std::ifstream { path, std::ios::binary | std::ios::ate };
    auto const size = file ? static_cast<std::streamoff>(file.tellg()) : std::streamoff { -1 };
    if (size <= 0)
        return {};
    auto text = std::string(static_cast<std::size_t>(size), '\0');
    if (!file.seekg(0).read(text.data(), size))
        return {};
    return text;
}

/// The bytes of the guest @p row.
std::vector<std::byte> guestBytes(GuestRow const& row)
{
    auto const text = readText(benchmarksDirectory() / "guests" / row.file);
    auto bytes = std::vector<std::byte> {};
    bytes.reserve(text.size());
    for (auto const c: text)
        bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

/// The guests benchmarks/guests.lock pins.
std::vector<GuestRow> const& committedGuests()
{
    static auto const guests = [] {
        auto const parsed = parseGuestLock(readText(benchmarksDirectory() / "guests.lock"));
        REQUIRE(parsed.has_value());
        return *parsed;
    }();
    return guests;
}

/// An engine configuration of @p row's engine and dispatch, with every feature the guests may use.
EngineConfig configOf(isoline::api::testing::EngineRow const& row)
{
    auto config = EngineConfig {};
    config.engine = row.engine;
    config.dispatch = row.dispatch;
    config.features = row.features;
    return config;
}

/// A clock that moves one millisecond on at every reading, so that each phase runGuest() times between two
/// readings takes exactly that.
class SteppingClock final: public core::IClock
{
  public:
    [[nodiscard]] core::SteadyTimePoint now() const noexcept override
    {
        _now += std::chrono::milliseconds { 1 };
        return _now;
    }

  private:
    mutable core::SteadyTimePoint _now {}; ///< The last reading.
};

/// The run(i32) -> i64 type every guest's export has.
isoline::binary::FuncType runType()
{
    return isoline::binary::FuncType { .params = { isoline::binary::I32 },
                                       .results = { isoline::binary::I64 } };
}

/// One instruction in its raw encoding: the opcode, then its one-byte immediate where it has one.
isoline::binary::testing::Bytes instruction(uint8_t opcode, std::optional<uint8_t> immediate = std::nullopt)
{
    auto bytes = isoline::binary::testing::Bytes { opcode };
    if (immediate)
        bytes.push_back(*immediate);
    return bytes;
}

/// The opcodes the tests' modules use beyond binary::testing's.
constexpr auto I64ExtendI32U = uint8_t { 0xAD };
constexpr auto I64Add = uint8_t { 0x7C };
constexpr auto GlobalGet = uint8_t { 0x23 };
constexpr auto GlobalSet = uint8_t { 0x24 };

/// A module whose run(i32) -> i64 returns its argument plus one, as an i64.
std::vector<std::byte> incrementModule()
{
    using namespace isoline::binary::testing;
    auto builder = ModuleBuilder {};
    builder.type(runType())
        .function(0, {}, code({ localGet(0), instruction(I64ExtendI32U), i64Const(1), instruction(I64Add) }))
        .exportFunction("run", 0);
    return builder.build();
}

} // namespace

TEST_CASE("BenchmarkGuests.lockPinsEveryCommittedGuest", "[benchmark]")
{
    // Every guest of the lock is committed with the bytes it pins, and every committed guest is in the lock.
    auto const& guests = committedGuests();
    REQUIRE(guests.size() == 25);
    for (auto const& guest: guests)
    {
        CAPTURE(guest.file);
        auto const bytes = guestBytes(guest);
        REQUIRE_FALSE(bytes.empty());
        CHECK(core::toLowerHexString(core::Sha256::hash(bytes)) == guest.sha256);
    }
    auto committed = std::size_t { 0 };
    for (auto const& entry: std::filesystem::directory_iterator { benchmarksDirectory() / "guests" })
    {
        if (entry.path().extension() != ".wasm")
            continue;
        ++committed;
        auto const name = entry.path().filename().string();
        CAPTURE(name);
        CHECK(std::ranges::find(guests, name, &GuestRow::file) != guests.end());
    }
    CHECK(committed == guests.size());
}

TEST_CASE("BenchmarkGuests.checksumsAgreeAcrossEngines", "[benchmark]")
{
    // Every guest's run(3) returns the same checksum on every engine of the build as on the reference.
    auto const guest = GENERATE(from_range(committedGuests()));
    CAPTURE(guest.file);
    auto const bytes = guestBytes(guest);
    auto const clock = core::SteadyClock {};
    auto const request = GuestRunRequest { .module = bytes, .iterations = ChecksumIterations };
    auto const reference = runGuest(configOf(engineRows().front()), request, clock);
    REQUIRE(reference.has_value());
    for (auto const& row: engineRows() | std::views::drop(1))
    {
        CAPTURE(row);
        auto const outcome = runGuest(configOf(row), request, clock);
        REQUIRE(outcome.has_value());
        CHECK(outcome->checksum == reference->checksum);
    }
}

TEST_CASE("BenchmarkGuests.validateUnderContoursWamrFeatures", "[benchmark]")
{
    // Every guest but the SIMD one loads under the features Contour's WAMR is built with, so that WAMR can
    // run it; the SIMD one does not, which shows the set leaves SIMD out.
    auto config = EngineConfig {};
    config.features = contourWamrFeatures();
    REQUIRE(config.features.checkPrerequisites().has_value());
    auto const engine = isoline::api::Engine { config };
    auto simdGuests = std::size_t { 0 };
    for (auto const& guest: committedGuests())
    {
        CAPTURE(guest.file);
        auto const loaded = engine.load(guestBytes(guest));
        if (usesSimd(guest))
        {
            ++simdGuests;
            CHECK_FALSE(loaded.has_value());
        }
        else
            CHECK(loaded.has_value());
    }
    CHECK(simdGuests == 1);
}

TEST_CASE("GuestRun.timesItsPhasesByTheInjectedClock", "[benchmark]")
{
    auto const module = incrementModule();
    auto const clock = SteppingClock {};
    auto const outcome =
        runGuest(EngineConfig {}, GuestRunRequest { .module = module, .iterations = 41 }, clock);
    REQUIRE(outcome.has_value());
    CHECK(outcome->checksum == 42);
    CHECK(outcome->load == std::chrono::milliseconds { 1 });
    CHECK(outcome->invoke == std::chrono::milliseconds { 1 });
}

TEST_CASE("GuestRun.callsRunAsOftenAsAsked", "[benchmark]")
{
    // A guest that counts its calls in a global: run() returns the count, so the last result is the number of
    // invocations.
    using namespace isoline::binary::testing;
    auto builder = ModuleBuilder {};
    auto const counter = isoline::binary::GlobalType { .type = isoline::binary::I64,
                                                       .mutability = isoline::binary::Mutability::Var };
    builder.type(runType())
        .global(counter, i64Const(0))
        .function(0,
                  {},
                  code({ instruction(GlobalGet, 0),
                         i64Const(1),
                         instruction(I64Add),
                         instruction(GlobalSet, 0),
                         instruction(GlobalGet, 0) }))
        .exportFunction("run", 0);
    auto const module = builder.build();
    auto const clock = core::ManualClock {};
    auto const outcome = runGuest(
        EngineConfig {}, GuestRunRequest { .module = module, .iterations = 0, .invocations = 7 }, clock);
    REQUIRE(outcome.has_value());
    CHECK(outcome->checksum == 7);
}

TEST_CASE("GuestRun.namesTheStepThatFailed", "[benchmark]")
{
    auto const clock = core::ManualClock {};
    SECTION("a module the engine refuses")
    {
        auto const garbage = std::to_array<std::byte>({ std::byte { 0 }, std::byte { 1 } });
        auto const outcome = runGuest(EngineConfig {}, GuestRunRequest { .module = garbage }, clock);
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().error == GuestRunError::LoadFailed);
        CHECK(outcome.error().detail == "the module is malformed");
    }
    SECTION("a module without run")
    {
        auto const module = isoline::binary::testing::ModuleBuilder {}.build();
        auto const outcome = runGuest(EngineConfig {}, GuestRunRequest { .module = module }, clock);
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().error == GuestRunError::NoRunExport);
    }
    SECTION("a run that traps")
    {
        using namespace isoline::binary::testing;
        auto builder = ModuleBuilder {};
        builder.type(runType()).function(0, {}, unreachable()).exportFunction("run", 0);
        auto const module = builder.build();
        auto const outcome = runGuest(EngineConfig {}, GuestRunRequest { .module = module }, clock);
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().error == GuestRunError::CallFailed);
        CHECK(outcome.error().detail == "trap: unreachable");
    }
    SECTION("a run of another signature")
    {
        using namespace isoline::binary::testing;
        auto builder = ModuleBuilder {};
        builder.type(isoline::binary::FuncType {}).function(0, {}, {}).exportFunction("run", 0);
        auto const module = builder.build();
        auto const outcome = runGuest(EngineConfig {}, GuestRunRequest { .module = module }, clock);
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().error == GuestRunError::CallFailed);
        CHECK(outcome.error().detail == "argument mismatch");
    }
}

TEST_CASE("GuestLock.parsesHashAndFeatureRows", "[benchmark]")
{
    auto const hash = std::string(64, 'a');
    auto const parsed = parseGuestLock("# a comment\n"
                                       "b.wasm = "
                                       + hash
                                       + "\n"
                                         "b.wasm.features = mvp,bulk-memory\n"
                                         "s.wasm = "
                                       + hash + "  # trailing comment\n" + "s.wasm.features = mvp,simd\n");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 2);
    CHECK((*parsed)[0].file == "b.wasm");
    CHECK((*parsed)[0].sha256 == hash);
    CHECK_FALSE(usesSimd((*parsed)[0]));
    CHECK((*parsed)[1].file == "s.wasm");
    CHECK(usesSimd((*parsed)[1]));
}

TEST_CASE("GuestLock.refusesWhatIsNoRow", "[benchmark]")
{
    auto const hash = std::string(64, 'a');
    struct Case
    {
        std::string text;
        std::size_t line;
    };
    auto const testCase = GENERATE_COPY(values<Case>({
        { .text = "no equals sign\n", .line = 1 },
        { .text = "\nb.wasm = 1234\n", .line = 2 },
        { .text = "b.wasm = " + std::string(64, 'A') + "\n", .line = 1 },
        { .text = "b.txt = " + hash + "\n", .line = 1 },
        { .text = "b.wasm = " + hash + "\n", .line = 1 },
    }));
    CAPTURE(testCase.text);
    auto const parsed = parseGuestLock(testCase.text);
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().line == testCase.line);
}
