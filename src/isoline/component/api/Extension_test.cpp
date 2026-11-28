// SPDX-License-Identifier: Apache-2.0
//
// The extension world end to end (plan P11 Task 21): the template Contour's extension host (P9) copies.
// ExtensionHost is a host written only against component::api and its typed bindings: the world's types
// as C++ types, every host function a typed noexcept body, the host-owned `surface` resource behind a
// HostResourceTable, and the exports called through TypedFunction. Each case drives both prebuilt guests
// of tests/component/extension, the hand-written component (extension.wat) and the one the C toolchain
// built (guest.c), on every engine the build provides.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Engine.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/api/CallError.hpp>
#include <isoline/component/api/Component.hpp>
#include <isoline/component/api/ComponentStore.hpp>
#include <isoline/component/api/Linker.hpp>
#include <isoline/component/api/Typed.hpp>
#include <isoline/component/api/testing/StatuslineWorld.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/runtime/testing/StoreFixtures.hpp>
#include <isoline/component/text/Forms.hpp>
#include <isoline/runtime/testing/ProvidedMemory.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using isoline::component::ComponentLimits;
using isoline::component::api::Component;
using isoline::component::api::ComponentCallError;
using isoline::component::api::ComponentCallFailure;
using isoline::component::api::ComponentConfig;
using isoline::component::api::ComponentInstance;
using isoline::component::api::ComponentStore;
using isoline::component::api::CoreModuleKey;
using isoline::component::api::EngineCoreModuleLoader;
using isoline::component::api::EngineCoreModuleValidator;
using isoline::component::api::HostComponentContext;
using isoline::component::api::HostResourceType;
using isoline::component::api::ICoreModuleLoader;
using isoline::component::api::InstantiateComponentError;
using isoline::component::api::Linker;
using isoline::component::api::ParameterNames;
using isoline::component::api::Satisfied;
using isoline::component::api::Unsatisfied;
using isoline::component::api::UnsatisfiedImport;
using isoline::component::api::testing::Assignment;
using isoline::component::api::testing::CommandError;
using isoline::component::api::testing::CommandOutput;
using isoline::component::api::testing::Event;
using isoline::component::api::testing::EventKinds;
using isoline::component::api::testing::FontMetrics;
using isoline::component::api::testing::InputMode;
using isoline::component::api::testing::LogLevel;
using isoline::component::api::testing::Progress;
using isoline::component::api::testing::ProgressState;
using isoline::component::api::testing::RenderRequest;
using isoline::component::api::testing::Size;
using isoline::component::api::testing::StateSnapshot;
using isoline::component::api::testing::Tab;
using isoline::component::api::typed::Borrow;
using isoline::component::api::typed::Case;
using isoline::component::api::typed::functionType;
using isoline::component::api::typed::hostOwn;
using isoline::component::api::typed::HostResourceTable;
using isoline::component::api::typed::makeTypedHostFunction;
using isoline::component::api::typed::Own;
using isoline::component::api::typed::ResourceTypes;
using isoline::component::api::typed::TypedFunction;
using isoline::component::api::typed::Unit;
using isoline::component::canonical::Trap;
using isoline::component::canonical::TrapCode;
using isoline::component::runtime::IHostResourceDestructor;
using isoline::component::runtime::InstantiationFailure;
using isoline::component::runtime::InstantiationFailureKind;
using isoline::component::runtime::testing::Engines;
using isoline::runtime::testing::GrowDecision;
using isoline::runtime::testing::ProvidedMemoryAllocator;

namespace
{

// The world's types as the host's C++ types (extension.wit): Contour's status-line world's
// (StatuslineWorld.hpp), and those of the host-owned surface and the control interface.

/// The event-kinds bits of `title` and `cwd`.
constexpr auto TitleAndCwd = EventKinds { .bits = 0b11 };

/// host.surface's C++ tag.
struct Surface
{
};

/// control.tab-info.
struct TabInfo
{
    std::string id;
    std::string title;
    bool isActive = false;

    bool operator==(TabInfo const&) const = default;
};

/// control.pane.
struct Pane
{
    std::string id;
    std::optional<std::string> title;
    std::vector<TabInfo> tabs;

    bool operator==(Pane const&) const = default;
};

/// control.control-error.
using ControlError = std::variant<Case<"bad-request", std::string>,
                                  Case<"not-found", std::string>,
                                  Case<"forbidden", Unit>,
                                  Case<"failed", std::string>>;

/// What control.list-panes answers.
using PaneList = std::expected<std::vector<Pane>, ControlError>;

} // namespace

namespace isoline::component::api::typed
{

template <>
struct Record<TabInfo>
{
    static constexpr auto Fields = std::tuple { field("id", &TabInfo::id),
                                                field("title", &TabInfo::title),
                                                field("is-active", &TabInfo::isActive) };
};

template <>
struct Record<Pane>
{
    static constexpr auto Fields =
        std::tuple { field("id", &Pane::id), field("title", &Pane::title), field("tabs", &Pane::tabs) };
};

} // namespace isoline::component::api::typed

namespace
{

namespace typed = isoline::component::api::typed;

// The host: what Contour's extension host builds per extension.

/// A surface the host owns: what a surface handle's rep finds in the host's table.
struct SurfaceObject
{
    std::vector<uint8_t> bytes {}; ///< Everything the guest wrote to it.
};

/// What the host saw the guest do, in order.
struct Journal
{
    std::vector<std::pair<LogLevel, std::string>> logs {}; ///< host.log's calls.
    std::vector<uint32_t> frames {};                       ///< host.request-frame's delays.
    std::vector<std::pair<std::string, std::vector<Assignment>>> commands {}; ///< host.run-command's calls.
    std::vector<std::optional<std::string>> filters {}; ///< control.list-panes' filters.
    std::vector<std::vector<uint8_t>> presented {};     ///< The bytes of each surface host.present took.
    std::vector<uint32_t> destroyed {};                 ///< The surfaces destroyed, by rep.
    /// Why host.open-surface could not hand a surface out: the trap hostOwn() refused it with.
    std::vector<TrapCode> refusedOwns {};
};

/// What host.present does with the surface it is given.
enum class PresentPolicy : uint8_t
{
    Drop,   ///< Drops it, as every host body must drop each own it is given and does not hand back.
    Forget, ///< Keeps it without dropping it: a host bug, which maxHostOwns bounds.
};

/// How an ExtensionHost is set up.
struct HostOptions
{
    std::string version = "0.1.0";               ///< The version its interfaces are defined at.
    StateSnapshot state {};                      ///< What host.state answers.
    std::optional<CommandOutput> status {};      ///< What run-command("status") gives; none: not declared.
    Size surfaceSize { .cols = 80, .rows = 1 };  ///< What surface.size answers.
    PresentPolicy present = PresentPolicy::Drop; ///< What host.present does.
    /// The most bytes the arguments of one host call may take: Contour's maxHostCallBytes. None: the store's
    /// cap alone.
    std::optional<uint64_t> maxCallBytes {};
    /// Called with each message host.log receives, on the calling thread.
    std::function<void(std::string_view message)> onLog {};
};

/// Destroys a surface: erases its object from the host's table, recording its rep.
class SurfaceDestructor final: public IHostResourceDestructor
{
  public:
    SurfaceDestructor(HostResourceTable<SurfaceObject>& surfaces, std::vector<uint32_t>& destroyed) noexcept:
        _surfaces { &surfaces }, _destroyed { &destroyed }
    {
    }

    void destroy(uint32_t rep) noexcept override
    {
        _destroyed->push_back(rep);
        _surfaces->erase(rep);
    }

  private:
    HostResourceTable<SurfaceObject>* _surfaces;
    std::vector<uint32_t>* _destroyed;
};

/// Why a host body failed: its HostFailure's code, which tells one failure from another.
enum class HostError : uint8_t
{
    UnknownSurface = 1, ///< A borrow of a surface the host's table does not hold.
    SurfaceTableFull,   ///< The host's surfaces table is full (HostResourceTable's Handles trap).
    OwnRefused,         ///< The store refused the own (hostOwn(): HostOwns or LiveValueBytes).
    DropRefused,        ///< The store refused to drop an own the host was given.
};

/// An embedder's failure a host body reports: what ends the call and locks the store down.
[[nodiscard]] isoline::api::HostFailure hostFailure(HostError error) noexcept
{
    return isoline::api::HostFailure { .kind = isoline::api::HostFailureKind::Embedder,
                                       .code = static_cast<uint32_t>(error),
                                       .mismatch = std::nullopt };
}

/// The host side of the world: a linker binding `host` and `control`, every function a typed body over the
/// host's own state. Neither copied nor moved: its bodies refer to it.
class ExtensionHost
{
  public:
    explicit ExtensionHost(HostOptions options, ComponentConfig const& config = {}):
        linker { config },
        surface { "surface", std::make_shared<SurfaceDestructor>(surfaces, journal.destroyed) },
        resources { surface },
        _options { std::move(options) }
    {
        defineHost();
        defineControl();
    }

    ExtensionHost(ExtensionHost const&) = delete;
    ExtensionHost(ExtensionHost&&) = delete;
    ExtensionHost& operator=(ExtensionHost const&) = delete;
    ExtensionHost& operator=(ExtensionHost&&) = delete;
    ~ExtensionHost() = default;

    Linker linker;                                    ///< What the extension is instantiated with.
    Journal journal {};                               ///< What the guest did.
    HostResourceTable<SurfaceObject> surfaces { 64 }; ///< The surfaces' objects, by rep.

    HostResourceType surface;         ///< host.surface.
    ResourceTypes<Surface> resources; ///< The resource types the typed bindings name.

    /// The linker path of @p name in @p interface, at the host's version.
    [[nodiscard]] std::string pathOf(std::string_view interface, std::string_view name) const
    {
        return std::format("isoline-test:extension/{}@{}#{}", interface, _options.version, name);
    }

  private:
    HostOptions _options;

    /// Defines @p body at @p name of @p interface as a typed host function of @p Signature, its parameters
    /// labelled @p labels and matched by name as @p names says.
    template <typename Signature, typename F>
    void define(std::string_view interface,
                std::string_view name,
                std::span<std::string_view const> labels,
                ParameterNames names,
                F body)
    {
        auto spec = functionType<Signature>(linker, resources, labels);
        REQUIRE(spec.has_value());
        spec->names = names;
        spec->maxCallBytes = _options.maxCallBytes;
        auto const defined = linker.defineFunction(
            pathOf(interface, name), *spec, makeTypedHostFunction<Signature>(std::move(body)));
        if (!defined)
            FAIL(defined.error().message());
    }

    /// `host`: its parameters labelled as the WIT names them, which an import must match.
    void defineHost()
    {
        auto const defined = linker.defineResource(pathOf("host", "surface"), surface);
        if (!defined)
            FAIL(defined.error().message());
        static constexpr auto Self = std::to_array<std::string_view>({ "self" });
        static constexpr auto Write = std::to_array<std::string_view>({ "self", "bytes" });
        static constexpr auto AfterMs = std::to_array<std::string_view>({ "after-ms" });
        static constexpr auto Log = std::to_array<std::string_view>({ "level", "message" });
        static constexpr auto RunCommand = std::to_array<std::string_view>({ "name", "args" });
        static constexpr auto Present = std::to_array<std::string_view>({ "surface" });
        auto constexpr Named = ParameterNames::Named;

        define<void(Borrow<Surface>, std::vector<uint8_t>)>(
            "host",
            "[method]surface.write",
            Write,
            Named,
            [this](Borrow<Surface> self,
                   std::vector<uint8_t> bytes) noexcept -> std::expected<void, isoline::api::HostFailure> {
                auto* const object = surfaces.find(self.rep());
                if (object == nullptr)
                    return std::unexpected { hostFailure(HostError::UnknownSurface) };
                object->bytes.insert(object->bytes.end(), bytes.begin(), bytes.end());
                return {};
            });
        define<Size(Borrow<Surface>)>(
            "host", "[method]surface.size", Self, Named, [this](Borrow<Surface>) noexcept {
                return _options.surfaceSize;
            });
        define<void(uint32_t)>("host", "request-frame", AfterMs, Named, [this](uint32_t afterMs) noexcept {
            journal.frames.push_back(afterMs);
        });
        define<void(LogLevel, std::string)>(
            "host", "log", Log, Named, [this](LogLevel level, std::string message) noexcept {
                if (_options.onLog)
                    _options.onLog(message);
                journal.logs.emplace_back(level, std::move(message));
            });
        define<StateSnapshot()>("host", "state", {}, Named, [this]() noexcept { return _options.state; });
        define<std::expected<CommandOutput, CommandError>(std::string, std::vector<Assignment>)>(
            "host",
            "run-command",
            RunCommand,
            Named,
            [this](std::string const& name,
                   std::vector<Assignment> args) noexcept -> std::expected<CommandOutput, CommandError> {
                journal.commands.emplace_back(name, std::move(args));
                if (name != "status" || !_options.status)
                    return std::unexpected { CommandError::NotDeclared };
                return *_options.status;
            });
        define<Own<Surface>()>("host",
                               "open-surface",
                               {},
                               Named,
                               [this](HostComponentContext& context) noexcept
                                   -> std::expected<Own<Surface>, isoline::api::HostFailure> {
                                   auto const rep = surfaces.insert(SurfaceObject {});
                                   if (!rep)
                                       return std::unexpected { hostFailure(HostError::SurfaceTableFull) };
                                   auto own = hostOwn<Surface>(context.store(), resources, *rep);
                                   if (!own)
                                   {
                                       journal.refusedOwns.push_back(own.error().code);
                                       surfaces.erase(*rep);
                                       return std::unexpected { hostFailure(HostError::OwnRefused) };
                                   }
                                   return std::move(*own);
                               });
        // P9: a host body drops every Own it is given and does not hand back (typed::drop); one it forgets
        // stays held, against maxHostOwns, until the store ends.
        define<void(Own<Surface>)>("host",
                                   "present",
                                   Present,
                                   Named,
                                   [this](HostComponentContext& context, Own<Surface> presented) noexcept
                                       -> std::expected<void, isoline::api::HostFailure> {
                                       if (auto const* const object = surfaces.find(presented.rep()))
                                           journal.presented.push_back(object->bytes);
                                       if (_options.present == PresentPolicy::Forget)
                                           return {};
                                       if (!typed::drop(context.store(), presented))
                                           return std::unexpected { hostFailure(HostError::DropRefused) };
                                       return {};
                                   });
    }

    /// `control`: a generated catalog's functions, defined unnamed (ParameterNames::Unnamed), so an import
    /// matches whatever it calls their parameters, as a Wasmtime host function does.
    void defineControl()
    {
        define<PaneList(std::optional<std::string>)>(
            "control",
            "list-panes",
            {},
            ParameterNames::Unnamed,
            [this](std::optional<std::string> filter) noexcept -> PaneList {
                journal.filters.push_back(filter);
                if (filter && *filter != "a")
                    return std::unexpected { ControlError { Case<"not-found", std::string> { *filter } } };
                return std::vector<Pane> {
                    Pane { .id = "a1",
                           .title = std::nullopt,
                           .tabs = { TabInfo { .id = "t1", .title = "one", .isActive = false },
                                     TabInfo { .id = "t2", .title = "two", .isActive = true } } },
                };
            });
    }
};

// The guests and their runs.

/// One prebuilt guest of the world, and what the tests know of how it was built.
struct Fixture
{
    std::string_view name;     ///< How the test names it.
    std::string_view file;     ///< Its file under tests/component/extension.
    uint32_t coreModules = 0;  ///< Its embedded core modules, every one loaded through the loader.
    uint32_t coreMemories = 0; ///< The memories one instantiation allocates (A9: a provision holds one).
    uint64_t initialMemoryBytes = 0; ///< Its memory's initial size.
};

/// Both guests: the hand-written one has a module for its memory and allocator and one for its code; the C
/// toolchain's, wit-component's three (the code, a shim and its fixup).
constexpr auto Fixtures = std::to_array<Fixture>({
    { .name = "hand-written (extension.wat)",
      .file = "extension.wasm",
      .coreModules = 2,
      .coreMemories = 1,
      .initialMemoryBytes = 65'536 },
    { .name = "C toolchain (guest.c)",
      .file = "guest.wasm",
      .coreModules = 3,
      .coreMemories = 1,
      .initialMemoryBytes = 131'072 },
});

/// The bytes of @p fixture's file (REQUIREd to be read).
[[nodiscard]] std::vector<std::byte> bytesOf(Fixture const& fixture)
{
    auto const path =
        std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "component" / "extension" / fixture.file;
    auto file = std::ifstream { path, std::ios::binary };
    REQUIRE(file.is_open());
    auto bytes = std::vector<std::byte> {};
    auto chunk = std::array<char, 4'096> {};
    while (file.read(chunk.data(), chunk.size()) || file.gcount() > 0)
    {
        auto const read = std::span { chunk }.first(static_cast<std::size_t>(file.gcount()));
        for (auto const character: read)
            bytes.push_back(static_cast<std::byte>(character));
    }
    REQUIRE_FALSE(bytes.empty());
    return bytes;
}

/// A loader that loads through the engine and records every key it is asked for: the seam a module cache
/// (P8) serves compiled modules through.
class CountingLoader final: public ICoreModuleLoader
{
  public:
    explicit CountingLoader(isoline::api::Engine const& engine) noexcept: _inner { engine } {}

    [[nodiscard]] std::expected<isoline::api::Module, isoline::api::LoadError> load(
        CoreModuleKey key, std::span<std::byte const> bytes) noexcept override
    {
        keys.push_back(key);
        return _inner.load(key, bytes);
    }

    std::vector<CoreModuleKey> keys {}; ///< Every key, in the order asked.

  private:
    EngineCoreModuleLoader _inner;
};

/// An allocator that counts the memories it hands out from the one it owns.
class CountingAllocator final: public isoline::api::IMemoryAllocator
{
  public:
    explicit CountingAllocator(std::unique_ptr<isoline::api::IMemoryAllocator> inner) noexcept:
        _inner { std::move(inner) }
    {
    }

    [[nodiscard]] std::expected<std::unique_ptr<isoline::api::ILinearMemory>, isoline::api::Refusal> allocate(
        isoline::api::MemoryRequest const& request) noexcept override
    {
        auto memory = _inner->allocate(request);
        if (memory)
            ++allocated;
        return memory;
    }

    uint32_t allocated = 0; ///< The memories handed out.

  private:
    std::unique_ptr<isoline::api::IMemoryAllocator> _inner;
};

/// Makes one store's memory allocator: a store's provision is its own, so a restart takes a fresh one.
using Provision = std::function<std::unique_ptr<isoline::api::IMemoryAllocator>()>;

/// The memory Contour gives an extension: ExtensionLimits::memoryBytes.
constexpr auto ContourMemoryBytes = uint64_t { 32 } << 20;
/// The guard Contour's provided memory declares: the Wasmtime engine's SignalsGuardBytes.
constexpr auto ContourGuardBytes = uint64_t { 32 } << 20;

/// The heap: any number of memories, no guard.
[[nodiscard]] Provision heapProvision()
{
    return [] {
        return std::make_unique<isoline::api::HeapMemoryAllocator>();
    };
}

/// Contour's provision: one memory of @p objectBytes followed by its guard, every grow granted.
[[nodiscard]] Provision contourProvision(uint64_t objectBytes = ContourMemoryBytes)
{
    return [objectBytes] {
        return std::make_unique<ProvidedMemoryAllocator>(
            objectBytes, ContourGuardBytes, [](uint64_t) { return GrowDecision::Grant; });
    };
}

/// What a run is set up with beside its fixture and engine.
struct RunOptions
{
    HostOptions host {};                   ///< The host's.
    ComponentLimits limits {};             ///< The store's caps.
    uint64_t maxSteps = 0;                 ///< The store's step budget; 0 for none.
    Provision provision = heapProvision(); ///< Makes each store's memory allocator.
};

/// An engine, the fixture loaded on it, an ExtensionHost and an empty store: what one extension needs.
class ExtensionRun
{
  public:
    ExtensionRun(Fixture const& fixture, isoline::api::ExecutionEngine engineKind, RunOptions options = {}):
        engine { isoline::api::EngineConfig { .engine = engineKind,
                                              .store = storeLimits(options.maxSteps) } },
        loader { engine },
        validator { engine },
        host { std::move(options.host), ComponentConfig { .limits = options.limits } },
        component { load(fixture) },
        memory { std::make_unique<CountingAllocator>(options.provision()) },
        store { engine, *memory, ComponentConfig { .limits = options.limits } },
        _config { .limits = options.limits },
        _provision { std::move(options.provision) }
    {
    }

    /// Drops the store and makes a fresh one, as an embedder does after a lockdown: of the same caps, with
    /// a fresh memory provision. The old store is destroyed before its allocator.
    void restart()
    {
        auto fresh = std::make_unique<CountingAllocator>(_provision());
        auto freshStore = ComponentStore { engine, *fresh, _config };
        store = std::move(freshStore);
        memory = std::move(fresh);
    }

    /// Instantiates the extension in the run's store.
    [[nodiscard]] std::expected<ComponentInstance, InstantiateComponentError> instantiate()
    {
        return store.instantiate(component, host.linker);
    }

    /// Instantiates the extension (REQUIREd to instantiate).
    [[nodiscard]] ComponentInstance instance()
    {
        auto instantiated = instantiate();
        REQUIRE(instantiated.has_value());
        return std::move(*instantiated);
    }

    isoline::api::Engine engine;
    CountingLoader loader;
    EngineCoreModuleValidator validator;
    ExtensionHost host;
    Component component;
    std::unique_ptr<CountingAllocator> memory; ///< The store's allocator: it outlives the store.
    ComponentStore store;

  private:
    ComponentConfig _config;
    Provision _provision;

    [[nodiscard]] static isoline::api::StoreLimits storeLimits(uint64_t maxSteps)
    {
        auto limits = isoline::api::StoreLimits {};
        if (maxSteps != 0)
            limits.maxSteps = maxSteps;
        return limits;
    }

    [[nodiscard]] Component load(Fixture const& fixture)
    {
        auto const bytes = bytesOf(fixture);
        auto loaded = Component::load(engine, bytes, ComponentConfig {}, loader, validator);
        REQUIRE(loaded.has_value());
        // A Component has no move constructor: copying it shares its state.
        return *loaded;
    }
};

/// The world's exports, bound by type.
struct Exports
{
    TypedFunction<EventKinds(std::vector<Assignment>)> load;
    TypedFunction<RenderRequest(Event)> update;
    TypedFunction<std::vector<uint8_t>(uint32_t, uint32_t)> render;
    TypedFunction<PaneList(std::optional<std::string>)> panes;
    TypedFunction<Size(std::vector<uint8_t>)> paint;
};

/// @p instance's exports (REQUIREd to be of the world's types).
[[nodiscard]] Exports exportsOf(ComponentInstance& instance)
{
    auto load = TypedFunction<EventKinds(std::vector<Assignment>)>::bind(instance, "load");
    auto update = TypedFunction<RenderRequest(Event)>::bind(instance, "update");
    auto render = TypedFunction<std::vector<uint8_t>(uint32_t, uint32_t)>::bind(instance, "render");
    auto panes = TypedFunction<PaneList(std::optional<std::string>)>::bind(instance, "panes");
    auto paint = TypedFunction<Size(std::vector<uint8_t>)>::bind(instance, "paint");
    REQUIRE(load.has_value());
    REQUIRE(update.has_value());
    REQUIRE(render.has_value());
    REQUIRE(panes.has_value());
    REQUIRE(paint.has_value());
    return Exports { .load = std::move(*load),
                     .update = std::move(*update),
                     .render = std::move(*render),
                     .panes = std::move(*panes),
                     .paint = std::move(*paint) };
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

/// The trap code of a call that must have trapped (REQUIREd).
template <typename T>
[[nodiscard]] TrapCode trapOf(std::expected<T, ComponentCallError> const& outcome)
{
    auto const error = errorOf(outcome);
    REQUIRE(error.failure == ComponentCallFailure::Trap);
    auto const* const trap = std::get_if<Trap>(&error.detail);
    REQUIRE(trap != nullptr);
    return trap->code;
}

/// The HostError a call that must have failed with a host body's failure failed with (REQUIREd).
template <typename T>
[[nodiscard]] HostError hostErrorOf(std::expected<T, ComponentCallError> const& outcome)
{
    auto const error = errorOf(outcome);
    REQUIRE(error.failure == ComponentCallFailure::HostFailure);
    auto const* const failure = std::get_if<isoline::api::HostFailure>(&error.detail);
    REQUIRE(failure != nullptr);
    REQUIRE(failure->kind == isoline::api::HostFailureKind::Embedder);
    return static_cast<HostError>(failure->code);
}

// How an embedder maps Isoline's failures onto its own: Contour's extension host reports an extension's
// failure as one of a few ExtensionErrors (docs/embedding.md, "Mapping failures"). The mapping is data: a row
// per call failure, the more specific (a trap code) before the general.

/// What Contour's extension host makes of a failed extension.
enum class ExtensionError : uint8_t
{
    Trapped,          ///< The guest trapped or misbehaved: it is restarted, as its host breaker allows.
    DeadlineExceeded, ///< The watchdog interrupted it past its deadline.
    OutputTooLarge,   ///< It passed a cap on what it hands the host: maxRenderBytes, maxHostCallBytes.
    WorldMismatch,    ///< It is not of the world the host implements: it is refused, not restarted.
};

/// One row of CallFailureMappings: a call failure, the trap code it carries (none: any), and what it is.
struct CallFailureMapping
{
    ComponentCallFailure failure; ///< The failure.
    std::optional<TrapCode> trap; ///< Its trap's code; none for any detail.
    ExtensionError error;         ///< What the extension host makes of it.
};

/// Every call failure's ExtensionError, the first matching row winning.
constexpr auto CallFailureMappings = std::to_array<CallFailureMapping>({
    { .failure = ComponentCallFailure::Trap,
      .trap = TrapCode::LiveValueBytes,
      .error = ExtensionError::OutputTooLarge },
    { .failure = ComponentCallFailure::Trap,
      .trap = TrapCode::HostCallBytes,
      .error = ExtensionError::OutputTooLarge },
    { .failure = ComponentCallFailure::Trap, .trap = std::nullopt, .error = ExtensionError::Trapped },
    { .failure = ComponentCallFailure::Interrupted,
      .trap = std::nullopt,
      .error = ExtensionError::DeadlineExceeded },
    { .failure = ComponentCallFailure::HostFailure, .trap = std::nullopt, .error = ExtensionError::Trapped },
    { .failure = ComponentCallFailure::LockedDown, .trap = std::nullopt, .error = ExtensionError::Trapped },
    { .failure = ComponentCallFailure::ArgumentMismatch,
      .trap = std::nullopt,
      .error = ExtensionError::WorldMismatch },
    { .failure = ComponentCallFailure::ForeignFunction,
      .trap = std::nullopt,
      .error = ExtensionError::WorldMismatch },
    { .failure = ComponentCallFailure::StoreDeleted, .trap = std::nullopt, .error = ExtensionError::Trapped },
    { .failure = ComponentCallFailure::Instantiating,
      .trap = std::nullopt,
      .error = ExtensionError::Trapped },
    { .failure = ComponentCallFailure::ResourceExhausted,
      .trap = std::nullopt,
      .error = ExtensionError::Trapped },
});

/// What the extension host makes of a failed call: its row's ExtensionError.
[[nodiscard]] ExtensionError extensionErrorOf(ComponentCallError const& error)
{
    auto const* const trap = std::get_if<Trap>(&error.detail);
    auto const row = std::ranges::find_if(CallFailureMappings, [&](CallFailureMapping const& mapping) {
        return mapping.failure == error.failure
               && (!mapping.trap || (trap != nullptr && trap->code == *mapping.trap));
    });
    REQUIRE(row != CallFailureMappings.end());
    return row->error;
}

/// What the extension host makes of a failed instantiation: an import it does not satisfy, or one of
/// another type, is a world mismatch; any other failure, a trap.
[[nodiscard]] ExtensionError extensionErrorOf(InstantiateComponentError const& error)
{
    return std::visit(
        [](auto const& refusal) {
            using Refusal = std::remove_cvref_t<decltype(refusal)>;
            if constexpr (std::is_same_v<Refusal, InstantiationFailure>)
                return refusal.kind == InstantiationFailureKind::ImportMismatch
                           ? ExtensionError::WorldMismatch
                           : ExtensionError::Trapped;
            else
                return ExtensionError::WorldMismatch; // the engine, an import unsatisfied, mistyped or
                                                      // foreign
        },
        error);
}

/// @p text's bytes.
[[nodiscard]] std::vector<uint8_t> bytesOf(std::string_view text)
{
    return { text.begin(), text.end() };
}

/// Whether @p bytes hold @p text.
[[nodiscard]] bool contains(std::vector<uint8_t> const& bytes, std::string_view text)
{
    return std::string_view { reinterpret_cast<char const*>(bytes.data()), bytes.size() }.contains(text);
}

/// No configuration.
[[nodiscard]] std::vector<Assignment> noConfig()
{
    return {};
}

/// The configuration that sets the guest's render mode to @p mode.
[[nodiscard]] std::vector<Assignment> modeConfig(std::string mode)
{
    return { Assignment { "theme", "dark" }, Assignment { "mode", std::move(mode) } };
}

/// A state with tabs and user variables, as Contour's host gives it.
[[nodiscard]] StateSnapshot richState()
{
    return StateSnapshot {
        .title = "shell",
        .cwd = "/home",
        .foregroundProcess = "bash",
        .inputMode = InputMode::Normal,
        .tabs = { Tab { .title = "one", .isActive = false }, Tab { .title = "two", .isActive = true } },
        .userVars = { Assignment { "host", "box" }, Assignment { "git", "main" } },
        .progress = Progress { .state = ProgressState::Normal, .percent = 40 },
        .isFocused = true,
        .size = Size { .cols = 120, .rows = 1 },
        .unixTimeMs = 1'795'000'000'000,
        .utcOffsetMinutes = 60,
        .font = FontMetrics { .sizePt = 12.5F, .cellWidthPx = 9, .cellHeightPx = 18 },
    };
}

} // namespace

TEST_CASE("Extension.fixturesMatchTheirDigests")
{
    // README.md records each prebuilt guest's SHA-256, which Component::hash() is: a fixture rebuilt without
    // its row updated fails here.
    auto readme = std::ifstream { std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "component"
                                  / "extension" / "README.md" };
    REQUIRE(readme.is_open());
    auto rows = std::vector<std::string> {};
    auto line = std::string {};
    while (std::getline(readme, line))
        if (line.starts_with("| `") && line.ends_with("` |"))
            rows.push_back(line);
    auto const& fixture = GENERATE(from_range(Fixtures));
    INFO(fixture.name);
    auto run = ExtensionRun { fixture, isoline::api::ExecutionEngine::Reference };
    auto digest = std::string {};
    for (auto const byte: run.component.hash())
        digest += std::format("{:02x}", static_cast<unsigned>(byte));
    auto const row = std::format("| `{}` |", fixture.file);
    auto const found =
        std::ranges::find_if(rows, [&](std::string const& entry) { return entry.starts_with(row); });
    REQUIRE(found != rows.end());
    CHECK(found->ends_with(std::format("| `{}` |", digest)));
}

TEST_CASE("Extension.loadUpdateRender")
{
    auto const engine = GENERATE(from_range(Engines));
    auto const& fixture = GENERATE(from_range(Fixtures));
    INFO(fixture.name);
    auto run = ExtensionRun { fixture, engine };
    auto instance = run.instance();
    auto exports = exportsOf(instance);

    CHECK(valueOf(exports.load(std::vector<Assignment> { Assignment { "theme", "dark" } })) == TitleAndCwd);
    // load() read the state's title, an empty one here.
    CHECK(valueOf(exports.render(80, 1)) == bytesOf("[]"));
    CHECK(valueOf(exports.update(Event { Case<"title", std::string> { "x" } })) == RenderRequest::Render);
    auto const rendered = valueOf(exports.render(80, 1));
    CHECK(contains(rendered, "x"));
    CHECK(rendered == bytesOf("[x]"));
    // Another event changes nothing the guest shows.
    CHECK(valueOf(exports.update(Event { Case<"cwd", std::string> { "/tmp" } })) == RenderRequest::None);
    CHECK(valueOf(exports.update(Event { Case<"timer", Unit> {} })) == RenderRequest::None);
    CHECK(valueOf(exports.update(Event { Case<"user-var", Assignment> { Assignment { "k", "v" } } }))
          == RenderRequest::None);
    CHECK(valueOf(exports.render(80, 1)) == bytesOf("[x]"));
    // The title is cut to the surface's width.
    CHECK(valueOf(exports.update(Event { Case<"title", std::string> { "a long title" } }))
          == RenderRequest::Render);
    CHECK(valueOf(exports.render(8, 1)) == bytesOf("[a long]"));
    CHECK_FALSE(run.store.isLockedDown());
}

TEST_CASE("Extension.hostInterfacesBound")
{
    auto const engine = GENERATE(from_range(Engines));
    auto const& fixture = GENERATE(from_range(Fixtures));
    INFO(fixture.name);
    auto options = RunOptions { .host = HostOptions { .state = richState() } };
    auto const declared = GENERATE(false, true);
    if (declared)
        options.host.status =
            CommandOutput { .exitCode = 0, .standardOutput = bytesOf("clean"), .standardError = {} };
    auto run = ExtensionRun { fixture, engine, std::move(options) };
    auto instance = run.instance();
    auto exports = exportsOf(instance);
    auto const config = std::vector<Assignment> { Assignment { "dir", "." } };

    CHECK(valueOf(exports.load(config)) == TitleAndCwd);
    auto const& journal = run.host.journal;
    auto const expected = std::vector<std::pair<LogLevel, std::string>> {
        { LogLevel::Info, "load" },
        // state(): the active tab and every user variable reached the guest.
        { LogLevel::Debug, "active tab: two" },
        { LogLevel::Debug, "var: host=box" },
        { LogLevel::Debug, "var: git=main" },
        // run-command: the declared command's output, or the error that it is not declared.
        declared ? std::pair { LogLevel::Info, std::string { "status: clean" } }
                 : std::pair { LogLevel::Warn, std::string { "status: not-declared" } },
    };
    CHECK(journal.logs == expected);
    // The guest passed its configuration as the command's arguments.
    REQUIRE(journal.commands.size() == 1);
    CHECK(journal.commands[0].first == "status");
    CHECK(journal.commands[0].second == config);
    CHECK(journal.frames == std::vector<uint32_t> { 250 });
    // The state's title is the guest's.
    CHECK(valueOf(exports.render(80, 1)) == bytesOf("[shell]"));
}

TEST_CASE("Extension.controlShapedImport")
{
    auto const engine = GENERATE(from_range(Engines));
    auto const& fixture = GENERATE(from_range(Fixtures));
    INFO(fixture.name);
    auto run = ExtensionRun { fixture, engine };
    auto instance = run.instance();
    auto exports = exportsOf(instance);

    auto const listed = valueOf(exports.panes(std::optional<std::string> { "a" }));
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    CHECK((*listed)[0].id == "a1");
    CHECK((*listed)[0].title == std::nullopt);
    CHECK((*listed)[0].tabs
          == std::vector<TabInfo> { TabInfo { .id = "t1", .title = "one", .isActive = false },
                                    TabInfo { .id = "t2", .title = "two", .isActive = true } });
    CHECK(valueOf(exports.panes(std::optional<std::string> {})).has_value());

    // The verb's error reaches the guest, which hands it back unchanged.
    auto const refused = valueOf(exports.panes(std::optional<std::string> { "x" }));
    REQUIRE_FALSE(refused.has_value());
    auto const* const notFound = std::get_if<Case<"not-found", std::string>>(&refused.error());
    REQUIRE(notFound != nullptr);
    CHECK(notFound->value == "x");
    CHECK(run.host.journal.filters
          == std::vector<std::optional<std::string>> {
              std::string { "a" }, std::nullopt, std::string { "x" } });
    CHECK_FALSE(run.store.isLockedDown());
}

TEST_CASE("Extension.hostOwnedSurface")
{
    auto const engine = GENERATE(from_range(Engines));
    auto const& fixture = GENERATE(from_range(Fixtures));
    INFO(fixture.name);

    SECTION("open, write three times, drop: each surface destroyed once")
    {
        auto run = ExtensionRun { fixture, engine };
        auto instance = run.instance();
        auto exports = exportsOf(instance);
        auto const& journal = run.host.journal;

        CHECK(valueOf(exports.paint(bytesOf("ab"))) == Size { .cols = 80, .rows = 1 });
        // The first surface took three writes; the guest dropped it, which destroyed it. The second was
        // presented empty, and the host's present dropped it.
        CHECK(journal.presented == std::vector<std::vector<uint8_t>> { {} });
        REQUIRE(journal.destroyed.size() == 2);
        CHECK(journal.destroyed[0] != journal.destroyed[1]);
        CHECK(run.host.surfaces.size() == 0);

        // A surface the host made and forgot stays held until the store ends, then is destroyed once.
        auto const rep = run.host.surfaces.insert(SurfaceObject {});
        REQUIRE(rep.has_value());
        auto forgotten = hostOwn<Surface>(run.store, run.host.resources, *rep);
        REQUIRE(forgotten.has_value());
        CHECK(run.host.surfaces.size() == 1);
        run.restart();
        CHECK(journal.destroyed.size() == 3);
        CHECK(journal.destroyed.back() == *rep);
        CHECK(run.host.surfaces.size() == 0);
    }

    SECTION("maxHostOwns bounds a host that forgets the owns it is given")
    {
        auto const limits = ComponentLimits { .maxHostOwns = 1 };
        auto const policy = GENERATE(PresentPolicy::Drop, PresentPolicy::Forget);
        auto run = ExtensionRun {
            fixture, engine, RunOptions { .host = HostOptions { .present = policy }, .limits = limits }
        };
        auto instance = run.instance();
        auto exports = exportsOf(instance);
        CHECK(valueOf(exports.paint(bytesOf("a"))) == Size { .cols = 80, .rows = 1 });
        auto const again = exports.paint(bytesOf("b"));
        if (policy == PresentPolicy::Drop)
        {
            // A host that drops what it is given stays within one own however often the guest paints.
            CHECK(again.has_value());
            CHECK(valueOf(exports.paint(bytesOf("c"))) == Size { .cols = 80, .rows = 1 });
            CHECK(run.host.surfaces.size() == 0);
        }
        else
        {
            // The forgotten surface holds the one own: the next surface the host would hand out passes the
            // cap, so the call fails and locks the store down; the store's end destroys the forgotten one.
            CHECK(hostErrorOf(again) == HostError::OwnRefused);
            CHECK(run.host.journal.refusedOwns == std::vector<TrapCode> { TrapCode::HostOwns });
            CHECK(run.store.isLockedDown());
            CHECK(run.host.surfaces.size() == 1);
            run.restart();
            CHECK(run.host.surfaces.size() == 0);
        }
    }
}

TEST_CASE("Extension.semverHostNewer")
{
    // The guest imports its interfaces @0.1.0; a host defining them at a compatible later version
    // satisfies it (0.M.x matches on major and minor), and an incompatible one does not.
    struct Row
    {
        std::string_view version;
        bool isCompatible;
    };
    auto const row = GENERATE(Row { .version = "0.1.2", .isCompatible = true },
                              Row { .version = "0.1.0", .isCompatible = true },
                              Row { .version = "0.2.0", .isCompatible = false });
    auto const engine = GENERATE(from_range(Engines));
    auto const& fixture = GENERATE(from_range(Fixtures));
    INFO(fixture.name << " against a host @" << row.version);
    auto run = ExtensionRun { fixture,
                              engine,
                              RunOptions { .host = HostOptions { .version = std::string { row.version } } } };
    auto const resolutions = run.host.linker.resolve(run.component);
    auto const host =
        std::ranges::find(resolutions,
                          std::string_view { "isoline-test:extension/host@0.1.0" },
                          [](auto const& resolution) { return std::string_view { resolution.name }; });
    REQUIRE(host != resolutions.end());
    auto instantiated = run.instantiate();
    if (row.isCompatible)
    {
        CHECK(host->outcome
              == isoline::component::api::ImportOutcome {
                  Satisfied { .matched = std::format("isoline-test:extension/host@{}", row.version) } });
        REQUIRE(instantiated.has_value());
        auto exports = exportsOf(*instantiated);
        CHECK(valueOf(exports.load(noConfig())) == TitleAndCwd);
    }
    else
    {
        CHECK(host->outcome == isoline::component::api::ImportOutcome { Unsatisfied {} });
        REQUIRE_FALSE(instantiated.has_value());
        CHECK(std::holds_alternative<UnsatisfiedImport>(instantiated.error()));
    }
}

TEST_CASE("Extension.embedderMemory")
{
    // Contour hands each extension one memory of its own, in a fixed buffer followed by a guard
    // (ExtensionLimits::memoryBytes and the Wasmtime engine's SignalsGuardBytes, 32 MiB each), refusing
    // every further request. Each guest allocates exactly one memory (A9), so it runs there.
    auto const engine = GENERATE(from_range(Engines));
    auto const& fixture = GENERATE(from_range(Fixtures));
    INFO(fixture.name);

    SECTION("Contour's provision")
    {
        auto run = ExtensionRun { fixture, engine, RunOptions { .provision = contourProvision() } };
        // Its guard covers what the engine needs past a memory's end.
        CHECK(run.engine.requiredGuardBytes() <= ContourGuardBytes);
        auto instance = run.instance();
        CHECK(run.memory->allocated == fixture.coreMemories);
        auto exports = exportsOf(instance);
        CHECK(valueOf(exports.load(noConfig())) == TitleAndCwd);
        CHECK(valueOf(exports.update(Event { Case<"title", std::string> { "x" } })) == RenderRequest::Render);
        CHECK(valueOf(exports.render(80, 1)) == bytesOf("[x]"));
        // The provision is spent: a second instance of the extension in the store gets no memory.
        auto const second = run.instantiate();
        REQUIRE_FALSE(second.has_value());
        auto const* const failure = std::get_if<InstantiationFailure>(&second.error());
        REQUIRE(failure != nullptr);
        CHECK(failure->kind == InstantiationFailureKind::CoreInstantiation);
        auto const* const core = std::get_if<isoline::api::InstantiateError>(&failure->detail);
        REQUIRE(core != nullptr);
        CHECK(*core
              == isoline::api::InstantiateError { isoline::api::AllocationFailure {
                  .kind = isoline::api::ExternKind::Memory, .reason = isoline::api::Refusal::Embedder } });
        CHECK(run.memory->allocated == fixture.coreMemories);
        // A restart takes a fresh provision, so the extension runs again.
        run.restart();
        auto fresh = run.instance();
        CHECK(run.memory->allocated == fixture.coreMemories);
        CHECK(valueOf(exportsOf(fresh).load(noConfig())) == TitleAndCwd);
    }

    SECTION("a provision smaller than the guest's memory")
    {
        auto run = ExtensionRun {
            fixture, engine, RunOptions { .provision = contourProvision(fixture.initialMemoryBytes - 1) }
        };
        auto const refused = run.instantiate();
        REQUIRE_FALSE(refused.has_value());
        auto const* const failure = std::get_if<InstantiationFailure>(&refused.error());
        REQUIRE(failure != nullptr);
        CHECK(failure->kind == InstantiationFailureKind::CoreInstantiation);
        CHECK(
            failure->detail
            == decltype(failure->detail) { isoline::api::InstantiateError { isoline::api::AllocationFailure {
                .kind = isoline::api::ExternKind::Memory, .reason = isoline::api::Refusal::Embedder } } });
        CHECK(run.store.isLockedDown());
        CHECK(extensionErrorOf(refused.error()) == ExtensionError::Trapped);
    }
}

namespace
{

/// How long a test waits for a guest to start spinning before it interrupts it anyway: a guard against a
/// hang, never the latency measured.
constexpr auto HangGuard = std::chrono::seconds { 30 };

/// The steps a store may take: a backstop, so that a broken interrupt fails the test (StepLimitExceeded)
/// instead of hanging it.
constexpr auto StepBackstop = uint64_t { 2'000'000'000 };

/// A run whose guest spins in the call under test, and a watchdog thread that interrupts it once the guest
/// logged `spinning`, as Contour's watchdog interrupts an extension past its deadline.
class SpinningRun
{
  public:
    SpinningRun(Fixture const& fixture, isoline::api::ExecutionEngine engine):
        run { fixture, engine, optionsFor(*this) }
    {
    }

    /// Runs @p call while the watchdog interrupts it, and reports how it ended.
    /// @return The call's outcome.
    template <typename Call>
    [[nodiscard]] auto interrupting(Call call)
    {
        auto const interrupter = run.store.interrupter();
        auto watchdog = std::thread { [&] {
            auto lock = std::unique_lock { _mutex };
            sawSpinning = _entered.wait_for(lock, HangGuard, [&] { return _isSpinning; });
            interruptedAt = std::chrono::steady_clock::now();
            interrupter->interrupt();
        } };
        auto outcome = call();
        returnedAt = std::chrono::steady_clock::now();
        watchdog.join();
        return outcome;
    }

    ExtensionRun run;
    bool sawSpinning = false; ///< Whether the guest logged `spinning` before the watchdog interrupted it.
    std::chrono::steady_clock::time_point interruptedAt {}; ///< When the watchdog interrupted.
    std::chrono::steady_clock::time_point returnedAt {};    ///< When the call returned.

  private:
    /// The run's options: its host's log callback reaches the members declared after the run, which it
    /// calls only once a guest calls host.log.
    [[nodiscard]] static RunOptions optionsFor(SpinningRun& self)
    {
        auto options = RunOptions { .maxSteps = StepBackstop };
        options.host.onLog = [&self](std::string_view message) {
            if (message != "spinning")
                return;
            auto const lock = std::lock_guard { self._mutex };
            self._isSpinning = true;
            self._entered.notify_all();
        };
        return options;
    }

    std::mutex _mutex {};
    std::condition_variable _entered {};
    bool _isSpinning = false;
};

/// Checks @p spinning's interrupted call ended Interrupted, the store locked down, and a restarted store
/// runs the extension afresh.
template <typename T>
void checkInterrupted(SpinningRun& spinning, std::expected<T, ComponentCallError> const& outcome)
{
    CHECK(spinning.sawSpinning);
    CHECK(errorOf(outcome).failure == ComponentCallFailure::Interrupted);
    CHECK(extensionErrorOf(errorOf(outcome)) == ExtensionError::DeadlineExceeded);
    CHECK(spinning.returnedAt - spinning.interruptedAt < HangGuard);
    CHECK(spinning.run.store.isLockedDown());
    // The embedder drops the locked-down store and instantiates afresh.
    spinning.run.restart();
    auto fresh = spinning.run.instance();
    auto exports = exportsOf(fresh);
    CHECK(valueOf(exports.load(noConfig())) == TitleAndCwd);
    CHECK(valueOf(exports.render(80, 1)) == bytesOf("[]"));
}

} // namespace

TEST_CASE("Extension.interruptDuringRender", "[threading]")
{
    auto const engine = GENERATE(from_range(Engines));
    auto const& fixture = GENERATE(from_range(Fixtures));
    INFO(fixture.name);
    auto spinning = SpinningRun { fixture, engine };
    auto instance = spinning.run.instance();
    auto exports = exportsOf(instance);
    CHECK(valueOf(exports.load(modeConfig("spin"))) == TitleAndCwd);
    auto const rendered = spinning.interrupting([&] { return exports.render(80, 1); });
    CHECK(errorOf(exports.render(80, 1)).failure == ComponentCallFailure::LockedDown);
    checkInterrupted(spinning, rendered);
}

TEST_CASE("Extension.interruptDuringLoad", "[threading]")
{
    auto const engine = GENERATE(from_range(Engines));
    auto const& fixture = GENERATE(from_range(Fixtures));
    INFO(fixture.name);
    auto spinning = SpinningRun { fixture, engine };
    auto instance = spinning.run.instance();
    auto exports = exportsOf(instance);
    auto const loaded = spinning.interrupting([&] { return exports.load(modeConfig("spin-load")); });
    CHECK(errorOf(exports.render(80, 1)).failure == ComponentCallFailure::LockedDown);
    checkInterrupted(spinning, loaded);
}

TEST_CASE("Extension.coreModulesThroughInjectedLoader")
{
    // Every embedded core module is loaded once through the injected loader, after validation, keyed by the
    // component's hash and the module's ordinal: what P8's cache serves compiled modules by.
    auto const engine = GENERATE(from_range(Engines));
    auto const& fixture = GENERATE(from_range(Fixtures));
    INFO(fixture.name);
    auto run = ExtensionRun { fixture, engine };
    auto const& keys = run.loader.keys;
    REQUIRE(keys.size() == fixture.coreModules);
    CHECK(run.component.coreModules().size() == fixture.coreModules);
    auto ordinal = uint32_t { 0 };
    for (auto const& key: keys)
    {
        CHECK(key.componentHash == run.component.hash());
        CHECK(key.ordinal == ordinal);
        ++ordinal;
    }
    // The modules it loaded are the ones that run.
    auto instance = run.instance();
    CHECK(valueOf(exportsOf(instance).load(noConfig())) == TitleAndCwd);
    CHECK(keys.size() == fixture.coreModules);
}

TEST_CASE("Extension.hostileGuestIsContained", "[memory]")
{
    // Each way an extension misbehaves ends its call, locks its store down, and holds the host to its caps;
    // the [memory] cases run under the test's memory cap (isoline_component_test_memory).
    auto const engine = GENERATE(from_range(Engines));
    auto const& fixture = GENERATE(from_range(Fixtures));
    INFO(fixture.name);
    auto const lockedDown = [](ExtensionRun& run, auto& exports) {
        CHECK(run.store.isLockedDown());
        CHECK(errorOf(exports.render(80, 1)).failure == ComponentCallFailure::LockedDown);
    };

    SECTION("a lying list length")
    {
        // render() returns a list<u8> of 2^28 - 1 bytes its memory does not hold: the lift refuses it before
        // the host allocates a byte of it, whatever the store's caps.
        auto const limits =
            GENERATE(ComponentLimits {}, ComponentLimits { .maxLiveValueBytes = uint64_t { 1 } << 20 });
        auto run = ExtensionRun { fixture, engine, RunOptions { .limits = limits } };
        auto instance = run.instance();
        auto exports = exportsOf(instance);
        CHECK(valueOf(exports.load(modeConfig("hostile"))) == TitleAndCwd);
        auto const rendered = exports.render(80, 1);
        CHECK(trapOf(rendered) == TrapCode::OutOfBounds);
        CHECK(extensionErrorOf(errorOf(rendered)) == ExtensionError::Trapped);
        lockedDown(run, exports);
    }

    SECTION("an in-bounds value over the store's cap")
    {
        // 4 MiB its memory holds, inside Contour's 32 MiB provision, against Contour's maxRenderBytes of 1
        // MiB (the store's maxLiveValueBytes): refused as it is charged, before the host holds it.
        auto run = ExtensionRun { fixture,
                                  engine,
                                  RunOptions {
                                      .limits = ComponentLimits { .maxLiveValueBytes = uint64_t { 1 } << 20 },
                                      .provision = contourProvision() } };
        auto instance = run.instance();
        auto exports = exportsOf(instance);
        CHECK(valueOf(exports.load(modeConfig("large"))) == TitleAndCwd);
        auto const rendered = exports.render(80, 1);
        CHECK(trapOf(rendered) == TrapCode::LiveValueBytes);
        CHECK(extensionErrorOf(errorOf(rendered)) == ExtensionError::OutputTooLarge);
        lockedDown(run, exports);
    }

    SECTION("a guest trap")
    {
        auto run = ExtensionRun { fixture, engine };
        auto instance = run.instance();
        auto exports = exportsOf(instance);
        CHECK(valueOf(exports.load(modeConfig("trap"))) == TitleAndCwd);
        auto const rendered = exports.render(80, 1);
        CHECK(trapOf(rendered) == TrapCode::Core);
        auto const error = errorOf(rendered);
        auto const* const trap = std::get_if<Trap>(&error.detail);
        REQUIRE(trap != nullptr);
        CHECK(trap->core == isoline::api::TrapCode::Unreachable);
        CHECK(extensionErrorOf(errorOf(rendered)) == ExtensionError::Trapped);
        lockedDown(run, exports);
    }

    SECTION("a guest that keeps every surface")
    {
        // The host's surfaces table holds 64: the guest keeps one per paint, so the 65th paint finds the
        // table full, and the store's end destroys the 64 the guest kept.
        auto run = ExtensionRun { fixture, engine };
        auto instance = run.instance();
        auto exports = exportsOf(instance);
        CHECK(valueOf(exports.load(modeConfig("hoard"))) == TitleAndCwd);
        for ([[maybe_unused]] auto const each: std::views::iota(0, 64))
            REQUIRE(valueOf(exports.paint(bytesOf("a"))) == Size { .cols = 80, .rows = 1 });
        CHECK(run.host.surfaces.size() == 64);
        auto const full = exports.paint(bytesOf("a"));
        CHECK(hostErrorOf(full) == HostError::SurfaceTableFull);
        lockedDown(run, exports);
        CHECK(run.host.journal.destroyed.empty());
        run.restart();
        CHECK(run.host.journal.destroyed.size() == 64);
        CHECK(run.host.surfaces.size() == 0);
    }

    SECTION("a cabi_realloc handing out memory it does not have")
    {
        // load() makes its allocator misbehave before it reads host.state, whose strings are lowered into
        // the block it hands out: past the memory's end, refused before a byte is written.
        auto run = ExtensionRun { fixture, engine };
        auto instance = run.instance();
        auto exports = exportsOf(instance);
        auto const loaded = exports.load(modeConfig("bad-realloc"));
        CHECK(trapOf(loaded) == TrapCode::OutOfBounds);
        lockedDown(run, exports);
    }
}

TEST_CASE("Extension.hostCallBytesCapped")
{
    // Contour caps what an extension passes to one host call (maxHostCallBytes, 64 KiB): surface.write's
    // bytes within it run, and past it trap HostCallBytes before the host function runs.
    constexpr auto ContourHostCallBytes = uint64_t { 64 } << 10;
    auto const engine = GENERATE(from_range(Engines));
    auto const& fixture = GENERATE(from_range(Fixtures));
    INFO(fixture.name);
    auto run = ExtensionRun { fixture,
                              engine,
                              RunOptions { .host = HostOptions { .maxCallBytes = ContourHostCallBytes } } };
    auto instance = run.instance();
    auto exports = exportsOf(instance);
    CHECK(valueOf(exports.load(noConfig())) == TitleAndCwd);
    CHECK(valueOf(exports.paint(std::vector<uint8_t>(32 << 10, 'a'))) == Size { .cols = 80, .rows = 1 });
    auto const painted = exports.paint(std::vector<uint8_t>(80 << 10, 'a'));
    REQUIRE_FALSE(painted.has_value());
    auto const* const trap = std::get_if<Trap>(&painted.error().detail);
    REQUIRE(trap != nullptr);
    CHECK(trap->code == TrapCode::HostCallBytes);
    CHECK(trap->message() == "host call arguments exceed the limit of 65536 bytes");
    CHECK(extensionErrorOf(painted.error()) == ExtensionError::OutputTooLarge);
    CHECK(run.store.isLockedDown());
    // The surface the refused write was for was never written to.
    CHECK(run.host.surfaces.size() == 1);
}

TEST_CASE("Extension.hostCallWithinTheStoresCap")
{
    // A host call's arguments count against the store's cap as well as their own: 50 KiB, within the
    // call's 64 KiB, passes a store of 100 KiB that holds paint's own 50 KiB argument, and traps
    // LiveValueBytes at the store's limit, not HostCallBytes.
    auto const engine = GENERATE(from_range(Engines));
    auto const& fixture = GENERATE(from_range(Fixtures));
    INFO(fixture.name);
    auto run = ExtensionRun { fixture,
                              engine,
                              RunOptions { .host = HostOptions { .maxCallBytes = uint64_t { 64 } << 10 },
                                           .limits = ComponentLimits { .maxLiveValueBytes =
                                                                           uint64_t { 100 } << 10 } } };
    auto instance = run.instance();
    auto exports = exportsOf(instance);
    CHECK(valueOf(exports.load(noConfig())) == TitleAndCwd);
    auto const painted = exports.paint(std::vector<uint8_t>(50 << 10, 'a'));
    CHECK(trapOf(painted) == TrapCode::LiveValueBytes);
    auto const* const trap = std::get_if<Trap>(&painted.error().detail);
    REQUIRE(trap != nullptr);
    CHECK(trap->message() == "live value bytes exceed the limit of 102400");
    CHECK(extensionErrorOf(painted.error()) == ExtensionError::OutputTooLarge);
}

TEST_CASE("Extension.errorMapping")
{
    // Every call failure maps to one ExtensionError by the table's first matching row.
    auto const trap = [](TrapCode code) {
        return isoline::component::api::callError(ComponentCallFailure::Trap, Trap { .code = code });
    };
    CHECK(extensionErrorOf(trap(TrapCode::LiveValueBytes)) == ExtensionError::OutputTooLarge);
    CHECK(extensionErrorOf(trap(TrapCode::HostCallBytes)) == ExtensionError::OutputTooLarge);
    CHECK(extensionErrorOf(trap(TrapCode::OutOfBounds)) == ExtensionError::Trapped);
    CHECK(extensionErrorOf(isoline::component::api::callError(ComponentCallFailure::Interrupted,
                                                              Trap { .code = TrapCode::Interrupted }))
          == ExtensionError::DeadlineExceeded);
    for (auto const& row: isoline::component::api::ComponentCallFailureTable)
    {
        INFO(row.name);
        auto const mapped = std::ranges::find_if(CallFailureMappings, [&](CallFailureMapping const& mapping) {
            return mapping.failure == row.failure;
        });
        CHECK(mapped != CallFailureMappings.end());
    }
    CHECK(extensionErrorOf(InstantiateComponentError { UnsatisfiedImport { .name = "x" } })
          == ExtensionError::WorldMismatch);
    CHECK(extensionErrorOf(InstantiateComponentError { isoline::component::api::ImportTypeMismatch {} })
          == ExtensionError::WorldMismatch);
    CHECK(extensionErrorOf(InstantiateComponentError { isoline::component::api::ForeignInstance {} })
          == ExtensionError::WorldMismatch);
    CHECK(extensionErrorOf(InstantiateComponentError { isoline::api::EngineMismatch {} })
          == ExtensionError::WorldMismatch);
    CHECK(extensionErrorOf(InstantiateComponentError {
              InstantiationFailure { .kind = InstantiationFailureKind::ImportMismatch, .detail = {} } })
          == ExtensionError::WorldMismatch);
    CHECK(extensionErrorOf(InstantiateComponentError {
              InstantiationFailure { .kind = InstantiationFailureKind::Trap, .detail = {} } })
          == ExtensionError::Trapped);
}

TEST_CASE("Extension.textMatchesItsBinary")
{
    // Isoline's own component text reader makes extension.wasm of extension.wat byte for byte, as
    // `wasm-tools parse` did: the committed binary is its source's, without the tool.
    auto source = std::ifstream { std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "component"
                                  / "extension" / "extension.wat" };
    REQUIRE(source.is_open());
    auto const text =
        std::string { std::istreambuf_iterator<char> { source }, std::istreambuf_iterator<char> {} };
    auto const compiled =
        isoline::text::compileText(text, isoline::text::TextLimits {}, isoline::component::text::Forms);
    REQUIRE(compiled.has_value());
    CHECK(*compiled == bytesOf(Fixtures[0]));
}
