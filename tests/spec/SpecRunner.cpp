// SPDX-License-Identifier: Apache-2.0
//
// isoline_spec_runner <dir> <skips> --features <set>: runs <dir>/<name>.isotest, one imported .wast
// file of the spec suite, against the decoder, the validator and the reference interpreter, with the
// features of <set> on (wasm2, wasm3 or all). It fails when a command fails, and when the script
// skips other than <skips> commands: every skip is pinned in tests/spec/files.txt, so none appears
// unnoticed.

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

#include "IsotestReader.hpp"
#include "SpecCheck.hpp"
#include "SpecFeatures.hpp"
#include <isoline/Features.hpp>
#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
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
        Runner(std::filesystem::path directory, FeatureSet features):
            _directory { std::move(directory) },
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
            return readFile(_directory / command.file);
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

        std::filesystem::path _directory;
        FeatureSet _features;
        runtime::HeapMemoryAllocator _allocator;
        runtime::Store _store;
        interpreter::ReferenceInterpreter _interpreter;
        std::optional<uint32_t> _current;
        std::map<std::string, uint32_t, std::less<>> _named;
        std::map<std::string, uint32_t, std::less<>> _registered;
        std::map<std::string, std::shared_ptr<validate::ValidatedModule const>, std::less<>> _definitions;
    };

    /// Runs the script in @p directory.
    /// @param directory The directory of one imported .wast file.
    /// @param expectedSkips How many of its commands are skip lines.
    /// @param features The features the decoder and the validator accept.
    /// @return 0 when every command passed and exactly @p expectedSkips were skipped, 1 when not,
    ///         and 2 when the script cannot be read.
    [[nodiscard]] int runDirectory(std::filesystem::path directory,
                                   std::size_t expectedSkips,
                                   FeatureSet features)
    {
        if (!directory.has_filename())
            directory = directory.parent_path();
        auto const scriptPath = directory / (directory.filename().string() + ".isotest");
        auto const bytes = readFile(scriptPath);
        if (!bytes)
        {
            std::cerr << bytes.error().message << '\n';
            return 2;
        }
        auto const text = std::string { reinterpret_cast<char const*>(bytes->data()), bytes->size() };
        auto const commands = parseIsotest(text);
        if (!commands)
        {
            std::cerr << std::format(
                "{}:{}: {}\n", scriptPath.string(), commands.error().line, commands.error().message);
            return 2;
        }

        auto runner = Runner { directory, features };
        auto counts = std::array<std::size_t, 3> {};
        auto const location = scriptPath.string();
        for (auto const& command: *commands)
            ++counts.at(static_cast<std::size_t>(runner.run(command, location)));
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
