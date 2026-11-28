// SPDX-License-Identifier: Apache-2.0
//
// isoline_spec_runner <dir> <skips> --features <set> [--engine <e>] [--suite <s>] [--component-features <f>]:
// runs one imported .wast file of a spec suite, <dir>/<name>.wast with its oracle <dir>/<name>.oracle,
// against the text reader, the decoder, the validator and the reference interpreter, with the features of
// <set> on (wasm2, wasm3 or all). A component, told apart by its preamble (layerOf()), is loaded through the
// component api on engine <e>, each in a store of its own, and linked against the component spectest; a
// component suite (<s> = component) asserts its messages by substring and words core traps as Wasmtime
// does (D12). It fails when a command fails, when a module given as text does not encode as the oracle's
// binary does, and when the script skips other than <skips> commands: every skip is pinned in
// tests/spec/files.txt, so none appears unnoticed.

#include <core/Sha256.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "ComponentSpectest.hpp"
#include "ComponentValues.hpp"
#include "CoreTrapWording.hpp"
#include "Oracle.hpp"
#include "Script.hpp"
#include "SpecCheck.hpp"
#include "SpecFeatures.hpp"
#include "WastReader.hpp"
#include <isoline/Features.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/EngineIdentity.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/api/CallError.hpp>
#include <isoline/component/api/Component.hpp>
#include <isoline/component/api/ComponentStore.hpp>
#include <isoline/component/api/Linker.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/binary/Encoder.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/ComponentStore.hpp>
#include <isoline/component/text/Forms.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/Validator.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>
#include <isoline/validate/ValidatedModule.hpp>
#include <isoline/validate/ValidationError.hpp>

namespace isoline::spectest
{

namespace
{
    using binary::ExternKind;
    using runtime::ExternValue;
    using runtime::Value;

    [[nodiscard]] Failure harnessFailure(std::string message)
    {
        return Failure { .stage = Stage::Harness, .message = std::move(message) };
    }

    [[nodiscard]] Failure trapFailure(Stage stage, runtime::Trap const& trap)
    {
        return Failure { .stage = stage, .message = runtime::specMessage(trap) };
    }

    /// The failure of an exception no handler caught: always Stage::Exception, which only
    /// assert_exception expects, with the reference interpreter's message.
    [[nodiscard]] Failure thrownFailure(runtime::Thrown const& /*thrown*/)
    {
        return Failure { .stage = Stage::Exception, .message = "uncaught exception" };
    }

    /// The failure of a host function: the spectest module's never fail, so it is the harness's.
    [[nodiscard]] Failure hostFailure(runtime::HostFailure const& failure)
    {
        return harnessFailure(
            std::format("host failure {} (code {})", static_cast<int>(failure.kind), failure.code));
    }

    /// The failure of an invocation that ended abruptly: a trap at @p stage, an uncaught exception, or
    /// a host failure.
    [[nodiscard]] Failure abruptFailure(Stage stage, runtime::Abrupt const& abrupt)
    {
        if (auto const* thrown = std::get_if<runtime::Thrown>(&abrupt))
            return thrownFailure(*thrown);
        if (auto const* failure = std::get_if<runtime::HostFailure>(&abrupt))
            return hostFailure(*failure);
        return trapFailure(stage, std::get<runtime::Trap>(abrupt));
    }

    [[nodiscard]] std::expected<std::vector<std::byte>, Failure> readFile(std::filesystem::path const& path)
    {
        auto stream = std::ifstream { path, std::ios::binary };
        if (!stream)
            return std::unexpected { harnessFailure(std::format("cannot open {}", path.string())) };
        auto bytes = std::vector<std::byte> {};
        std::ranges::transform(std::istreambuf_iterator<char> { stream },
                               std::istreambuf_iterator<char> {},
                               std::back_inserter(bytes),
                               [](char byte) { return static_cast<std::byte>(byte); });
        return bytes;
    }

    /// The stage a text error is reported at, by text::TextErrorStage: a malformed custom annotation is
    /// what assert_malformed_custom expects, every other malformed text what assert_malformed does.
    constexpr auto TextErrorStages = std::to_array<Stage>({
        Stage::Decode,          // text::TextErrorStage::Syntax
        Stage::MalformedCustom, // text::TextErrorStage::Custom
    });
    static_assert(TextErrorStages.size() == std::to_underlying(text::TextErrorStage::Custom) + 1U
                      && TextErrorStages[std::to_underlying(text::TextErrorStage::Syntax)] == Stage::Decode
                      && TextErrorStages[std::to_underlying(text::TextErrorStage::Custom)]
                             == Stage::MalformedCustom,
                  "TextErrorStages holds one row per TextErrorStage, at the index of its enumerator");

    /// The failure of a module's text: its spec message, then where it is. A Text module's text is the
    /// script's own, so the position is shifted to the script's; a Quote module's is that of its
    /// concatenated strings, which have no place in the script.
    [[nodiscard]] Failure textFailure(ModuleSource const& source, text::TextError const& error)
    {
        auto const stage = TextErrorStages.at(static_cast<std::size_t>(text::stageOf(error.code)));
        auto const& at = error.position;
        auto const where =
            source.form == ModuleForm::Text || source.form == ModuleForm::ComponentText
                ? std::format("{}:{}",
                              source.position.line + at.line - 1,
                              at.line == 1 ? source.position.column + at.column - 1 : at.column)
                : std::format("{}:{} of its quoted text", at.line, at.column);
        return Failure { .stage = stage,
                         .message = std::format("{} at {}", text::specMessage(error), where) };
    }

    /// Compiles a command's module or component given as text: a `(component quote …)`'s strings as a
    /// component's fields, as wasm-tools reads them.
    /// @param source The module or component, of a text form (isTextForm()).
    /// @return Its bytes, or the text's first error.
    [[nodiscard]] std::expected<std::vector<std::byte>, text::TextError> compileSource(
        ModuleSource const& source)
    {
        if (source.form == ModuleForm::ComponentQuote)
            return text::compileText(
                std::format("(component {})", source.text), text::TextLimits {}, component::text::Forms);
        if (source.form == ModuleForm::ComponentText)
            return text::compileText(source.text, text::TextLimits {}, component::text::Forms);
        return text::compileText(source.text, text::TextLimits {}, text::CoreForms);
    }

    /// The bytes of a command's module or component: a binary's strings, or the text compiled.
    /// @param source The module.
    /// @return The bytes, or the failure of malformed text.
    [[nodiscard]] std::expected<std::vector<std::byte>, Failure> moduleBytesOf(ModuleSource const& source)
    {
        if (!isTextForm(source.form))
        {
            auto const bytes = std::as_bytes(std::span { source.text });
            return std::vector<std::byte> { bytes.begin(), bytes.end() };
        }
        return compileSource(source).transform_error(
            [&](text::TextError const& error) { return textFailure(source, error); });
    }

    /// The `spectest` module every script may import from, as the spec interpreter defines it.
    [[nodiscard]] runtime::ModuleInstance makeSpectest(runtime::Store& store)
    {
        using namespace binary;
        struct HostFunction
        {
            std::string_view name;
            std::vector<ValueType> params;
        };
        auto instance = runtime::ModuleInstance {};
        instance.module = std::make_shared<Module const>();
        auto const ignore = runtime::makeHostFunction(
            [](runtime::HostContext&,
               std::span<Value const>,
               std::span<Value>) noexcept -> std::expected<void, runtime::Abrupt> { return {}; });
        for (auto const& function: std::to_array<HostFunction>({
                 { .name = "print", .params = {} },
                 { .name = "print_i32", .params = { I32 } },
                 { .name = "print_i64", .params = { I64 } },
                 { .name = "print_f32", .params = { F32 } },
                 { .name = "print_f64", .params = { F64 } },
                 { .name = "print_i32_f32", .params = { I32, F32 } },
                 { .name = "print_f64_f64", .params = { F64, F64 } },
             }))
        {
            auto const address =
                store.addHostFunction(FuncType { .params = function.params, .results = {} }, ignore).value();
            instance.exports.emplace_back(std::string { function.name },
                                          ExternValue { .kind = ExternKind::Func, .address = address });
        }

        struct HostGlobal
        {
            std::string_view name;
            ValueType type;
            Value value;
        };
        for (auto const& global: std::to_array<HostGlobal>({
                 { .name = "global_i32", .type = I32, .value = runtime::I32 { 666 } },
                 { .name = "global_i64", .type = I64, .value = runtime::I64 { 666 } },
                 { .name = "global_f32",
                   .type = F32,
                   .value = runtime::F32 { std::bit_cast<uint32_t>(666.6F) } },
                 { .name = "global_f64",
                   .type = F64,
                   .value = runtime::F64 { std::bit_cast<uint64_t>(666.6) } },
             }))
        {
            auto const address = store.addGlobal(
                GlobalType { .type = global.type, .mutability = Mutability::Const }, global.value);
            instance.exports.emplace_back(std::string { global.name },
                                          ExternValue { .kind = ExternKind::Global, .address = address });
        }

        struct HostTable
        {
            std::string_view name;
            AddressType addressType;
        };
        for (auto const& table: std::to_array<HostTable>({
                 { .name = "table", .addressType = AddressType::I32 },
                 { .name = "table64", .addressType = AddressType::I64 },
             }))
        {
            auto const address = store.addTable(
                TableType { .element = FuncRef,
                            .limits = { .min = 10, .max = 20, .addressType = table.addressType } },
                runtime::Ref { .kind = runtime::RefKind::Null, .heap = FuncRef.heap, .address = 0 });
            if (!address)
                throw std::runtime_error { "cannot allocate a spectest table" };
            instance.exports.emplace_back(std::string { table.name },
                                          ExternValue { .kind = ExternKind::Table, .address = *address });
        }
        auto const memory =
            store.addMemory(MemoryType { .limits = { .min = 1, .max = 2, .addressType = AddressType::I32 } });
        if (!memory)
            throw std::runtime_error { "cannot allocate the spectest memory" };
        instance.exports.emplace_back("memory",
                                      ExternValue { .kind = ExternKind::Memory, .address = *memory });
        return instance;
    }

    /// The result of one command.
    enum class Verdict : uint8_t
    {
        Passed,
        Failed,
        Skipped,
    };

    /// How a suite's assertions word a core trap that reached a component call (canonical::Trap of code
    /// Core).
    enum class CoreTrapText : uint8_t
    {
        Isoline,  ///< As the component layer reports it: the core's spec message.
        Wasmtime, ///< As Wasmtime words it (CoreTrapWording), which the component suites assert.
    };

    /// Which reasons a suite's oracles may skip a command, or record a parity divergence, for.
    enum class SkipPolicy : uint8_t
    {
        AnyReason,   ///< Any: the core suites' importer words its own. Such a file may hold no component.
        SkipReasons, ///< Only a SkipReasons row's of its use (D12); any other reason fails the file.
    };

    /// One suite a file can come from: how `--suite` names it, and the rules its files run by.
    struct SuiteRow
    {
        std::string_view name;  ///< What `--suite` spells it as.
        MessageRule rule;       ///< How an assertion's expected message must occur in the actual one.
        CoreTrapText coreTraps; ///< How a core trap a component call met is worded.
        SkipPolicy skips;       ///< Which skip reasons its oracles may give.
    };

    /// Every suite. The core suites hold to the reference interpreter's rules, the component suites to
    /// Wasmtime's (D12). Adding a suite is adding a row.
    constexpr auto Suites = std::to_array<SuiteRow>({
        { .name = "core",
          .rule = MessageRule::Prefix,
          .coreTraps = CoreTrapText::Isoline,
          .skips = SkipPolicy::AnyReason },
        { .name = "component",
          .rule = MessageRule::Substring,
          .coreTraps = CoreTrapText::Wasmtime,
          .skips = SkipPolicy::SkipReasons },
    });

    /// One engine `--engine` can name.
    struct EngineRow
    {
        std::string_view name;                ///< What `--engine` spells it as.
        isoline::api::ExecutionEngine engine; ///< The engine.
    };

    /// Every engine the runner runs components on. Adding an engine is adding a row.
    constexpr auto Engines = std::to_array<EngineRow>({
        { .name = "reference", .engine = isoline::api::ExecutionEngine::Reference },
    });

    /// What a file runs with: its features, its engine and its suite's rules.
    struct RunOptions
    {
        FeatureSet features;                                           ///< The core features.
        isoline::api::ExecutionEngine engine = Engines.front().engine; ///< What runs components.
        SuiteRow suite = Suites.front();                               ///< The suite's rules.
        /// The gated component features: wasm-tools' defaults, which enable implements, unless
        /// `--component-features` names others (the importer maps them per file).
        component::ComponentFeatureSet componentFeatures =
            component::ComponentFeatureSet::defaults().with(component::ComponentFeature::Implements);
    };

    /// One row of CallFailureStages: the stage a component call's failure is reported at, and the
    /// message it is reported with, empty for the failure's own.
    struct CallFailureRow
    {
        component::api::ComponentCallFailure failure; ///< The failure; equal to the row's index.
        Stage stage;                                  ///< Where it is reported.
        std::string_view text;                        ///< Its message; empty for the failure's own.
    };

    /// How each failure of a component call is reported, in ComponentCallFailure's order. A store a trap
    /// locked down refuses every later call, which Wasmtime reports as the trap of entering it.
    constexpr auto CallFailureStages = std::to_array<CallFailureRow>({
        { .failure = component::api::ComponentCallFailure::Trap, .stage = Stage::Execute, .text = "" },
        { .failure = component::api::ComponentCallFailure::Interrupted, .stage = Stage::Execute, .text = "" },
        { .failure = component::api::ComponentCallFailure::HostFailure, .stage = Stage::Execute, .text = "" },
        { .failure = component::api::ComponentCallFailure::LockedDown,
          .stage = Stage::Execute,
          .text = component::canonical::trapCodeRow(component::canonical::TrapCode::CannotEnter).format },
        { .failure = component::api::ComponentCallFailure::ArgumentMismatch,
          .stage = Stage::Harness,
          .text = "" },
        { .failure = component::api::ComponentCallFailure::ForeignFunction,
          .stage = Stage::Harness,
          .text = "" },
        { .failure = component::api::ComponentCallFailure::StoreDeleted,
          .stage = Stage::Harness,
          .text = "" },
        { .failure = component::api::ComponentCallFailure::Instantiating,
          .stage = Stage::Harness,
          .text = "" },
        { .failure = component::api::ComponentCallFailure::ResourceExhausted,
          .stage = Stage::Execute,
          .text = "" },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: CallFailureStages)
                if (static_cast<std::size_t>(row.failure) != index++)
                    return false;
            return index == static_cast<std::size_t>(component::api::LastComponentCallFailure) + 1;
        }(),
        "CallFailureStages holds one row per ComponentCallFailure, at the index of its enumerator");

    /// One row of InstantiationStages: the stage a failed component instantiation is reported at.
    struct InstantiationFailureRow
    {
        component::runtime::InstantiationFailureKind kind; ///< The failure; equal to the row's index.
        Stage stage;                                       ///< Where it is reported.
        std::string_view text;                             ///< Its message, before its detail.
    };

    /// How each failure of a component instantiation is reported, in InstantiationFailureKind's order.
    constexpr auto InstantiationStages = std::to_array<InstantiationFailureRow>({
        { .kind = component::runtime::InstantiationFailureKind::Reentrant,
          .stage = Stage::Harness,
          .text = "instantiation re-entered its store" },
        { .kind = component::runtime::InstantiationFailureKind::LockedDown,
          .stage = Stage::Harness,
          .text = "instantiation in a locked-down store" },
        { .kind = component::runtime::InstantiationFailureKind::EngineMismatch,
          .stage = Stage::Harness,
          .text = "component of another engine" },
        { .kind = component::runtime::InstantiationFailureKind::ImportMismatch,
          .stage = Stage::Link,
          .text = "import has the wrong type" },
        { .kind = component::runtime::InstantiationFailureKind::LimitExceeded,
          .stage = Stage::Instantiate,
          .text = "component limit exceeded" },
        { .kind = component::runtime::InstantiationFailureKind::CoreInstantiation,
          .stage = Stage::Instantiate,
          .text = "core instantiation failed" },
        { .kind = component::runtime::InstantiationFailureKind::CoreDefinition,
          .stage = Stage::Instantiate,
          .text = "core definition failed" },
        { .kind = component::runtime::InstantiationFailureKind::Trap,
          .stage = Stage::Instantiate,
          .text = "" },
        { .kind = component::runtime::InstantiationFailureKind::HostFailure,
          .stage = Stage::Instantiate,
          .text = "host function failed" },
        { .kind = component::runtime::InstantiationFailureKind::HostMemoryExhausted,
          .stage = Stage::Harness,
          .text = "host out of memory" },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: InstantiationStages)
                if (static_cast<std::size_t>(row.kind) != index++)
                    return false;
            return index
                   == static_cast<std::size_t>(
                          component::runtime::InstantiationFailureKind::HostMemoryExhausted)
                          + 1;
        }(),
        "InstantiationStages holds one row per InstantiationFailureKind, at the index of its enumerator");

    /// A component instance and the store it alone lives in: each component command gets a fresh store,
    /// as Wasmtime's runner gives it (D12).
    struct ComponentRun
    {
        std::unique_ptr<component::api::ComponentStore> store; ///< Its store.
        component::api::ComponentInstance instance;            ///< The instance.
    };

    /// What a command acts on: a core module instance of the runner's store, or a component instance.
    using Target = std::variant<uint32_t, std::shared_ptr<ComponentRun>>;

    /// What a definition names: a validated core module, or a loaded component.
    using Definition =
        std::variant<std::shared_ptr<validate::ValidatedModule const>, component::api::Component>;

    /// A component function's results, with the function that returned them.
    struct ComponentResults
    {
        component::api::ComponentFunction function; ///< The function called.
        component::canonical::Values values;        ///< Its results, in its arena.
    };

    /// Runs the commands of one script against one store, as the spec interpreter runs a .wast file; a
    /// component, as Wasmtime's runner does, against a store of its own.
    class Runner
    {
      public:
        explicit Runner(RunOptions const& options):
            _options { options },
            _store { runtime::StoreLimits {}, runtime::StoreServices { .allocator = _allocator } },
            _engine { isoline::api::EngineConfig { .features = options.features, .engine = options.engine } },
            _loader { _engine },
            _validator { _engine },
            _componentConfig { .features = options.componentFeatures, .limits = {} },
            _linker { linkSpectest(_engine, _componentConfig) }
        {
            _registered.emplace("spectest", _store.addModuleInstance(makeSpectest(_store)));
        }

        /// Runs @p command; prints why it failed, prefixed with @p location, when it does.
        Verdict run(Command const& command, std::string_view location)
        {
            if (command.kind == CommandKind::Skip)
                return Verdict::Skipped;
            auto const outcome = execute(command);
            if (outcome)
                return Verdict::Passed;
            std::cout << std::format("{}:{}: {}\n", location, command.line, outcome.error());
            return Verdict::Failed;
        }

      private:
        using Outcome = std::expected<void, std::string>;

        /// The component spectest, or an exception: a runner that cannot define it runs nothing.
        [[nodiscard]] static component::api::Linker linkSpectest(
            isoline::api::Engine const& engine, component::api::ComponentConfig const& config)
        {
            auto linker = makeComponentSpectest(engine, config);
            if (!linker)
                throw std::runtime_error { linker.error() };
            return std::move(*linker);
        }

        [[nodiscard]] Outcome execute(Command const& command)
        {
            switch (command.kind)
            {
                case CommandKind::Module: return defineModule(command);
                case CommandKind::ModuleDefinition: return defineDefinition(command);
                case CommandKind::ModuleInstance: return instantiateDefinition(command);
                case CommandKind::Register: return registerModule(command);
                case CommandKind::Invoke: return performAny(command).transform_error(&describeFailure);
                case CommandKind::AssertReturn: return assertReturn(command);
                case CommandKind::AssertTrap:
                case CommandKind::AssertExhaustion:
                    return expectFailure(command, performAny(command), Stage::Execute);
                case CommandKind::AssertTrapModule:
                case CommandKind::AssertUninstantiable:
                    return expectFailure(command, instantiateCommandModule(command), Stage::Instantiate);
                case CommandKind::AssertInvalid:
                    return expectFailure(command, loadDefinition(command), Stage::Validate);
                case CommandKind::AssertMalformed:
                    return expectFailure(command, loadDefinition(command), Stage::Decode);
                case CommandKind::AssertMalformedCustom:
                    return expectFailure(command, loadModule(command), Stage::MalformedCustom);
                case CommandKind::AssertUnlinkable:
                    return expectFailure(command, instantiateCommandModule(command), Stage::Link);
                case CommandKind::AssertException: return assertException(command);
                case CommandKind::AssertInvalidCustom: return assertInvalidCustom(command);
                case CommandKind::Skip: return {};
            }
            return std::unexpected { std::string { "unknown command" } };
        }

        [[nodiscard]] static std::string describeFailure(Failure const& failure)
        {
            return failure.stage == Stage::Harness ? failure.message
                                                   : std::format("unexpected failure: {}", failure.message);
        }

        /// Checks that @p result failed at @p stage with a message holding the expected one, as the
        /// suite's rule finds it.
        template <typename T>
        [[nodiscard]] Outcome expectFailure(Command const& command,
                                            std::expected<T, Failure> const& result,
                                            Stage stage) const
        {
            return checkFailure(stage,
                                command.text,
                                result ? std::nullopt : std::optional { result.error() },
                                _options.suite.rule);
        }

        /// assert_exception: a core invocation must throw; a component call must trap with the uncaught
        /// exception's trap, as no exception crosses the component boundary.
        [[nodiscard]] Outcome assertException(Command const& command)
        {
            auto const target = instanceNamed(command.action.module);
            if (!target)
                return std::unexpected { target.error().message };
            if (std::holds_alternative<uint32_t>(*target))
                return expectFailure(command, perform(command.action), Stage::Exception);
            auto const uncaught =
                component::canonical::trapCodeRow(component::canonical::TrapCode::UncaughtException).format;
            auto const result = performComponent(command, *std::get<std::shared_ptr<ComponentRun>>(*target));
            return checkFailure(Stage::Execute,
                                uncaught,
                                result ? std::nullopt : std::optional { result.error() },
                                MessageRule::Substring);
        }

        /// Checks that a module loads, and that one of the custom sections the decoder interpreted was
        /// refused with a message starting with the expected one.
        [[nodiscard]] Outcome assertInvalidCustom(Command const& command) const
        {
            auto const module = loadModule(command);
            if (!module)
                return checkFailure(Stage::Custom, command.text, module.error());
            auto const& errors = (*module)->module().customErrors;
            auto const matches = [&](binary::CustomSectionError const& error) {
                return binary::specMessage(error.code).starts_with(command.text);
            };
            if (std::ranges::any_of(errors, matches))
                return {};
            // None matches: the first refusal, if any, is the one reported.
            if (errors.empty())
                return checkFailure(Stage::Custom, command.text, std::nullopt);
            auto const message = std::string { binary::specMessage(errors.front().code) };
            return checkFailure(
                Stage::Custom, command.text, Failure { .stage = Stage::Custom, .message = message });
        }

        /// The bytes of a command's module.
        [[nodiscard]] std::expected<std::vector<std::byte>, Failure> moduleBytes(Command const& command) const
        {
            return moduleBytesOf(command.source);
        }

        /// Decodes and validates a module.
        [[nodiscard]] std::expected<std::shared_ptr<validate::ValidatedModule const>, Failure> loadModule(
            std::span<std::byte const> bytes) const
        {
            struct Describe
            {
                Failure operator()(binary::DecodeError const& error) const
                {
                    return Failure { .stage = Stage::Decode, .message = binary::specMessage(error) };
                }
                Failure operator()(validate::ValidationError const& error) const
                {
                    return Failure { .stage = Stage::Validate, .message = validate::specMessage(error) };
                }
            };
            return validate::ValidatedModule::load(bytes, _options.features, binary::DecodeLimits {})
                .transform_error(
                    [](validate::ModuleError const& error) { return std::visit(Describe {}, error); });
        }

        /// Decodes and validates a command's module.
        [[nodiscard]] std::expected<std::shared_ptr<validate::ValidatedModule const>, Failure> loadModule(
            Command const& command) const
        {
            return moduleBytes(command).and_then(
                [this](std::vector<std::byte> const& bytes) { return loadModule(bytes); });
        }

        /// Loads a component on the runner's engine.
        [[nodiscard]] std::expected<component::api::Component, Failure> loadComponent(
            std::span<std::byte const> bytes)
        {
            struct Describe
            {
                Failure operator()(component::binary::DecodeError const& error) const
                {
                    return Failure { .stage = Stage::Decode,
                                     .message = component::binary::specMessage(error) };
                }
                Failure operator()(component::validate::ValidationError const& error) const
                {
                    return Failure { .stage = Stage::Validate,
                                     .message = component::validate::specMessage(error) };
                }
                Failure operator()(component::api::CoreModuleLoadError const& error) const
                {
                    return Failure { .stage = Stage::Validate,
                                     .message =
                                         std::format("core module {} refused at load", error.ordinal) };
                }
                Failure operator()(component::api::CoreModuleEngineMismatch const& error) const
                {
                    return harnessFailure(std::format("core module {} is of another engine", error.ordinal));
                }
                Failure operator()(component::api::CoreFeaturesMismatch const&) const
                {
                    return harnessFailure("the core module validator's features are not the engine's");
                }
                Failure operator()(isoline::api::HostMemoryExhausted const&) const
                {
                    return harnessFailure("the host ran out of memory loading a component");
                }
            };
            return component::api::Component::load(_engine, bytes, _componentConfig, _loader, _validator)
                .transform_error([](component::api::ComponentLoadError const& error) {
                    return std::visit(Describe {}, error);
                });
        }

        /// Whether a command's bytes are a component: by their preamble (layerOf()), or, for bytes of no
        /// layer, by the form the script gives them in, which decides whose decoder reports them.
        [[nodiscard]] static bool isComponent(ModuleSource const& source,
                                              std::span<std::byte const> bytes) noexcept
        {
            switch (component::binary::layerOf(bytes))
            {
                case component::binary::LayerKind::Component: return true;
                case component::binary::LayerKind::CoreModule: return false;
                case component::binary::LayerKind::Unknown: break;
            }
            return isComponentForm(source.form);
        }

        /// Loads a command's module or component, by the layer of its bytes.
        [[nodiscard]] std::expected<Definition, Failure> loadDefinition(Command const& command)
        {
            auto const bytes = moduleBytes(command);
            if (!bytes)
                return std::unexpected { bytes.error() };
            if (isComponent(command.source, *bytes))
                return loadComponent(*bytes).transform(
                    [](component::api::Component component) { return Definition { std::move(component) }; });
            return loadModule(*bytes).transform([](std::shared_ptr<validate::ValidatedModule const> module) {
                return Definition { std::move(module) };
            });
        }

        /// Resolves a module's imports against the registered modules, in import order.
        [[nodiscard]] std::expected<std::vector<ExternValue>, Failure> resolveImports(
            binary::Module const& module) const
        {
            auto resolved = std::vector<ExternValue> {};
            for (auto const& import: module.imports)
            {
                auto const exporter = _registered.find(binary::importModuleOf(module, import));
                auto const value =
                    exporter == _registered.end() ? std::nullopt : exportOf(exporter->second, import.name);
                if (!value)
                    return std::unexpected { Failure { .stage = Stage::Link, .message = "unknown import" } };
                resolved.push_back(*value);
            }
            return resolved;
        }

        [[nodiscard]] std::optional<ExternValue> exportOf(uint32_t instance, std::string_view name) const
        {
            for (auto const& [exportName, value]: _store.moduleInstance(instance).exports)
                if (exportName == name)
                    return value;
            return std::nullopt;
        }

        [[nodiscard]] std::expected<uint32_t, Failure> instantiateModule(
            std::shared_ptr<validate::ValidatedModule const> module)
        {
            auto const imports = resolveImports(module->module());
            if (!imports)
                return std::unexpected { imports.error() };
            auto const instance = runtime::instantiate(_store, _interpreter, std::move(module), *imports);
            if (instance)
                return *instance;
            struct Describe
            {
                Failure operator()(runtime::LinkError const& error) const
                {
                    return Failure { .stage = Stage::Link,
                                     .message = std::string { runtime::specMessage(error.code) } };
                }
                Failure operator()(runtime::LimitExceeded const& exceeded) const
                {
                    return Failure { .stage = Stage::Instantiate, .message = runtime::describe(exceeded) };
                }
                Failure operator()(runtime::AllocationFailure const& failure) const
                {
                    return Failure { .stage = Stage::Instantiate,
                                     .message = std::string { runtime::specMessage(failure) } };
                }
                Failure operator()(runtime::Trap const& trap) const
                {
                    return trapFailure(Stage::Instantiate, trap);
                }
                Failure operator()(runtime::Thrown const& thrown) const { return thrownFailure(thrown); }
                Failure operator()(runtime::HostFailure const& failure) const { return hostFailure(failure); }
            };
            return std::unexpected { std::visit(Describe {}, instance.error()) };
        }

        /// The text of a component trap: a core trap as the suite words it, else the trap's own.
        [[nodiscard]] std::string trapText(component::canonical::Trap const& trap) const
        {
            if (trap.code == component::canonical::TrapCode::Core && trap.core
                && _options.suite.coreTraps == CoreTrapText::Wasmtime)
                return std::string { coreTrapWording(*trap.core) };
            return trap.message();
        }

        /// Instantiates a component in a store of its own, against the component spectest.
        [[nodiscard]] std::expected<std::shared_ptr<ComponentRun>, Failure> instantiateComponent(
            component::api::Component const& component)
        {
            auto store = std::make_unique<component::api::ComponentStore>(
                _engine, _componentAllocator, _componentConfig);
            auto instance = store->instantiate(component, _linker);
            if (instance)
                return std::make_shared<ComponentRun>(
                    ComponentRun { .store = std::move(store), .instance = std::move(*instance) });
            auto const describe = [this](component::api::InstantiateComponentError const& error) -> Failure {
                if (auto const* missing = std::get_if<component::api::UnsatisfiedImport>(&error))
                    return Failure { .stage = Stage::Link,
                                     .message = std::format("import `{}` was not found", missing->name) };
                if (auto const* mismatch = std::get_if<component::api::ImportTypeMismatch>(&error))
                    return Failure { .stage = Stage::Link,
                                     .message = std::format("import `{}` has the wrong type: {}",
                                                            mismatch->name,
                                                            mismatch->detail) };
                if (std::holds_alternative<isoline::api::EngineMismatch>(error))
                    return harnessFailure("a component of another engine");
                if (auto const* foreign = std::get_if<component::api::ForeignInstance>(&error))
                    return harnessFailure(std::format("import `{}` of another store", foreign->name));
                auto const& failure = std::get<component::runtime::InstantiationFailure>(error);
                auto const& row = InstantiationStages.at(static_cast<std::size_t>(failure.kind));
                if (auto const* trap = std::get_if<component::canonical::Trap>(&failure.detail))
                    return Failure { .stage = row.stage, .message = trapText(*trap) };
                return Failure { .stage = row.stage, .message = std::string { row.text } };
            };
            return std::unexpected { describe(instance.error()) };
        }

        /// Instantiates a definition: a core module in the runner's store, a component in one of its own.
        [[nodiscard]] std::expected<Target, Failure> instantiateDefinition(Definition const& definition)
        {
            if (auto const* component = std::get_if<component::api::Component>(&definition))
                return instantiateComponent(*component).transform([](std::shared_ptr<ComponentRun> run) {
                    return Target { std::move(run) };
                });
            return instantiateModule(std::get<std::shared_ptr<validate::ValidatedModule const>>(definition))
                .transform([](uint32_t instance) { return Target { instance }; });
        }

        [[nodiscard]] std::expected<Target, Failure> instantiateCommandModule(Command const& command)
        {
            return loadDefinition(command).and_then(
                [&](Definition const& definition) { return instantiateDefinition(definition); });
        }

        /// Instantiates a module or a component and makes it current. A name names both the instance and,
        /// as in the reference interpreter's scripts, a definition for ModuleInstance.
        [[nodiscard]] Outcome defineModule(Command const& command)
        {
            _current = std::nullopt; // the commands after a module that failed fail too
            auto const definition = loadDefinition(command);
            if (!definition)
                return std::unexpected { describeFailure(definition.error()) };
            if (command.name)
                _definitions.insert_or_assign(*command.name, *definition);
            return makeCurrent(command, instantiateDefinition(*definition));
        }

        /// Decodes and validates a module or a component without instantiating it; a name keeps it for
        /// ModuleInstance.
        [[nodiscard]] Outcome defineDefinition(Command const& command)
        {
            auto const definition = loadDefinition(command);
            if (!definition)
                return std::unexpected { describeFailure(definition.error()) };
            if (command.name)
                _definitions.insert_or_assign(*command.name, *definition);
            return {};
        }

        /// Instantiates a named definition, as a module command instantiates its file.
        [[nodiscard]] Outcome instantiateDefinition(Command const& command)
        {
            _current = std::nullopt; // the commands after an instance that failed fail too
            auto const definition = _definitions.find(command.definition);
            if (definition == _definitions.end())
                return std::unexpected { std::format("no module definition named {}", command.definition) };
            return makeCurrent(command, instantiateDefinition(definition->second));
        }

        /// Makes an instantiated target current, under the command's name if it gives one.
        [[nodiscard]] Outcome makeCurrent(Command const& command,
                                          std::expected<Target, Failure> const& instance)
        {
            if (!instance)
                return std::unexpected { describeFailure(instance.error()) };
            _current = *instance;
            if (command.name)
                _named.insert_or_assign(*command.name, *instance);
            return {};
        }

        [[nodiscard]] std::expected<Target, Failure> instanceNamed(
            std::optional<std::string> const& name) const
        {
            if (!name)
            {
                if (!_current)
                    return std::unexpected { harnessFailure("no current module") };
                return *_current;
            }
            auto const found = _named.find(*name);
            if (found == _named.end())
                return std::unexpected { harnessFailure(std::format("no module named {}", *name)) };
            return found->second;
        }

        /// The core module instance @p name names.
        [[nodiscard]] std::expected<uint32_t, Failure> coreInstanceNamed(
            std::optional<std::string> const& name) const
        {
            return instanceNamed(name).and_then([](Target const& target) -> std::expected<uint32_t, Failure> {
                if (auto const* instance = std::get_if<uint32_t>(&target))
                    return *instance;
                return std::unexpected { harnessFailure(
                    "a component instance where a core module is expected") };
            });
        }

        /// Registers a core module instance for later imports. A component instance lives in a store of its
        /// own, which no other store's instantiation reaches, so it is no exporter (as in Wasmtime's runner).
        [[nodiscard]] Outcome registerModule(Command const& command)
        {
            auto const instance = coreInstanceNamed(command.name);
            if (!instance)
                return std::unexpected { std::format("register: {}", instance.error().message) };
            _registered[command.text] = *instance;
            return {};
        }

        /// Performs an action on its target, whichever layer it is of, its results unchecked.
        [[nodiscard]] std::expected<void, Failure> performAny(Command const& command)
        {
            auto const target = instanceNamed(command.action.module);
            if (!target)
                return std::unexpected { target.error() };
            if (auto const* run = std::get_if<std::shared_ptr<ComponentRun>>(&*target))
                return performComponent(command, **run).transform([](auto const&) {});
            return perform(command.action).transform([](auto const&) {});
        }

        /// Invokes an exported function or reads an exported global of a core module instance.
        [[nodiscard]] std::expected<std::vector<Value>, Failure> perform(Action const& action)
        {
            auto const instance = coreInstanceNamed(action.module);
            if (!instance)
                return std::unexpected { instance.error() };
            if (action.args.size() != action.arity)
                return std::unexpected { harnessFailure(
                    std::format("a component value passed to the core function '{}'", action.field)) };
            auto const target = exportOf(*instance, action.field);
            if (!target)
                return std::unexpected { harnessFailure(std::format("no export '{}'", action.field)) };

            if (action.kind == ActionKind::Get)
            {
                if (target->kind != ExternKind::Global)
                    return std::unexpected { harnessFailure(std::format("'{}' is no global", action.field)) };
                return std::vector { _store.global(target->address).value };
            }

            if (target->kind != ExternKind::Func)
                return std::unexpected { harnessFailure(std::format("'{}' is no function", action.field)) };
            auto const& type = _store.functionType(target->address);
            auto results = std::vector<Value> {};
            for (auto const& resultType: type.results)
                results.push_back(runtime::defaultValue(resultType));
            auto const invoked =
                runtime::invokeChecked(_store, _interpreter, target->address, action.args, results);
            if (invoked.entry == runtime::Entry::Refused)
                return std::unexpected { harnessFailure(
                    std::format("arguments {} do not match '{}'", describeAll(action.args), action.field)) };
            if (!invoked.outcome)
                return std::unexpected { abruptFailure(Stage::Execute, invoked.outcome.error()) };
            return results;
        }

        /// The failure of a component call, as CallFailureStages reports it.
        [[nodiscard]] Failure callFailure(component::api::ComponentCallError const& error) const
        {
            auto const& row = CallFailureStages.at(static_cast<std::size_t>(error.failure));
            if (!row.text.empty())
                return Failure { .stage = row.stage, .message = std::string { row.text } };
            if (auto const* trap = std::get_if<component::canonical::Trap>(&error.detail))
                return Failure { .stage = row.stage, .message = trapText(*trap) };
            return Failure { .stage = row.stage, .message = error.message() };
        }

        /// Calls an exported function of a component instance with the command's component arguments.
        [[nodiscard]] std::expected<ComponentResults, Failure> performComponent(Command const& command,
                                                                                ComponentRun& run) const
        {
            auto const& action = command.action;
            if (action.kind == ActionKind::Get)
                return std::unexpected { harnessFailure(
                    std::format("a component exports no global '{}'", action.field)) };
            if (command.componentArgs.size() != action.arity)
                return std::unexpected { harnessFailure(
                    std::format("a core value passed to the component function '{}'", action.field)) };
            auto const found = run.instance.function(action.field);
            if (!found)
                return std::unexpected { callFailure(found.error()) };
            if (!*found)
                return std::unexpected { harnessFailure(std::format(
                    "no export '{}' (or one that re-exports a host import, which the runner does not call)",
                    action.field)) };
            auto const& function = **found;
            auto builder = run.instance.argumentsFor(function);
            if (!builder)
                return std::unexpected { callFailure(builder.error()) };
            auto const* type = function.type();
            auto const* arena = function.types();
            if (type == nullptr || arena == nullptr)
                return std::unexpected { harnessFailure("the component store is gone") };
            auto parameters = std::vector<component::types::TypeId> {};
            for (auto const& [label, parameter]: type->params)
                parameters.push_back(parameter);
            if (auto const built = buildComponentValues(command.componentArgs, *arena, parameters, *builder);
                !built)
                return std::unexpected { built.error() };
            auto const arguments = builder->finish();
            if (!arguments)
                return std::unexpected { harnessFailure(arguments.error().message()) };
            auto results = run.instance.call(function, *arguments);
            if (!results)
                return std::unexpected { callFailure(results.error()) };
            return ComponentResults { .function = function, .values = std::move(*results) };
        }

        [[nodiscard]] Outcome assertReturn(Command const& command)
        {
            auto const target = instanceNamed(command.action.module);
            if (!target)
                return std::unexpected { target.error().message };
            if (auto const* run = std::get_if<std::shared_ptr<ComponentRun>>(&*target))
                return assertComponentReturn(command, **run);
            if (command.expected.size() != command.expectedCount)
                return std::unexpected { std::format("'{}': a component value expected of a core function",
                                                     command.action.field) };
            auto const results = perform(command.action);
            if (!results)
                return std::unexpected { describeFailure(results.error()) };
            return checkResults(command.expected, *results).transform_error([&](std::string const& mismatch) {
                return std::format("'{}': {}", command.action.field, mismatch);
            });
        }

        /// assert_return of a component function: each result equal to its expected value
        /// (canonical::equal(), every NaN equal to every NaN), built against the result's type.
        [[nodiscard]] Outcome assertComponentReturn(Command const& command, ComponentRun& run) const
        {
            auto const& field = command.action.field;
            if (command.componentExpected.size() != command.expectedCount)
                return std::unexpected { std::format("'{}': a core pattern expected of a component function",
                                                     field) };
            auto const results = performComponent(command, run);
            if (!results)
                return std::unexpected { describeFailure(results.error()) };
            auto const* type = results->function.type();
            auto const* arena = results->function.types();
            if (type == nullptr || arena == nullptr)
                return std::unexpected { std::string { "the component store is gone" } };
            auto resultTypes = std::vector<component::types::TypeId> {};
            if (type->result)
                resultTypes.push_back(*type->result);
            auto const expected = componentValuesOf(command.componentExpected,
                                                    *arena,
                                                    resultTypes,
                                                    std::make_shared<component::canonical::LiveBytesBudget>(
                                                        _componentConfig.limits.maxLiveValueBytes));
            if (!expected)
                return std::unexpected { std::format("'{}': {}", field, expected.error().message) };
            auto const& actual = results->values;
            for (auto const index: std::views::iota(std::size_t { 0 }, actual.rootCount()))
                if (!component::canonical::equal(expected->root(index), actual.root(index)))
                    return std::unexpected { std::format(
                        "'{}': expected {}, got {}",
                        field,
                        component::canonical::toText(*arena, expected->root(index)),
                        component::canonical::toText(*arena, actual.root(index))) };
            return {};
        }

        RunOptions _options;
        runtime::HeapMemoryAllocator _allocator;
        runtime::Store _store;
        interpreter::ReferenceInterpreter _interpreter;
        isoline::api::Engine _engine;
        component::api::EngineCoreModuleLoader _loader;
        component::api::EngineCoreModuleValidator _validator;
        isoline::api::HeapMemoryAllocator _componentAllocator;
        component::api::ComponentConfig _componentConfig;
        component::api::Linker _linker;
        std::optional<Target> _current;
        std::map<std::string, Target, std::less<>> _named;
        std::map<std::string, uint32_t, std::less<>> _registered;
        std::map<std::string, Definition, std::less<>> _definitions;
    };
    /// The commands of a script, and the oracle its modules given as text are checked against.
    struct Script
    {
        std::vector<Command> commands; ///< The commands, skips applied.
        Oracle oracle;                 ///< The script's oracle.
    };

    /// Why a script cannot be run: it or a file it needs cannot be read (exit status 2), or the oracle
    /// does not describe it (a Harness failure, exit status 1).
    struct ScriptError
    {
        int status = 2;      ///< The exit status.
        std::string message; ///< What is wrong.
    };

    /// Reads a text file whole.
    [[nodiscard]] std::expected<std::string, ScriptError> readText(std::filesystem::path const& path)
    {
        return readFile(path)
            .transform([](std::vector<std::byte> const& bytes) {
                return std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
            })
            .transform_error([](Failure const& failure) {
                return ScriptError { .status = 2, .message = failure.message };
            });
    }

    /// The error of a script or manifest that does not parse.
    [[nodiscard]] ScriptError parseFailure(std::filesystem::path const& path, ParseError const& error)
    {
        return ScriptError { .status = 2,
                             .message = std::format("{}:{}: {}", path.string(), error.line, error.message) };
    }

    /// The reason of a recorded parity divergence, `0x<offset> sha256:<digest> <reason>`: what follows its
    /// second space.
    [[nodiscard]] std::string_view parityReasonOf(std::string_view parity) noexcept
    {
        auto const digest = parity.find(' ');
        auto const reason = digest == std::string_view::npos ? digest : parity.find(' ', digest + 1);
        return reason == std::string_view::npos ? std::string_view {} : parity.substr(reason + 1);
    }

    /// Checks a script and its oracle against @p policy. Under SkipReasons, every skip's reason and every
    /// parity divergence's must be a SkipReasons row of its use. Under AnyReason, whose reasons go unpoliced,
    /// the script may hold no component: a component suite's file run without its rules (`--suite
    /// component`) would accept any reason.
    [[nodiscard]] std::expected<void, std::string> checkReasons(Oracle const& oracle,
                                                                std::span<Command const> commands,
                                                                SkipPolicy policy)
    {
        if (policy == SkipPolicy::AnyReason)
        {
            for (auto const [ordinal, command]: core::views::enumerate(commands))
                if (isComponentForm(command.source.form))
                    return std::unexpected { std::format(
                        "command {} (line {}) is a component, which runs under the component suites' rules "
                        "only (--suite component)",
                        ordinal,
                        command.line) };
            return {};
        }
        for (auto const& [ordinal, entry]: oracle.entries)
        {
            if (entry.skip && reasonRowOf(*entry.skip, ReasonUse::Skip) == nullptr)
                return std::unexpected { std::format(
                    "command {} (line {}) is skipped for '{}', which is no SkipReasons row of a skip",
                    ordinal,
                    entry.line,
                    *entry.skip) };
            if (entry.parity && reasonRowOf(parityReasonOf(*entry.parity), ReasonUse::Parity) == nullptr)
                return std::unexpected { std::format("command {} (line {}) records a parity divergence for "
                                                     "'{}', which is no SkipReasons row of a "
                                                     "parity divergence",
                                                     ordinal,
                                                     entry.line,
                                                     parityReasonOf(*entry.parity)) };
        }
        return {};
    }

    /// Reads `<name>.wast` and its oracle, which must count its commands alike, whose every entry must fit
    /// the command it lands on (checkEntries) and which must hold to @p policy (checkReasons), and
    /// marks the commands the oracle skips: they are never compiled nor run.
    [[nodiscard]] std::expected<Script, ScriptError> readWast(std::filesystem::path const& scriptPath,
                                                              SkipPolicy policy)
    {
        auto const text = readText(scriptPath);
        if (!text)
            return std::unexpected { text.error() };
        auto commands = parseWast(*text, text::TextLimits {});
        if (!commands)
            return std::unexpected { parseFailure(scriptPath, commands.error()) };
        auto oraclePath = scriptPath;
        oraclePath.replace_extension(".oracle");
        auto const manifest = readText(oraclePath);
        if (!manifest)
            return std::unexpected { manifest.error() };
        auto oracle = parseOracle(*manifest);
        if (!oracle)
            return std::unexpected { parseFailure(oraclePath, oracle.error()) };
        auto const aligned = checkAlignment(*oracle, commands->size())
                                 .and_then([&] { return checkEntries(*oracle, *commands); })
                                 .and_then([&] { return checkReasons(*oracle, *commands, policy); });
        if (!aligned)
            return std::unexpected { ScriptError {
                .status = 1,
                .message = std::format("{}: harness failure: {}", oraclePath.string(), aligned.error()) } };
        for (auto const& [ordinal, entry]: oracle->entries)
        {
            if (!entry.skip)
                continue;
            auto& command = commands->at(ordinal);
            command.kind = CommandKind::Skip;
            command.text = *entry.skip;
        }
        return Script { .commands = std::move(*commands), .oracle = std::move(*oracle) };
    }

    /// How a module given as text compared with the oracle's binary of it.
    enum class ParityKind : uint8_t
    {
        NotCompared, ///< The command has no module given as text, or the oracle no binary of it.
        NoEncoding,  ///< Isoline produces no encoding: the text does not compile (the command reports it).
        Agrees,      ///< The encodings mean the same module.
        Differs,     ///< They do not.
    };

    /// What checkParity() found.
    struct Parity
    {
        ParityKind kind = ParityKind::NotCompared; ///< How the encodings compared.
        /// Why they differ, `encoding differs from the oracle: …`; empty unless they do.
        std::string difference {};
        /// The first offset they differ at, as difference spells it (`0x…`); empty unless both decode.
        std::string offset {};
        /// `sha256:<hex>` of Isoline's encoding as compared (canonical when it decodes); empty when there
        /// is none.
        std::string digest {};
    };

    /// The `sha256:<hex>` digest of @p bytes, lower-case hex, as a parity divergence records it.
    [[nodiscard]] std::string digestOf(std::span<std::byte const> bytes)
    {
        auto digest = std::string { "sha256:" };
        for (auto const byte: core::Sha256::hash(bytes))
            digest += std::format("{:02x}", static_cast<unsigned>(byte));
        return digest;
    }

    /// The canonical form of a module or a component, by the layer of its bytes: binary::canonicalize, or
    /// component::binary::canonicalizeComponent under every gated feature, as the text is compiled.
    /// @param bytes A module or a component.
    /// @param features The core features the decoder accepts.
    /// @return The canonical bytes, or the decoder's message.
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> canonicalOf(
        std::span<std::byte const> bytes, FeatureSet const& features)
    {
        if (component::binary::layerOf(bytes) == component::binary::LayerKind::Component)
        {
            auto modules = component::text::P10CoreModules {};
            return component::binary::canonicalizeComponent(bytes,
                                                            component::text::everyComponentFeature(),
                                                            component::ComponentLimits {},
                                                            modules)
                .transform_error([](component::binary::DecodeError const& error) {
                    return component::binary::specMessage(error);
                });
        }
        return binary::canonicalize(bytes, features).transform_error([](binary::DecodeError const& error) {
            return std::string { binary::specMessage(error) };
        });
    }

    /// Checks that a module given as text encodes as the oracle's binary does, canonically
    /// (binary::canonicalize): encoding choices aside, the two must mean the same module. Two that do not
    /// decode are compared byte for byte; one that decodes and one that does not differ.
    /// @param command The command.
    /// @param entry What the oracle says of it, if anything.
    /// @param directory The script's directory, which holds the oracle's binaries.
    /// @param features The features the decoder accepts.
    /// @return How the encodings compare, or why the oracle's binary cannot be read.
    [[nodiscard]] std::expected<Parity, std::string> checkParity(Command const& command,
                                                                 OracleEntry const* entry,
                                                                 std::filesystem::path const& directory,
                                                                 FeatureSet const& features)
    {
        if (!entry || !entry->wasm || !isTextForm(command.source.form))
            return Parity {};
        auto const ours = moduleBytesOf(command.source);
        if (!ours)
            return Parity { .kind = ParityKind::NoEncoding };
        auto const theirs = readFile(directory / *entry->wasm);
        if (!theirs)
            return std::unexpected { theirs.error().message };
        auto const ourCanonical = canonicalOf(*ours, features);
        auto const theirCanonical = canonicalOf(*theirs, features);
        auto const& left = ourCanonical ? *ourCanonical : *ours;
        auto parity = Parity { .kind = ParityKind::Differs, .digest = digestOf(left) };
        if (ourCanonical.has_value() != theirCanonical.has_value())
        {
            parity.difference = std::format("encoding differs from the oracle: {} does not decode ({})",
                                            ourCanonical ? "the oracle's" : "Isoline's",
                                            ourCanonical ? theirCanonical.error() : ourCanonical.error());
            return parity;
        }
        auto const& right = theirCanonical ? *theirCanonical : *theirs;
        auto const [differs, _] = std::ranges::mismatch(left, right);
        if (differs == left.end() && left.size() == right.size())
            return Parity { .kind = ParityKind::Agrees };
        parity.offset = std::format("{:#x}", static_cast<std::size_t>(differs - left.begin()));
        parity.difference = std::format("encoding differs from the oracle: {}", parity.offset);
        return parity;
    }

    /// Holds a parity check to what the oracle records. Where it records no parity divergence, the
    /// encodings must agree. Where it records one, `0x<offset> sha256:<hex> <reason>`, they must differ
    /// first at exactly that offset, and Isoline's encoding must have exactly that digest: a divergence
    /// that no longer occurs or moves fails, and so does any later change to Isoline's encoding, so the
    /// record cannot go stale. A second difference already present behind the first when the row was
    /// recorded is not seen here: whoever records the row checks there is none.
    /// @param parity The parity check's result (checkParity).
    /// @param entry What the oracle says of the command, if anything.
    /// @return Nothing when the check agrees with the record, else why not.
    [[nodiscard]] std::expected<void, std::string> holdToRecord(
        std::expected<Parity, std::string> const& parity, OracleEntry const* entry)
    {
        if (!parity)
            return std::unexpected { parity.error() };
        if (!entry || !entry->parity)
        {
            if (parity->kind == ParityKind::Differs)
                return std::unexpected { parity->difference };
            return {};
        }
        auto const record = std::string_view { *entry->parity };
        auto const offset = record.substr(0, record.find(' '));
        auto const digest =
            record.substr(offset.size() + 1, record.find(' ', offset.size() + 1) - offset.size() - 1);
        switch (parity->kind)
        {
            case ParityKind::NotCompared:
                return std::unexpected { std::format(
                    "the recorded parity divergence at {} is not compared: the command has no module given "
                    "as text with an oracle binary",
                    offset) };
            case ParityKind::NoEncoding:
                return std::unexpected { std::format(
                    "the recorded parity divergence at {} cannot be checked: Isoline produces no encoding",
                    offset) };
            case ParityKind::Agrees:
                return std::unexpected { std::format(
                    "the recorded parity divergence at {} does not occur: the encodings agree", offset) };
            case ParityKind::Differs: break;
        }
        if (parity->offset != offset)
            return std::unexpected { std::format(
                "expected the recorded parity divergence at {}, got: {}", offset, parity->difference) };
        if (parity->digest != digest)
            return std::unexpected { std::format("the recorded parity divergence at {} is stale: Isoline's "
                                                 "encoding is now {}, not the recorded {}",
                                                 offset,
                                                 parity->digest,
                                                 digest) };
        return {};
    }

    /// The text stage an assertion on malformed text expects: a custom annotation's for
    /// assert_malformed_custom, any other for assert_malformed.
    [[nodiscard]] constexpr text::TextErrorStage textStageOf(CommandKind kind) noexcept
    {
        return kind == CommandKind::AssertMalformedCustom ? text::TextErrorStage::Custom
                                                          : text::TextErrorStage::Syntax;
    }

    /// Checks an expected divergence: an assert_malformed's or assert_malformed_custom's module text must
    /// fail to compile, at the stage the assertion expects, with exactly the message the oracle records
    /// for Isoline, its position aside. It stands in for the assertion, whose message the reference's is.
    /// @param command The assertion, which checkEntries() found to carry a module given as text.
    /// @param message Isoline's message, as the oracle records it.
    /// @return Nothing, or why Isoline's error is not the one recorded.
    [[nodiscard]] std::expected<void, std::string> checkDivergence(Command const& command,
                                                                   std::string_view message)
    {
        auto const compiled = compileSource(command.source);
        if (compiled)
            return std::unexpected { std::format(
                "expected the recorded divergence '{}', but the text compiles", message) };
        auto const actual = text::specMessage(compiled.error());
        if (text::stageOf(compiled.error().code) != textStageOf(command.kind) || actual != message)
            return std::unexpected { std::format(
                "expected the recorded divergence '{}' ({} stage), got '{}'",
                message,
                stageName(TextErrorStages.at(static_cast<std::size_t>(textStageOf(command.kind)))),
                actual) };
        return {};
    }

    /// Runs the script in @p directory: `<name>.wast` against `<name>.oracle`.
    /// @param directory The directory of one imported .wast file.
    /// @param expectedSkips How many of its commands are skipped.
    /// @param options The features the decoder and the validator accept, the engine and the suite's rules.
    /// @return 0 when every command passed and exactly @p expectedSkips were skipped, 1 when not or when
    ///         the oracle does not describe the script, and 2 when a file cannot be read.
    [[nodiscard]] int runDirectory(std::filesystem::path directory,
                                   std::size_t expectedSkips,
                                   RunOptions const& options)
    {
        auto const& features = options.features;
        if (!directory.has_filename())
            directory = directory.parent_path();
        auto const scriptPath = directory / (directory.filename().string() + ".wast");
        auto const script = readWast(scriptPath, options.suite.skips);
        if (!script)
        {
            (script.error().status == 2 ? std::cerr : std::cout) << script.error().message << '\n';
            return script.error().status;
        }

        auto runner = Runner { options };
        auto counts = std::array<std::size_t, 3> {};
        auto const location = scriptPath.string();
        for (auto const [ordinal, command]: core::views::enumerate(script->commands))
        {
            auto const entry = script->oracle.entries.find(ordinal);
            auto const* const oracleEntry = entry == script->oracle.entries.end() ? nullptr : &entry->second;
            auto const parity =
                command.kind == CommandKind::Skip
                    ? std::expected<void, std::string> {}
                    : holdToRecord(checkParity(command, oracleEntry, directory, features), oracleEntry);
            if (!parity)
                std::cout << std::format("{}:{}: {}\n", location, command.line, parity.error());
            // A command the oracle records a divergence for is checked against it instead of run.
            auto const* const divergence =
                oracleEntry && oracleEntry->message ? &*oracleEntry->message : nullptr;
            auto verdict = Verdict::Skipped;
            if (divergence)
            {
                auto const diverged = checkDivergence(command, *divergence);
                if (!diverged)
                    std::cout << std::format("{}:{}: {}\n", location, command.line, diverged.error());
                verdict = diverged ? Verdict::Passed : Verdict::Failed;
            }
            else
                verdict = runner.run(command, location);
            ++counts.at(static_cast<std::size_t>(parity ? verdict : Verdict::Failed));
        }
        std::cout << std::format("passed {}, failed {}, skipped {}\n",
                                 counts[static_cast<std::size_t>(Verdict::Passed)],
                                 counts[static_cast<std::size_t>(Verdict::Failed)],
                                 counts[static_cast<std::size_t>(Verdict::Skipped)]);
        auto const skipped = counts[static_cast<std::size_t>(Verdict::Skipped)];
        if (skipped != expectedSkips)
            std::cout << std::format(
                "{}: skipped {}, but {} skips are pinned\n", location, skipped, expectedSkips);
        return counts[static_cast<std::size_t>(Verdict::Failed)] == 0 && skipped == expectedSkips ? 0 : 1;
    }

    /// The skip count a command line names.
    /// @param text The argument.
    /// @return Its value, or nullopt when it is no decimal number.
    [[nodiscard]] std::optional<std::size_t> parseCount(std::string_view text)
    {
        auto count = std::size_t { 0 };
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
        if (error != std::errc {} || end != text.data() + text.size())
            return std::nullopt;
        return count;
    }

    /// Whether a command line must give an option.
    enum class OptionPresence : uint8_t
    {
        Optional, ///< It may be left out, for its default.
        Required, ///< It must be given.
    };

    /// Reads an option's value into the options, or says why it names nothing.
    using OptionReader = std::expected<void, std::string> (*)(RunOptions& options, std::string_view value);

    /// One option of the command line: its flag, whether it is required, and what reads its value.
    struct OptionRow
    {
        std::string_view flag;   ///< `--features` and its siblings.
        OptionPresence presence; ///< Whether it must be given.
        OptionReader read;       ///< What reads its value.
        std::string_view usage;  ///< Its value, for the usage line.
    };

    /// The row of @p rows whose `name` is @p name, or nullptr.
    template <typename Row, std::size_t Count>
    [[nodiscard]] constexpr Row const* rowNamed(std::array<Row, Count> const& rows,
                                                std::string_view name) noexcept
    {
        auto const found = std::ranges::find(rows, name, &Row::name);
        return found == rows.end() ? nullptr : &*found;
    }

    /// The gated features a `--component-features` value names: `none`, or feature names (wasm-tools'
    /// `--features` spellings) joined by commas.
    [[nodiscard]] std::expected<component::ComponentFeatureSet, std::string> componentFeaturesNamed(
        std::string_view value)
    {
        auto features = component::ComponentFeatureSet::defaults();
        if (value == "none")
            return features;
        for (auto const name: std::views::split(value, ','))
        {
            auto const text = std::string_view { name.begin(), name.end() };
            auto const row = std::ranges::find(
                component::ComponentFeatureTable, text, &component::ComponentFeatureRow::name);
            if (row == component::ComponentFeatureTable.end())
                return std::unexpected { std::format("no component feature named '{}'", text) };
            features = features.with(row->feature);
        }
        return features;
    }

    /// Every option of the command line. Adding one is adding a row.
    constexpr auto Options = std::to_array<OptionRow>({
        { .flag = "--features",
          .presence = OptionPresence::Required,
          .read = [](RunOptions& options, std::string_view value) -> std::expected<void, std::string> {
              auto const features = featureSetNamed(value);
              if (!features)
                  return std::unexpected { std::format("no feature set named '{}'", value) };
              options.features = *features;
              return {};
          },
          .usage = "<wasm2|wasm3|all>" },
        { .flag = "--engine",
          .presence = OptionPresence::Optional,
          .read = [](RunOptions& options, std::string_view value) -> std::expected<void, std::string> {
              auto const* const row = rowNamed(Engines, value);
              if (row == nullptr)
                  return std::unexpected { std::format("no engine named '{}'", value) };
              options.engine = row->engine;
              return {};
          },
          .usage = "<reference>" },
        { .flag = "--suite",
          .presence = OptionPresence::Optional,
          .read = [](RunOptions& options, std::string_view value) -> std::expected<void, std::string> {
              auto const* const row = rowNamed(Suites, value);
              if (row == nullptr)
                  return std::unexpected { std::format("no suite named '{}'", value) };
              options.suite = *row;
              return {};
          },
          .usage = "<core|component>" },
        { .flag = "--component-features",
          .presence = OptionPresence::Optional,
          .read = [](RunOptions& options, std::string_view value) -> std::expected<void, std::string> {
              return componentFeaturesNamed(value).transform(
                  [&](component::ComponentFeatureSet features) { options.componentFeatures = features; });
          },
          .usage = "<none|name,name…>" },
    });

    /// The options a command line gives after its directory and skip count, as `--flag value` pairs.
    /// @param arguments The pairs.
    /// @return The options; or why they are none (an unknown or repeated flag, a bad value, a missing one).
    [[nodiscard]] std::expected<RunOptions, std::string> parseOptions(std::span<char const* const> arguments)
    {
        if (arguments.size() % 2 != 0)
            return std::unexpected { std::string { "every option takes a value" } };
        auto options = RunOptions { .features = FeatureSet::wasm3() };
        auto given = std::array<std::size_t, Options.size()> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, arguments.size() / 2))
        {
            auto const flag = std::string_view { arguments[2 * index] };
            auto const row = std::ranges::find(Options, flag, &OptionRow::flag);
            if (row == Options.end())
                return std::unexpected { std::format("unknown option '{}'", flag) };
            if (++given.at(static_cast<std::size_t>(row - Options.begin())) > 1)
                return std::unexpected { std::format("'{}' given twice", flag) };
            if (auto const read = row->read(options, arguments[(2 * index) + 1]); !read)
                return std::unexpected { read.error() };
        }
        for (auto const index: std::views::iota(std::size_t { 0 }, Options.size()))
            if (Options.at(index).presence == OptionPresence::Required && given.at(index) == 0)
                return std::unexpected { std::format("'{}' is required", Options.at(index).flag) };
        return options;
    }

    /// The command line's usage, every option listed.
    [[nodiscard]] std::string usage()
    {
        auto text = std::string { "usage: isoline_spec_runner <directory> <expected skips>" };
        for (auto const& option: Options)
            text += option.presence == OptionPresence::Required
                        ? std::format(" {} {}", option.flag, option.usage)
                        : std::format(" [{} {}]", option.flag, option.usage);
        return text;
    }
} // namespace

} // namespace isoline::spectest

int main(int argc, char const* argv[])
{
    auto const arguments = std::span { argv, static_cast<std::size_t>(argc) };
    auto const expectedSkips =
        arguments.size() >= 3 ? isoline::spectest::parseCount(arguments[2]) : std::nullopt;
    auto const options =
        arguments.size() >= 3
            ? isoline::spectest::parseOptions(arguments.subspan(3))
            : std::expected<isoline::spectest::RunOptions, std::string> { std::unexpect,
                                                                          "no directory and skip count" };
    if (!expectedSkips || !options)
    {
        if (!options)
            std::cerr << "isoline_spec_runner: " << options.error() << '\n';
        std::cerr << isoline::spectest::usage() << '\n';
        return 2;
    }
    try
    {
        return isoline::spectest::runDirectory(arguments[1], *expectedSkips, *options);
    }
    catch (std::exception const& error)
    {
        std::cerr << "isoline_spec_runner: " << error.what() << '\n';
        return 2;
    }
}
