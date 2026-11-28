// SPDX-License-Identifier: Apache-2.0
//
// Contour's migration path (plan P11 Task 22): Contour's bundled clock extension, the Rust crate it builds
// for wasm32-wasip1, componentized with the preview-1 reactor adapter (clock.wasm, build-clock.sh), hosted on
// Isoline as Contour's extension host (P9) would host it: an engine of Contour's EngineFeatures, its
// status-line host as typed host functions, and the narrow WASI 0.2 the adapter imports bound by a test
// double (Wasi02Subset). Each case runs on every engine the build provides.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "Wasi02Subset.hpp"
#include <isoline/Features.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/api/CallError.hpp>
#include <isoline/component/api/Component.hpp>
#include <isoline/component/api/ComponentStore.hpp>
#include <isoline/component/api/Linker.hpp>
#include <isoline/component/api/Typed.hpp>
#include <isoline/component/api/testing/StatuslineWorld.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/runtime/testing/StoreFixtures.hpp>
#include <isoline/component/text/Forms.hpp>
#include <isoline/component/validate/Validator.hpp>
#include <isoline/runtime/testing/ProvidedMemory.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using isoline::Feature;
using isoline::FeatureSet;
using isoline::component::ComponentFeatureSet;
using isoline::component::api::Component;
using isoline::component::api::ComponentCallError;
using isoline::component::api::ComponentCallFailure;
using isoline::component::api::ComponentConfig;
using isoline::component::api::ComponentInstance;
using isoline::component::api::ComponentLoadError;
using isoline::component::api::ComponentStore;
using isoline::component::api::EngineCoreModuleLoader;
using isoline::component::api::EngineCoreModuleValidator;
using isoline::component::api::ImportOutcome;
using isoline::component::api::InstantiateComponentError;
using isoline::component::api::Linker;
using isoline::component::api::Satisfied;
using isoline::component::api::SatisfiedByTypes;
using isoline::component::api::Unsatisfied;
using isoline::component::api::testing::Case;
using isoline::component::api::testing::Event;
using isoline::component::api::testing::EventKinds;
using isoline::component::api::testing::RenderRequest;
using isoline::component::api::testing::StateSnapshot;
using isoline::component::api::testing::Tab;
using isoline::component::api::testing::Unit;
using isoline::component::api::typed::Borrow;
using isoline::component::api::typed::functionType;
using isoline::component::api::typed::hostBorrow;
using isoline::component::api::typed::makeTypedHostFunction;
using isoline::component::api::typed::Own;
using isoline::component::api::typed::TypedFunction;
using isoline::component::runtime::InstantiationFailure;
using isoline::component::runtime::InstantiationFailureKind;
using isoline::component::runtime::testing::Engines;
using isoline::component::testing::Datetime;
using isoline::component::testing::Descriptor;
using isoline::component::testing::DescriptorStat;
using isoline::component::testing::DescriptorType;
using isoline::component::testing::EnvironmentVariable;
using isoline::component::testing::Error;
using isoline::component::testing::ErrorCode;
using isoline::component::testing::ExitStatus;
using isoline::component::testing::FakeClock;
using isoline::component::testing::InputStream;
using isoline::component::testing::OutputStream;
using isoline::component::testing::StreamError;
using isoline::component::testing::Wasi02Failure;
using isoline::component::testing::Wasi02Options;
using isoline::component::testing::Wasi02Subset;
using isoline::runtime::testing::GrowDecision;
using isoline::runtime::testing::ProvidedMemoryAllocator;

namespace
{

// Contour's engine, as its extension host configures every engine it runs extensions on.

/// One On row of Contour's EngineFeatures table (src/vtextension/wasm/EngineFeatures.hpp at Contour commit
/// 54387ba8140a81e62e92a2b1beb9a76b591be04f), and the Isoline feature it is.
struct ContourFeatureRow
{
    std::string_view contourName; ///< Its row's name in Contour's table.
    std::optional<Feature>
        feature; ///< Isoline's; none for a feature of Isoline's baseline, which has no switch.
};

/// Contour's On rows, in its table's order.
constexpr auto ContourOnFeatures = std::to_array<ContourFeatureRow>({
    { .contourName = "bulk-memory", .feature = Feature::BulkMemory },
    { .contourName = "sign-extension", .feature = Feature::SignExtension },
    { .contourName = "saturating-float-to-int", .feature = Feature::NonTrappingFloatToInt },
    // The import and export of mutable globals: WebAssembly 1.0 as Isoline implements it.
    { .contourName = "mutable-globals", .feature = std::nullopt },
    { .contourName = "multi-value", .feature = Feature::MultiValue },
    { .contourName = "reference-types", .feature = Feature::ReferenceTypes },
    { .contourName = "extended-const", .feature = Feature::ExtendedConst },
});

/// Contour's Off rows, in its table's order: what every engine of Contour's refuses.
constexpr auto ContourOffFeatures = std::to_array<ContourFeatureRow>({
    { .contourName = "gc", .feature = std::nullopt }, // Isoline's GC cut: no switch, never on.
    { .contourName = "exceptions", .feature = Feature::ExceptionHandling },
    { .contourName = "threads", .feature = std::nullopt }, // Isoline implements no shared memory.
    { .contourName = "memory64", .feature = Feature::Memory64 },
    { .contourName = "multi-memory", .feature = Feature::MultiMemory },
    { .contourName = "tail-call", .feature = Feature::TailCall },
    { .contourName = "simd", .feature = Feature::Simd },
    { .contourName = "relaxed-simd", .feature = Feature::RelaxedSimd },
    // Off while Contour runs core modules; hosting components (P9) turns it On. No core feature of Isoline's:
    // components are loaded by component::api::Component.
    { .contourName = "component-model", .feature = std::nullopt },
    { .contourName = "function-references", .feature = Feature::TypedFunctionReferences },
    { .contourName = "relaxed-simd-deterministic", .feature = std::nullopt }, // a behaviour, not a feature
    { .contourName = "wide-arithmetic", .feature = Feature::WideArithmetic },
    { .contourName = "branch-hinting", .feature = Feature::BranchHinting },
    { .contourName = "custom-page-sizes", .feature = Feature::CustomPageSizes },
    { .contourName = "stack-switching", .feature = std::nullopt },
    // The component model's gates: ContourComponentFeatures.
    { .contourName = "component-model-async", .feature = std::nullopt },
    { .contourName = "component-model-async-builtins", .feature = std::nullopt },
    { .contourName = "component-model-async-stackful", .feature = std::nullopt },
    { .contourName = "component-model-map", .feature = std::nullopt },
});

/// Isoline's features Contour's table has no row for, which its engines therefore leave off: the text
/// format's annotations, and compact imports.
constexpr auto FeaturesContourLacks =
    std::to_array<Feature>({ Feature::Annotations, Feature::CompactImports });

/// The EngineConfig features equal to Contour's On rows.
constexpr auto ContourEngineFeatures = [] {
    auto set = FeatureSet::none();
    for (auto const& row: ContourOnFeatures)
        if (row.feature)
            set = set.with(*row.feature);
    return set;
}();

/// The component features: none of the gates. Contour's ComponentModelAsync, ComponentModelAsyncBuiltins,
/// ComponentModelAsyncStackful and ComponentModelMap rows are Off.
constexpr auto ContourComponentFeatures = ComponentFeatureSet::defaults();

// What is known of clock.wasm (README.md).

/// The file under tests/component/contour-clock.
[[nodiscard]] std::filesystem::path pathOf(std::string_view file)
{
    return std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "component" / "contour-clock" / file;
}

/// The address of wasi-libc's reactor guard in the main module's memory: the word `_initialize` checks is 0
/// and sets to 1 (its first `i32.load`, `wasm-tools print clock.wasm`), trapping when it runs a second time.
constexpr auto InitializeGuardAddress = std::size_t { 1'054'608 };

/// Who binds an import of clock.wasm.
enum class Binder : uint8_t
{
    Types,          ///< Nobody: an instance or a type of types only (SatisfiedByTypes).
    StatuslineHost, ///< Contour's status-line host.
    Wasi02Subset,   ///< The WASI 0.2 double.
};

/// One import of clock.wasm, as Component::imports() lists it, and who binds it.
struct PinnedImport
{
    std::string_view name;
    Binder binder;
};

/// clock.wasm's imports, in its order: the eleven WASI 0.2 interfaces the adapter kept of the ones it can
/// import (wit-component drops the rest), and the world's own. README.md records the WASI ones as P9's to-do.
constexpr auto PinnedImports = std::to_array<PinnedImport>({
    { .name = "wasi:cli/environment@0.2.12", .binder = Binder::Wasi02Subset },
    { .name = "wasi:cli/exit@0.2.12", .binder = Binder::Wasi02Subset },
    { .name = "wasi:io/error@0.2.12", .binder = Binder::Wasi02Subset },
    { .name = "wasi:io/streams@0.2.12", .binder = Binder::Wasi02Subset },
    { .name = "wasi:cli/stdin@0.2.12", .binder = Binder::Wasi02Subset },
    { .name = "wasi:cli/stdout@0.2.12", .binder = Binder::Wasi02Subset },
    { .name = "wasi:cli/stderr@0.2.12", .binder = Binder::Wasi02Subset },
    { .name = "wasi:clocks/monotonic-clock@0.2.12", .binder = Binder::Wasi02Subset },
    { .name = "wasi:clocks/wall-clock@0.2.12", .binder = Binder::Wasi02Subset },
    { .name = "wasi:filesystem/types@0.2.12", .binder = Binder::Wasi02Subset },
    { .name = "wasi:filesystem/preopens@0.2.12", .binder = Binder::Wasi02Subset },
    { .name = "terminal-extensions:statusline/types@0.1.0", .binder = Binder::Types },
    { .name = "terminal-extensions:statusline/host@0.1.0", .binder = Binder::StatuslineHost },
    { .name = "event", .binder = Binder::Types },
    { .name = "event-kinds", .binder = Binder::Types },
    { .name = "render-request", .binder = Binder::Types },
});

/// The bytes of @p file (REQUIREd to be read).
[[nodiscard]] std::vector<std::byte> bytesOf(std::string_view file)
{
    auto stream = std::ifstream { pathOf(file), std::ios::binary };
    REQUIRE(stream.is_open());
    auto const text =
        std::string { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
    auto bytes = std::vector<std::byte>(text.size());
    std::ranges::transform(
        text, bytes.begin(), [](char character) { return static_cast<std::byte>(character); });
    REQUIRE_FALSE(bytes.empty());
    return bytes;
}

/// What a refused load says: wasm-tools' message, which names a feature the engine lacks.
[[nodiscard]] std::string refusalOf(ComponentLoadError const& error)
{
    if (auto const* const decoding = std::get_if<isoline::component::binary::DecodeError>(&error))
        return isoline::component::binary::specMessage(*decoding);
    if (auto const* const validation = std::get_if<isoline::component::validate::ValidationError>(&error))
        return isoline::component::validate::specMessage(*validation);
    return std::format("refused at load (alternative {})", error.index());
}

/// An allocator that hands its inner one's memories out and keeps an eye on them: the guest's memory, read as
/// the embedder can read the memory it provides.
class ObservingAllocator final: public isoline::api::IMemoryAllocator
{
  public:
    explicit ObservingAllocator(std::unique_ptr<isoline::api::IMemoryAllocator> inner) noexcept:
        _inner { std::move(inner) }
    {
    }

    [[nodiscard]] std::expected<std::unique_ptr<isoline::api::ILinearMemory>, isoline::api::Refusal> allocate(
        isoline::api::MemoryRequest const& request) noexcept override
    {
        auto memory = _inner->allocate(request);
        if (memory)
            memories.push_back(memory->get());
        return memory;
    }

    /// The word at @p address of the first memory handed out (REQUIREd to be there).
    [[nodiscard]] uint32_t wordAt(std::size_t address) const
    {
        REQUIRE_FALSE(memories.empty());
        auto const bytes = memories.front()->bytes();
        REQUIRE(address + sizeof(uint32_t) <= bytes.size());
        auto word = uint32_t { 0 };
        std::memcpy(&word, bytes.subspan(address, sizeof(word)).data(), sizeof(word));
        return word;
    }

    /// The memories handed out, which the store owns: valid while it lives.
    std::vector<isoline::api::ILinearMemory*> memories {};

  private:
    std::unique_ptr<isoline::api::IMemoryAllocator> _inner;
};

/// Makes one store's memory allocator.
using Provision = std::function<std::unique_ptr<isoline::api::IMemoryAllocator>()>;

/// The heap: any number of memories, no guard.
[[nodiscard]] Provision heapProvision()
{
    return [] {
        return std::make_unique<isoline::api::HeapMemoryAllocator>();
    };
}

/// The memory Contour gives an extension (ExtensionLimits::memoryBytes) and the guard it declares (the
/// Wasmtime engine's SignalsGuardBytes).
constexpr auto ContourMemoryBytes = uint64_t { 32 } << 20;
constexpr auto ContourGuardBytes = uint64_t { 32 } << 20;

/// Contour's provision: one memory of ContourMemoryBytes followed by its guard, each grow as @p decide says.
[[nodiscard]] Provision contourProvision(std::function<GrowDecision(uint64_t)> decide = [](uint64_t) {
    return GrowDecision::Grant;
})
{
    return [decide = std::move(decide)] {
        return std::make_unique<ProvidedMemoryAllocator>(ContourMemoryBytes, ContourGuardBytes, decide);
    };
}

/// The time the runs start at, in milliseconds since the epoch: 11:06:40 UTC.
constexpr auto StartMilliseconds = uint64_t { 1'795'000'000'000 };
/// The time zone the host reports: an hour ahead of UTC.
constexpr auto UtcOffsetMinutes = int32_t { 60 };
constexpr auto NanosecondsPerMillisecond = uint64_t { 1'000'000 };
/// A WebAssembly page: what a memory grows by.
constexpr auto WasmPageBytes = uint64_t { 65'536 };

/// Contour's status-line host: `terminal-extensions:statusline/host@0.1.0`'s functions as clock.wasm imports
/// them, the state's time read from the run's FakeClock.
class StatuslineHost
{
  public:
    /// Defines the host's functions in @p linker.
    /// @param linker The linker; the host outlives every store instantiated with it.
    /// @param clock The time the state reports; it outlives the host.
    StatuslineHost(Linker& linker, FakeClock const& clock): _clock { &clock }
    {
        static constexpr auto AfterMs = std::to_array<std::string_view>({ "after-ms" });
        define<void(uint32_t)>(linker, "request-frame", AfterMs, [this](uint32_t afterMs) noexcept {
            frames.push_back(afterMs);
        });
        define<StateSnapshot()>(linker, "state", {}, [this]() noexcept {
            if (onState)
                onState();
            auto state = StateSnapshot {};
            state.unixTimeMs = _clock->unixTimeNanoseconds() / NanosecondsPerMillisecond;
            state.utcOffsetMinutes = UtcOffsetMinutes;
            state.tabs = tabs;
            return state;
        });
    }

    StatuslineHost(StatuslineHost const&) = delete;
    StatuslineHost(StatuslineHost&&) = delete;
    StatuslineHost& operator=(StatuslineHost const&) = delete;
    StatuslineHost& operator=(StatuslineHost&&) = delete;
    ~StatuslineHost() = default;

    std::vector<uint32_t> frames {};  ///< request-frame's delays, in order.
    std::vector<Tab> tabs {};         ///< The tabs the state reports.
    std::function<void()> onState {}; ///< Called at the start of each state(), on the calling thread.

  private:
    FakeClock const* _clock;

    template <typename Signature, typename Body>
    static void define(Linker& linker,
                       std::string_view name,
                       std::span<std::string_view const> labels,
                       Body body)
    {
        auto const spec = functionType<Signature>(linker, {}, labels);
        REQUIRE(spec.has_value());
        auto const defined =
            linker.defineFunction(std::format("terminal-extensions:statusline/host@0.1.0#{}", name),
                                  *spec,
                                  makeTypedHostFunction<Signature>(std::move(body)));
        if (!defined)
            FAIL(defined.error().message());
    }
};

/// An engine of Contour's features, clock.wasm loaded on it, the double and the host in one linker, and a
/// store.
class ClockRun
{
  public:
    ClockRun(isoline::api::ExecutionEngine engineKind, Provision const& provision = heapProvision()):
        engine { isoline::api::EngineConfig { .features = ContourEngineFeatures, .engine = engineKind } },
        loader { engine },
        validator { engine },
        linker { ComponentConfig { .features = ContourComponentFeatures } },
        clock { StartMilliseconds * NanosecondsPerMillisecond },
        wasi { defineWasi(linker, clock) },
        host { linker, clock },
        component { load() },
        memory { std::make_unique<ObservingAllocator>(provision()) },
        store { engine, *memory, ComponentConfig { .features = ContourComponentFeatures } }
    {
    }

    /// Instantiates clock.wasm in the run's store.
    [[nodiscard]] std::expected<ComponentInstance, InstantiateComponentError> instantiate()
    {
        return store.instantiate(component, linker);
    }

    /// Instantiates clock.wasm (REQUIREd to instantiate).
    [[nodiscard]] ComponentInstance instance()
    {
        auto instantiated = instantiate();
        REQUIRE(instantiated.has_value());
        return std::move(*instantiated);
    }

    isoline::api::Engine engine;
    EngineCoreModuleLoader loader;
    EngineCoreModuleValidator validator;
    Linker linker;
    FakeClock clock;
    std::unique_ptr<Wasi02Subset> wasi;
    StatuslineHost host;
    Component component;
    std::unique_ptr<ObservingAllocator> memory; ///< The store's allocator: it outlives the store.
    ComponentStore store;

  private:
    [[nodiscard]] static std::unique_ptr<Wasi02Subset> defineWasi(Linker& linker, FakeClock const& clock)
    {
        auto defined = Wasi02Subset::define(linker, clock);
        if (!defined)
            FAIL(defined.error().message());
        return std::move(*defined);
    }

    [[nodiscard]] Component load()
    {
        auto loaded = Component::load(engine,
                                      bytesOf("clock.wasm"),
                                      ComponentConfig { .features = ContourComponentFeatures },
                                      loader,
                                      validator);
        if (!loaded)
            FAIL(refusalOf(loaded.error()));
        // A Component has no move constructor: copying it shares its state.
        return *loaded;
    }
};

/// The world's exports, bound by type.
struct Exports
{
    TypedFunction<EventKinds(std::vector<EnvironmentVariable>)> load;
    TypedFunction<RenderRequest(Event)> update;
    TypedFunction<std::vector<uint8_t>(uint32_t, uint32_t)> render;
};

/// @p instance's exports (REQUIREd to be of the world's types).
[[nodiscard]] Exports exportsOf(ComponentInstance& instance)
{
    auto load = TypedFunction<EventKinds(std::vector<EnvironmentVariable>)>::bind(instance, "load");
    auto update = TypedFunction<RenderRequest(Event)>::bind(instance, "update");
    auto render = TypedFunction<std::vector<uint8_t>(uint32_t, uint32_t)>::bind(instance, "render");
    REQUIRE(load.has_value());
    REQUIRE(update.has_value());
    REQUIRE(render.has_value());
    return Exports { .load = std::move(*load), .update = std::move(*update), .render = std::move(*render) };
}

/// The value of a call that must succeed (REQUIREd, its error's message reported).
template <typename T>
[[nodiscard]] T valueOf(std::expected<T, ComponentCallError> outcome)
{
    if (!outcome)
        FAIL(outcome.error().message());
    return std::move(*outcome);
}

/// The error of a call that must fail (REQUIREd).
template <typename T>
[[nodiscard]] ComponentCallError errorOf(std::expected<T, ComponentCallError> const& outcome)
{
    REQUIRE_FALSE(outcome.has_value());
    return outcome.error();
}

/// @p text's bytes.
[[nodiscard]] std::vector<uint8_t> textBytes(std::string_view text)
{
    return { text.begin(), text.end() };
}

/// A timer event.
[[nodiscard]] Event timer()
{
    return Event { Case<"timer", Unit> {} };
}

/// The names the component @p instance's instance type @p name exports that a definition must bind: its
/// functions and the resources it defines.
[[nodiscard]] std::set<std::string> requiredBindingsOf(Component const& component, std::string_view name)
{
    auto const& imports = component.imports();
    auto const import =
        std::ranges::find_if(imports, [&](auto const& entry) { return entry.name.str() == name; });
    REQUIRE(import != imports.end());
    auto const* const instanceType = std::get_if<isoline::component::types::InstanceTypeId>(&import->type);
    REQUIRE(instanceType != nullptr);
    auto const& info = component.types()[*instanceType];
    auto required = std::set<std::string> {};
    for (auto const& entry: info.exports)
        if (std::holds_alternative<isoline::component::types::FunctionTypeId>(entry.type))
            required.emplace(entry.name.str());
    // A resource the interface declares is a type export of its own entry; one it uses (`eq` to another
    // interface's) names the entry it refers to.
    for (auto const& entry: info.exports)
        if (auto const* const type = std::get_if<isoline::component::types::TypeExtern>(&entry.type);
            type != nullptr && std::holds_alternative<isoline::component::types::ResourceTypeId>(type->type)
            && !type->referenced)
            required.emplace(entry.name.str());
    return required;
}

} // namespace

TEST_CASE("ContourClock.fixtureMatchesItsDigest")
{
    // README.md records clock.wasm's SHA-256, which Component::hash() is: a fixture rebuilt without its row
    // updated fails here, before any fact the other cases pin (its imports, the guard's address) is read
    // against it.
    auto readme = std::ifstream { pathOf("README.md") };
    REQUIRE(readme.is_open());
    auto row = std::optional<std::string> {};
    for (auto line = std::string {}; std::getline(readme, line);)
        if (line.starts_with("| `clock.wasm` |"))
            row = line;
    REQUIRE(row.has_value());
    auto run = ClockRun { isoline::api::ExecutionEngine::Reference };
    auto digest = std::string {};
    for (auto const byte: run.component.hash())
        digest += std::format("{:02x}", static_cast<unsigned>(byte));
    CHECK(row->ends_with(std::format("| `{}` |", digest)));
}

TEST_CASE("ContourClock.contourFeaturesTranscribed")
{
    // Every feature Isoline knows is one of Contour's rows, or one Contour lacks; the set holds exactly the
    // On rows' features.
    for (auto const& row: isoline::FeatureTable)
    {
        INFO(row.name);
        auto const isRow = [&](ContourFeatureRow const& contour) {
            return contour.feature == row.feature;
        };
        auto const isOn = std::ranges::any_of(ContourOnFeatures, isRow);
        auto const isOff = std::ranges::any_of(ContourOffFeatures, isRow);
        auto const isLacking =
            std::ranges::find(FeaturesContourLacks, row.feature) != FeaturesContourLacks.end();
        CHECK(int { isOn } + int { isOff } + int { isLacking } == 1);
        CHECK(ContourEngineFeatures.has(row.feature) == isOn);
    }
    CHECK(ContourOnFeatures.size() + ContourOffFeatures.size() == 26); // Contour's EngineFeature enumerators
}

TEST_CASE("ContourClock.importsAreTheAdapterSubset")
{
    auto const engine = GENERATE(from_range(Engines));
    auto run = ClockRun { engine };

    // imports() is the pinned list, in its order.
    auto names = std::vector<std::string_view> {};
    for (auto const& import: run.component.imports())
        names.push_back(import.name.str());
    auto pinned = std::vector<std::string_view> {};
    for (auto const& import: PinnedImports)
        pinned.push_back(import.name);
    CHECK(names == pinned);

    // The double binds exactly the WASI ones, and of each exactly its functions and resources.
    auto wasiImports = std::vector<std::string_view> {};
    for (auto const& import: PinnedImports)
        if (import.binder == Binder::Wasi02Subset)
            wasiImports.push_back(import.name);
    CHECK(Wasi02Subset::interfaces() == wasiImports);
    for (auto const interface: wasiImports)
    {
        INFO(interface);
        auto bound = std::set<std::string> {};
        for (auto const& binding: Wasi02Subset::bindings())
            if (binding.interface == interface)
                bound.emplace(binding.name);
        CHECK(bound == requiredBindingsOf(run.component, interface));
    }

    // Every import is bound: by the double, the host, or (types only) by nobody.
    auto const resolutions = run.linker.resolve(run.component);
    REQUIRE(resolutions.size() == PinnedImports.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, PinnedImports.size()))
    {
        auto const& resolution = resolutions[index];
        auto const& import = PinnedImports[index];
        INFO(import.name);
        if (import.binder == Binder::Types)
            CHECK(std::holds_alternative<SatisfiedByTypes>(resolution.outcome));
        else
            CHECK(resolution.outcome
                  == ImportOutcome { Satisfied { .matched = std::string { import.name } } });
    }

    // The double alone binds the WASI imports and nothing of the world's.
    auto wasiOnly = Linker { ComponentConfig { .features = ContourComponentFeatures } };
    auto const subset = Wasi02Subset::define(wasiOnly, run.clock);
    REQUIRE(subset.has_value());
    auto const wasiResolutions = wasiOnly.resolve(run.component);
    REQUIRE(wasiResolutions.size() == PinnedImports.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, PinnedImports.size()))
    {
        auto const& resolution = wasiResolutions[index];
        auto const& import = PinnedImports[index];
        INFO(import.name);
        if (import.binder == Binder::StatuslineHost)
            CHECK(std::holds_alternative<Unsatisfied>(resolution.outcome));
        else
            CHECK_FALSE(std::holds_alternative<Unsatisfied>(resolution.outcome));
    }
}

TEST_CASE("ContourClock.loadsUnderContourFeatures")
{
    auto const engine = GENERATE(from_range(Engines));
    // Contour's On rows are a closed set: each one's prerequisites are On too.
    CHECK(ContourEngineFeatures.checkPrerequisites().has_value());

    SECTION("Contour's features: it loads and validates")
    {
        auto engineOfContour = isoline::api::Engine { isoline::api::EngineConfig {
            .features = ContourEngineFeatures, .engine = engine } };
        auto loader = EngineCoreModuleLoader { engineOfContour };
        auto validator = EngineCoreModuleValidator { engineOfContour };
        auto const loaded = Component::load(engineOfContour,
                                            bytesOf("clock.wasm"),
                                            ComponentConfig { .features = ContourComponentFeatures },
                                            loader,
                                            validator);
        // A refusal is a P9 finding: its message names the feature the extension needs.
        if (!loaded)
            FAIL(refusalOf(loaded.error()));
    }

    SECTION("one of Contour's features fewer: loads, or the refusal names a feature it needs")
    {
        // What the extension needs of Contour's switchable On rows (a P9 finding): removing a feature removes
        // those built on it too, and a refusal names the first one the extension uses.
        struct Need
        {
            Feature removed;                     ///< The feature taken out of Contour's set.
            std::optional<Feature> refusalNames; ///< The feature the refusal names; none: it loads.
        };
        auto const need =
            GENERATE(Need { .removed = Feature::BulkMemory, .refusalNames = Feature::ReferenceTypes },
                     Need { .removed = Feature::SignExtension, .refusalNames = Feature::SignExtension },
                     Need { .removed = Feature::NonTrappingFloatToInt, .refusalNames = std::nullopt },
                     Need { .removed = Feature::MultiValue, .refusalNames = std::nullopt },
                     Need { .removed = Feature::ReferenceTypes, .refusalNames = Feature::ReferenceTypes },
                     Need { .removed = Feature::ExtendedConst, .refusalNames = std::nullopt });
        auto features = ContourEngineFeatures.without(need.removed);
        auto unmet = features.checkPrerequisites();
        while (!unmet)
        {
            features = features.without(unmet.error().feature);
            unmet = features.checkPrerequisites();
        }
        INFO(isoline::FeatureTable[static_cast<std::size_t>(need.removed)].name);
        auto engineWithout =
            isoline::api::Engine { isoline::api::EngineConfig { .features = features, .engine = engine } };
        auto loader = EngineCoreModuleLoader { engineWithout };
        auto validator = EngineCoreModuleValidator { engineWithout };
        auto const loaded = Component::load(engineWithout,
                                            bytesOf("clock.wasm"),
                                            ComponentConfig { .features = ContourComponentFeatures },
                                            loader,
                                            validator);
        if (!need.refusalNames)
        {
            if (!loaded)
                FAIL(refusalOf(loaded.error()));
            return;
        }
        REQUIRE_FALSE(loaded.has_value());
        auto const refusal = refusalOf(loaded.error());
        INFO(refusal);
        CHECK(refusal.contains(std::format(
            "({} not enabled)", isoline::FeatureTable[static_cast<std::size_t>(*need.refusalNames)].name)));
    }
}

TEST_CASE("ContourClock.initializeRunsOnce")
{
    // wit-component wires the reactor's `_initialize` as the start function of its fixup module, the last
    // core instance the component makes: instantiating runs it, before any export can be called, and nothing
    // the component exports reaches it. wasi-libc's guard makes a second run trap, so an instantiation that
    // succeeds ran it at most once; the guard's word being set shows it ran.
    auto const engine = GENERATE(from_range(Engines));
    auto run = ClockRun { engine };
    auto exportNames = std::vector<std::string_view> {};
    for (auto const& entry: run.component.exports())
        exportNames.push_back(entry.name.str());
    CHECK(exportNames == std::vector<std::string_view> { "load", "update", "render" });

    CHECK(run.memory->memories.empty());
    auto instance = run.instance();
    REQUIRE(run.memory->memories.size() == 1);
    CHECK(run.memory->wordAt(InitializeGuardAddress) == 1);

    // The guard was set when load() made its first host call: _initialize ran before load().
    auto guardAtFirstState = std::optional<uint32_t> {};
    run.host.onState = [&] {
        if (!guardAtFirstState)
            guardAtFirstState = run.memory->wordAt(InitializeGuardAddress);
    };
    auto exports = exportsOf(instance);
    CHECK(valueOf(exports.load(std::vector<EnvironmentVariable> {})) == EventKinds {});
    CHECK(guardAtFirstState == std::optional<uint32_t> { 1 });
    CHECK(valueOf(exports.render(80, 1)) == textBytes("12:06"));
    CHECK(run.memory->wordAt(InitializeGuardAddress) == 1);

    // A second instance runs its own _initialize, once, in its own memory.
    auto second = run.instance();
    REQUIRE(run.memory->memories.size() == 2);
    auto const bytes = run.memory->memories.back()->bytes();
    auto word = uint32_t { 0 };
    std::memcpy(&word, bytes.subspan(InitializeGuardAddress, sizeof(word)).data(), sizeof(word));
    CHECK(word == 1);
    CHECK_FALSE(run.store.isLockedDown());
}

TEST_CASE("ContourClock.renderShowsTime")
{
    auto const engine = GENERATE(from_range(Engines));

    SECTION("load, update(timer), render: the faked clock's time")
    {
        auto run = ClockRun { engine };
        auto instance = run.instance();
        auto exports = exportsOf(instance);

        // load reads the time (12:06 local, 40 s into the minute) and asks to be woken at the next minute.
        CHECK(valueOf(
                  exports.load(std::vector<EnvironmentVariable> { EnvironmentVariable { "format", "24h" } }))
              == EventKinds {});
        CHECK(run.host.frames == std::vector<uint32_t> { 20'000 });
        CHECK(valueOf(exports.render(80, 1)) == textBytes("12:06"));
        // A timer within the same minute changes nothing shown.
        CHECK(valueOf(exports.update(timer())) == RenderRequest::None);
        // At the minute's turn the time shown changes, and the next wake-up is a whole minute away.
        run.clock.advance(uint64_t { 20'000 } * NanosecondsPerMillisecond);
        CHECK(valueOf(exports.update(timer())) == RenderRequest::Render);
        CHECK(valueOf(exports.render(80, 1)) == textBytes("12:07"));
        CHECK(run.host.frames == std::vector<uint32_t> { 20'000, 20'000, 60'000 });
        // Other events change nothing.
        CHECK(valueOf(exports.update(Event { Case<"title", std::string> { "x" } })) == RenderRequest::None);
        // Nothing was written to a standard stream, and nothing read the environment.
        CHECK(run.wasi->capture().standardOutput.empty());
        CHECK(run.wasi->capture().standardError.empty());
        CHECK(run.wasi->capture().environmentReads == 0);
        CHECK_FALSE(run.store.isLockedDown());
    }

    SECTION("fd_write: the guest's stderr reaches the capture")
    {
        // Rust reports an allocation it cannot make on stderr (fd_write, which the adapter writes to
        // wasi:cli/stderr's stream) and aborts. Once load() has run the memory may grow by three pages only,
        // and the state the host reports holds 9,600 tabs: their canonical list (12 bytes a tab) fits, but
        // the Vec<Tab> wit-bindgen lifts it into (16 bytes a tab) does not, and what is left holds the 64 KiB
        // stack and the 64 KiB state the adapter allocates (through the main module's cabi_realloc) at its
        // first fd_write. (An allocation of wit-bindgen's own cabi_realloc that fails is a bare `unreachable`
        // in a release build, and writes nothing.)
        auto growLimit = std::make_shared<uint64_t>(ContourMemoryBytes);
        auto run = ClockRun { engine, contourProvision([growLimit](uint64_t newBytes) {
                                  return newBytes <= *growLimit ? GrowDecision::Grant : GrowDecision::Refuse;
                              }) };
        auto instance = run.instance();
        auto exports = exportsOf(instance);
        CHECK(valueOf(exports.load(std::vector<EnvironmentVariable> {})) == EventKinds {});
        *growLimit = run.memory->memories.front()->bytes().size() + (3 * WasmPageBytes);
        run.host.tabs = std::vector<Tab>(9'600);
        auto const error = errorOf(exports.update(timer()));
        CHECK(error.failure == ComponentCallFailure::Trap);
        auto const& standardError = run.wasi->capture().standardError;
        CHECK(std::string_view { reinterpret_cast<char const*>(standardError.data()), standardError.size() }
              == "memory allocation of 153600 bytes failed\n"
                 "note: run with `RUST_BACKTRACE=1` environment variable to display a backtrace\n");
        // The adapter opened the three standard streams, and Rust read the environment (for RUST_BACKTRACE)
        // once.
        CHECK(run.wasi->openStreams() == 3);
        CHECK(run.wasi->capture().environmentReads == 1);
        CHECK(run.wasi->capture().standardOutput.empty());
        CHECK(run.store.isLockedDown());
    }
}

TEST_CASE("ContourClock.embedderMemory")
{
    // Contour hands each extension one memory of its own, in a fixed buffer followed by a guard, refusing
    // every further request (A9). The componentized extension allocates exactly one: the adapter imports the
    // main module's memory instead of defining its own, so it runs there.
    auto const engine = GENERATE(from_range(Engines));
    auto run = ClockRun { engine, contourProvision() };
    CHECK(run.engine.requiredGuardBytes() <= ContourGuardBytes);
    auto instance = run.instance();
    CHECK(run.memory->memories.size() == 1);
    auto exports = exportsOf(instance);
    CHECK(valueOf(exports.load(std::vector<EnvironmentVariable> {})) == EventKinds {});
    CHECK(valueOf(exports.render(80, 1)) == textBytes("12:06"));

    // The provision is spent: a second instance gets no memory, and the refusal names the memory.
    auto const second = run.instantiate();
    REQUIRE_FALSE(second.has_value());
    auto const* const failure = std::get_if<InstantiationFailure>(&second.error());
    REQUIRE(failure != nullptr);
    CHECK(failure->kind == InstantiationFailureKind::CoreInstantiation);
    CHECK(failure->detail
          == decltype(failure->detail) { isoline::api::InstantiateError { isoline::api::AllocationFailure {
              .kind = isoline::api::ExternKind::Memory, .reason = isoline::api::Refusal::Embedder } } });
    CHECK(run.memory->memories.size() == 1);
}

// The double's own rows, called through wasi-subset.wat: a component that imports the WASI interfaces
// clock.wasm imports and re-exports each, so the host calls the double's functions as a guest would.

namespace
{

/// The double and the re-exporting component, instantiated.
class SubsetRun
{
  public:
    explicit SubsetRun(isoline::api::ExecutionEngine engineKind, Wasi02Options options = {}):
        engine { isoline::api::EngineConfig { .features = ContourEngineFeatures, .engine = engineKind } },
        loader { engine },
        validator { engine },
        linker { ComponentConfig {} },
        clock { StartMilliseconds * NanosecondsPerMillisecond },
        wasi { define(linker, clock, std::move(options)) },
        store { engine, allocator, ComponentConfig {} },
        instance { instantiate() }
    {
    }

    /// The re-exported function of @p interface named @p name, bound as @p Signature (REQUIREd to be).
    template <typename Signature>
    [[nodiscard]] TypedFunction<Signature> function(std::string_view interface, std::string_view name)
    {
        auto bound = TypedFunction<Signature>::bind(
            instance, std::format("{}#{}", interface, name), wasi->resources());
        REQUIRE(bound.has_value());
        return std::move(*bound);
    }

    /// An own of a stream from @p interface's @p name, the host holding it (REQUIREd to succeed).
    template <typename Tag>
    [[nodiscard]] Own<Tag> open(std::string_view interface, std::string_view name)
    {
        return valueOf(function<Own<Tag>()>(interface, name)());
    }

    isoline::api::Engine engine;
    EngineCoreModuleLoader loader;
    EngineCoreModuleValidator validator;
    Linker linker;
    FakeClock clock;
    std::unique_ptr<Wasi02Subset> wasi;
    isoline::api::HeapMemoryAllocator allocator {};
    ComponentStore store;
    ComponentInstance instance;

  private:
    [[nodiscard]] static std::unique_ptr<Wasi02Subset> define(Linker& linker,
                                                              FakeClock const& clock,
                                                              Wasi02Options options)
    {
        auto defined = Wasi02Subset::define(linker, clock, std::move(options));
        if (!defined)
            FAIL(defined.error().message());
        return std::move(*defined);
    }

    [[nodiscard]] ComponentInstance instantiate()
    {
        auto source = std::ifstream { pathOf("wasi-subset.wat") };
        REQUIRE(source.is_open());
        auto const text =
            std::string { std::istreambuf_iterator<char> { source }, std::istreambuf_iterator<char> {} };
        auto const bytes =
            isoline::text::compileText(text, isoline::text::TextLimits {}, isoline::component::text::Forms);
        REQUIRE(bytes.has_value());
        auto loaded = Component::load(engine, *bytes, ComponentConfig {}, loader, validator);
        if (!loaded)
            FAIL(refusalOf(loaded.error()));
        auto instantiated = store.instantiate(*loaded, linker);
        REQUIRE(instantiated.has_value());
        return std::move(*instantiated);
    }
};

constexpr auto Streams = std::string_view { "wasi:io/streams@0.2.12" };
constexpr auto Filesystem = std::string_view { "wasi:filesystem/types@0.2.12" };

/// What a stream call answers when the stream is closed.
template <typename T>
[[nodiscard]] bool isClosed(std::expected<T, StreamError> const& answer)
{
    return !answer.has_value() && std::holds_alternative<Case<"closed", Unit>>(answer.error());
}

} // namespace

TEST_CASE("Wasi02Subset.environment")
{
    auto const engine = GENERATE(from_range(Engines));
    auto const environment =
        std::vector<EnvironmentVariable> { { "TERM", "contour" }, { "LANG", "C.UTF-8" } };
    auto run = SubsetRun { engine, Wasi02Options { .environment = environment } };
    auto get =
        run.function<std::vector<EnvironmentVariable>()>("wasi:cli/environment@0.2.12", "get-environment");
    CHECK(valueOf(get()) == environment);
    CHECK(valueOf(get()) == environment);
    CHECK(run.wasi->capture().environmentReads == 2);
}

TEST_CASE("Wasi02Subset.exitTraps")
{
    auto const engine = GENERATE(from_range(Engines));
    auto const status = GENERATE(ExitStatus::Success, ExitStatus::Failure);
    auto run = SubsetRun { engine };
    auto exit = run.function<void(std::expected<Unit, Unit>)>("wasi:cli/exit@0.2.12", "exit");
    auto const argument = status == ExitStatus::Success
                              ? std::expected<Unit, Unit> { Unit {} }
                              : std::expected<Unit, Unit> { std::unexpected { Unit {} } };
    auto const error = errorOf(exit(argument));
    // exit never returns: the call fails with the double's failure, and the store is locked down.
    CHECK(error.failure == ComponentCallFailure::HostFailure);
    CHECK(error.detail
          == isoline::component::api::ComponentCallDetail {
              isoline::api::HostFailure { .kind = isoline::api::HostFailureKind::Embedder,
                                          .code = static_cast<uint32_t>(Wasi02Failure::Exited),
                                          .mismatch = std::nullopt } });
    CHECK(run.store.isLockedDown());
    CHECK(run.wasi->capture().exits == std::vector<ExitStatus> { status });
}

TEST_CASE("Wasi02Subset.clocksAreFakes")
{
    // Both clocks read the FakeClock: deterministic, and moved only by the test.
    auto const engine = GENERATE(from_range(Engines));
    auto run = SubsetRun { engine };
    auto monotonic = run.function<uint64_t()>("wasi:clocks/monotonic-clock@0.2.12", "now");
    auto wall = run.function<Datetime()>("wasi:clocks/wall-clock@0.2.12", "now");
    CHECK(valueOf(monotonic()) == 0);
    CHECK(valueOf(monotonic()) == 0);
    CHECK(valueOf(wall()) == Datetime { .seconds = 1'795'000'000, .nanoseconds = 0 });
    run.clock.advance(1'500'000'001);
    CHECK(valueOf(monotonic()) == 1'500'000'001);
    CHECK(valueOf(wall()) == Datetime { .seconds = 1'795'000'001, .nanoseconds = 500'000'001 });
}

TEST_CASE("Wasi02Subset.outputStreamsCapture")
{
    auto const engine = GENERATE(from_range(Engines));
    auto run = SubsetRun { engine, Wasi02Options { .writeCapacity = 512 } };
    auto checkWrite = run.function<std::expected<uint64_t, StreamError>(Borrow<OutputStream>)>(
        Streams, "[method]output-stream.check-write");
    auto write = run.function<std::expected<Unit, StreamError>(Borrow<OutputStream>, std::vector<uint8_t>)>(
        Streams, "[method]output-stream.write");
    auto writeAndFlush =
        run.function<std::expected<Unit, StreamError>(Borrow<OutputStream>, std::vector<uint8_t>)>(
            Streams, "[method]output-stream.blocking-write-and-flush");
    auto flush = run.function<std::expected<Unit, StreamError>(Borrow<OutputStream>)>(
        Streams, "[method]output-stream.blocking-flush");
    auto standardOutput = run.open<OutputStream>("wasi:cli/stdout@0.2.12", "get-stdout");
    auto standardError = run.open<OutputStream>("wasi:cli/stderr@0.2.12", "get-stderr");
    CHECK(run.wasi->openStreams() == 2);
    auto const borrow = [&](Own<OutputStream> const& own) {
        auto lent = hostBorrow<OutputStream>(run.store, run.wasi->resources(), own.rep());
        REQUIRE(lent.has_value());
        return *lent;
    };

    auto const capacity = valueOf(checkWrite(borrow(standardOutput)));
    REQUIRE(capacity.has_value());
    CHECK(*capacity == 512);
    CHECK(valueOf(write(borrow(standardOutput), textBytes("out "))).has_value());
    CHECK(valueOf(writeAndFlush(borrow(standardError), textBytes("err"))).has_value());
    CHECK(valueOf(writeAndFlush(borrow(standardOutput), textBytes("put"))).has_value());
    CHECK(valueOf(flush(borrow(standardOutput))).has_value());
    CHECK(run.wasi->capture().standardOutput == textBytes("out put"));
    CHECK(run.wasi->capture().standardError == textBytes("err"));

    // A dropped stream is destroyed, and is closed to a borrow that outlived it.
    auto const rep = standardError.rep();
    REQUIRE(isoline::component::api::typed::drop(run.store, standardError).has_value());
    CHECK(run.wasi->openStreams() == 1);
    auto stale = hostBorrow<OutputStream>(run.store, run.wasi->resources(), rep);
    REQUIRE(stale.has_value());
    CHECK(isClosed(valueOf(checkWrite(*stale))));
    CHECK(isClosed(valueOf(write(*stale, textBytes("lost")))));
    CHECK(isClosed(valueOf(flush(*stale))));
    CHECK(run.wasi->capture().standardError == textBytes("err"));
    CHECK_FALSE(run.store.isLockedDown());
}

TEST_CASE("Wasi02Subset.standardInputIsAStream")
{
    auto const engine = GENERATE(from_range(Engines));
    auto run = SubsetRun { engine };
    auto input = run.open<InputStream>("wasi:cli/stdin@0.2.12", "get-stdin");
    CHECK(run.wasi->openStreams() == 1);
    REQUIRE(isoline::component::api::typed::drop(run.store, input).has_value());
    CHECK(run.wasi->openStreams() == 0);
}

TEST_CASE("Wasi02Subset.streamTableIsBounded")
{
    // The double holds at most 16 streams of each kind: the 17th is refused, and the call fails.
    auto const engine = GENERATE(from_range(Engines));
    auto run = SubsetRun { engine };
    auto const isTableFull = [](ComponentCallError const& error) {
        return error.failure == ComponentCallFailure::HostFailure
               && error.detail
                      == isoline::component::api::ComponentCallDetail { isoline::api::HostFailure {
                          .kind = isoline::api::HostFailureKind::Embedder,
                          .code = static_cast<uint32_t>(Wasi02Failure::TableFull),
                          .mismatch = std::nullopt } };
    };
    auto const kind = GENERATE(std::string_view { "input" }, std::string_view { "output" });
    if (kind == "input")
    {
        auto get = run.function<Own<InputStream>()>("wasi:cli/stdin@0.2.12", "get-stdin");
        auto held = std::vector<Own<InputStream>> {};
        for ([[maybe_unused]] auto const index: std::views::iota(0, 16))
            held.push_back(valueOf(get()));
        CHECK(isTableFull(errorOf(get())));
        return;
    }
    auto get = run.function<Own<OutputStream>()>("wasi:cli/stdout@0.2.12", "get-stdout");
    auto held = std::vector<Own<OutputStream>> {};
    for ([[maybe_unused]] auto const index: std::views::iota(0, 16))
        held.push_back(valueOf(get()));
    CHECK(isTableFull(errorOf(get())));
}

TEST_CASE("Wasi02Subset.noFilesystem")
{
    // No directory is preopened, and a descriptor (which the double never hands out) is a bad one.
    auto const engine = GENERATE(from_range(Engines));
    auto run = SubsetRun { engine };
    auto directories = run.function<std::vector<std::tuple<Own<Descriptor>, std::string>>()>(
        "wasi:filesystem/preopens@0.2.12", "get-directories");
    CHECK(valueOf(directories()).empty());

    auto descriptor = hostBorrow<Descriptor>(run.store, run.wasi->resources(), 7);
    REQUIRE(descriptor.has_value());
    auto writeVia = run.function<std::expected<Own<OutputStream>, ErrorCode>(Borrow<Descriptor>, uint64_t)>(
        Filesystem, "[method]descriptor.write-via-stream");
    auto appendVia = run.function<std::expected<Own<OutputStream>, ErrorCode>(Borrow<Descriptor>)>(
        Filesystem, "[method]descriptor.append-via-stream");
    auto getType = run.function<std::expected<DescriptorType, ErrorCode>(Borrow<Descriptor>)>(
        Filesystem, "[method]descriptor.get-type");
    auto stat = run.function<std::expected<DescriptorStat, ErrorCode>(Borrow<Descriptor>)>(
        Filesystem, "[method]descriptor.stat");
    auto const isBad = [](auto const& answer) {
        return !answer.has_value() && answer.error() == ErrorCode::BadDescriptor;
    };
    CHECK(isBad(valueOf(writeVia(*descriptor, 0))));
    CHECK(isBad(valueOf(appendVia(*descriptor))));
    CHECK(isBad(valueOf(getType(*descriptor))));
    CHECK(isBad(valueOf(stat(*descriptor))));

    auto error = hostBorrow<Error>(run.store, run.wasi->resources(), 3);
    REQUIRE(error.has_value());
    auto errorCode =
        run.function<std::optional<ErrorCode>(Borrow<Error>)>(Filesystem, "filesystem-error-code");
    CHECK(valueOf(errorCode(*error)) == std::nullopt);
    CHECK_FALSE(run.store.isLockedDown());
}

TEST_CASE("Wasi02Subset.bindingsAreItsRows")
{
    // Every binding is one definition: a linker that already holds one refuses the double, naming it.
    auto clock = FakeClock { 0 };
    auto linker = Linker { ComponentConfig {} };
    auto const first = Wasi02Subset::define(linker, clock);
    REQUIRE(first.has_value());
    auto const second = Wasi02Subset::define(linker, clock);
    REQUIRE_FALSE(second.has_value());
    auto const& bindings = Wasi02Subset::bindings();
    REQUIRE_FALSE(bindings.empty());
    CHECK(second.error().message().contains(bindings.front().interface));
    auto distinct = std::set<std::pair<std::string_view, std::string_view>> {};
    for (auto const& binding: bindings)
        distinct.emplace(binding.interface, binding.name);
    CHECK(distinct.size() == bindings.size());
}
