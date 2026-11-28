// SPDX-License-Identifier: Apache-2.0
//
// isoline_spec_runner <dir> <skips> --features <set>: runs one imported .wast file of the spec suite,
// <dir>/<name>.wast with its oracle <dir>/<name>.oracle, against the text reader, the decoder, the validator
// and the reference interpreter, with the features of <set> on (wasm2, wasm3 or all). It fails when a command
// fails, when a module given as text does not encode as the oracle's binary does, and when the script skips
// other than <skips> commands: every skip is pinned in tests/spec/files.txt, so none appears unnoticed.

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

#include "Oracle.hpp"
#include "Script.hpp"
#include "SpecCheck.hpp"
#include "SpecFeatures.hpp"
#include "WastReader.hpp"
#include <isoline/Features.hpp>
#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
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
            source.form == ModuleForm::Text
                ? std::format("{}:{}",
                              source.position.line + at.line - 1,
                              at.line == 1 ? source.position.column + at.column - 1 : at.column)
                : std::format("{}:{} of its quoted text", at.line, at.column);
        return Failure { .stage = stage,
                         .message = std::format("{} at {}", text::specMessage(error), where) };
    }

    /// The bytes of a command's module: a binary's strings, or the text compiled.
    /// @param source The module.
    /// @return The bytes, or the failure of malformed text.
    [[nodiscard]] std::expected<std::vector<std::byte>, Failure> moduleBytesOf(ModuleSource const& source)
    {
        switch (source.form)
        {
            case ModuleForm::Binary: {
                auto const bytes = std::as_bytes(std::span { source.text });
                return std::vector<std::byte> { bytes.begin(), bytes.end() };
            }
            case ModuleForm::Text:
            case ModuleForm::Quote: break;
        }
        return text::compileText(source.text, text::TextLimits {}, text::CoreForms)
            .transform_error([&](text::TextError const& error) { return textFailure(source, error); });
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

    /// Runs the commands of one script against one store, as the spec interpreter runs a .wast file.
    class Runner
    {
      public:
        explicit Runner(FeatureSet features):
            _features { features },
            _store { runtime::StoreLimits {}, runtime::StoreServices { .allocator = _allocator } }
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

        [[nodiscard]] Outcome execute(Command const& command)
        {
            switch (command.kind)
            {
                case CommandKind::Module: return defineModule(command);
                case CommandKind::ModuleDefinition: return defineDefinition(command);
                case CommandKind::ModuleInstance: return instantiateDefinition(command);
                case CommandKind::Register: return registerModule(command);
                case CommandKind::Invoke:
                    return perform(command.action)
                        .transform([](auto const&) {})
                        .transform_error(&describeFailure);
                case CommandKind::AssertReturn: return assertReturn(command);
                case CommandKind::AssertTrap:
                case CommandKind::AssertExhaustion:
                    return expectFailure(command, perform(command.action), Stage::Execute);
                case CommandKind::AssertTrapModule:
                case CommandKind::AssertUninstantiable:
                    return expectFailure(command, instantiateCommandModule(command), Stage::Instantiate);
                case CommandKind::AssertInvalid:
                    return expectFailure(command, loadModule(command), Stage::Validate);
                case CommandKind::AssertMalformed:
                    return expectFailure(command, loadModule(command), Stage::Decode);
                case CommandKind::AssertMalformedCustom:
                    return expectFailure(command, loadModule(command), Stage::MalformedCustom);
                case CommandKind::AssertUnlinkable:
                    return expectFailure(command, instantiateCommandModule(command), Stage::Link);
                case CommandKind::AssertException:
                    return expectFailure(command, perform(command.action), Stage::Exception);
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

        /// Checks that @p result failed at @p stage with a message starting with the expected one.
        template <typename T>
        [[nodiscard]] static Outcome expectFailure(Command const& command,
                                                   std::expected<T, Failure> const& result,
                                                   Stage stage)
        {
            return checkFailure(
                stage, command.text, result ? std::nullopt : std::optional { result.error() });
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
            return validate::ValidatedModule::load(bytes, _features, binary::DecodeLimits {})
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

        [[nodiscard]] std::expected<uint32_t, Failure> instantiateCommandModule(Command const& command)
        {
            return loadModule(command).and_then([&](std::shared_ptr<validate::ValidatedModule const> module) {
                return instantiateModule(std::move(module));
            });
        }

        /// Instantiates a module and makes it current. A name names both the instance and, as in
        /// the reference interpreter's scripts, a definition for ModuleInstance.
        [[nodiscard]] Outcome defineModule(Command const& command)
        {
            _current = std::nullopt; // the commands after a module that failed fail too
            auto module = loadModule(command);
            if (!module)
                return std::unexpected { describeFailure(module.error()) };
            if (command.name)
                _definitions[*command.name] = *module;
            auto const instance = instantiateModule(std::move(*module));
            if (!instance)
                return std::unexpected { describeFailure(instance.error()) };
            _current = *instance;
            if (command.name)
                _named[*command.name] = *instance;
            return {};
        }

        /// Decodes and validates a module without instantiating it; a name keeps it for
        /// ModuleInstance.
        [[nodiscard]] Outcome defineDefinition(Command const& command)
        {
            auto module = loadModule(command);
            if (!module)
                return std::unexpected { describeFailure(module.error()) };
            if (command.name)
                _definitions[*command.name] = std::move(*module);
            return {};
        }

        /// Instantiates a named definition, as a module command instantiates its file.
        [[nodiscard]] Outcome instantiateDefinition(Command const& command)
        {
            _current = std::nullopt; // the commands after an instance that failed fail too
            auto const definition = _definitions.find(command.definition);
            if (definition == _definitions.end())
                return std::unexpected { std::format("no module definition named {}", command.definition) };
            auto const instance = instantiateModule(definition->second);
            if (!instance)
                return std::unexpected { describeFailure(instance.error()) };
            _current = *instance;
            if (command.name)
                _named[*command.name] = *instance;
            return {};
        }

        [[nodiscard]] std::expected<uint32_t, Failure> instanceNamed(
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

        [[nodiscard]] Outcome registerModule(Command const& command)
        {
            auto const instance = instanceNamed(command.name);
            if (!instance)
                return std::unexpected { instance.error().message };
            _registered[command.text] = *instance;
            return {};
        }

        /// Invokes an exported function or reads an exported global.
        [[nodiscard]] std::expected<std::vector<Value>, Failure> perform(Action const& action)
        {
            auto const instance = instanceNamed(action.module);
            if (!instance)
                return std::unexpected { instance.error() };
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

        [[nodiscard]] Outcome assertReturn(Command const& command)
        {
            auto const results = perform(command.action);
            if (!results)
                return std::unexpected { describeFailure(results.error()) };
            return checkResults(command.expected, *results).transform_error([&](std::string const& mismatch) {
                return std::format("'{}': {}", command.action.field, mismatch);
            });
        }

        FeatureSet _features;
        runtime::HeapMemoryAllocator _allocator;
        runtime::Store _store;
        interpreter::ReferenceInterpreter _interpreter;
        std::optional<uint32_t> _current;
        std::map<std::string, uint32_t, std::less<>> _named;
        std::map<std::string, uint32_t, std::less<>> _registered;
        std::map<std::string, std::shared_ptr<validate::ValidatedModule const>, std::less<>> _definitions;
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

    /// Reads `<name>.wast` and its oracle, which must count its commands alike and whose every entry
    /// must fit the command it lands on (checkEntries), and marks the commands the oracle skips: they are
    /// never compiled nor run.
    [[nodiscard]] std::expected<Script, ScriptError> readWast(std::filesystem::path const& scriptPath)
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
        auto const aligned = checkAlignment(*oracle, commands->size()).and_then([&] {
            return checkEntries(*oracle, *commands);
        });
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
        if (!entry || !entry->wasm
            || (command.source.form != ModuleForm::Text && command.source.form != ModuleForm::Quote))
            return Parity {};
        auto const ours = moduleBytesOf(command.source);
        if (!ours)
            return Parity { .kind = ParityKind::NoEncoding };
        auto const theirs = readFile(directory / *entry->wasm);
        if (!theirs)
            return std::unexpected { theirs.error().message };
        auto const ourCanonical = binary::canonicalize(*ours, features);
        auto const theirCanonical = binary::canonicalize(*theirs, features);
        auto const& left = ourCanonical ? *ourCanonical : *ours;
        auto parity = Parity { .kind = ParityKind::Differs, .digest = digestOf(left) };
        if (ourCanonical.has_value() != theirCanonical.has_value())
        {
            parity.difference = std::format(
                "encoding differs from the oracle: {} does not decode ({})",
                ourCanonical ? "the oracle's" : "Isoline's",
                binary::specMessage(ourCanonical ? theirCanonical.error() : ourCanonical.error()));
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
        auto const compiled = text::compileText(command.source.text, text::TextLimits {}, text::CoreForms);
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
    /// @param features The features the decoder and the validator accept.
    /// @return 0 when every command passed and exactly @p expectedSkips were skipped, 1 when not or when
    ///         the oracle does not describe the script, and 2 when a file cannot be read.
    [[nodiscard]] int runDirectory(std::filesystem::path directory,
                                   std::size_t expectedSkips,
                                   FeatureSet features)
    {
        if (!directory.has_filename())
            directory = directory.parent_path();
        auto const scriptPath = directory / (directory.filename().string() + ".wast");
        auto const script = readWast(scriptPath);
        if (!script)
        {
            (script.error().status == 2 ? std::cerr : std::cout) << script.error().message << '\n';
            return script.error().status;
        }

        auto runner = Runner { features };
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
} // namespace

} // namespace isoline::spectest

int main(int argc, char const* argv[])
{
    auto const arguments = std::span { argv, static_cast<std::size_t>(argc) };
    auto const expectedSkips =
        arguments.size() == 5 ? isoline::spectest::parseCount(arguments[2]) : std::nullopt;
    auto const features = arguments.size() == 5 && std::string_view { arguments[3] } == "--features"
                              ? isoline::spectest::featureSetNamed(arguments[4])
                              : std::nullopt;
    if (!expectedSkips || !features)
    {
        std::cerr << "usage: isoline_spec_runner <directory> <expected skips> --features <wasm2|wasm3|all>\n";
        return 2;
    }
    try
    {
        return isoline::spectest::runDirectory(arguments[1], *expectedSkips, *features);
    }
    catch (std::exception const& error)
    {
        std::cerr << "isoline_spec_runner: " << error.what() << '\n';
        return 2;
    }
}
