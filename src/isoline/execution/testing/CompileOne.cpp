// SPDX-License-Identifier: Apache-2.0
#include <isoline/execution/testing/CompileOne.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/HostTarget.hpp>
#include <isoline/compiler/Compiler.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>
#include <isoline/compiler/x86_64/Target.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/CountingArena.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

namespace isoline::execution::testing
{

std::shared_ptr<validate::ValidatedModule const> moduleOf(std::string_view wat)
{
    auto const bytes = text::compileText(wat, text::TextLimits {});
    Require(bytes.has_value());
    auto loaded = validate::ValidatedModule::load(*bytes, FeatureSet::all(), binary::DecodeLimits {});
    Require(loaded.has_value());
    return std::move(*loaded);
}

compiler::CompileConfig hostConfig(runtime::ExecutionMode mode, compiler::machine::CpuFeatures features)
{
    return compiler::CompileConfig { compiler::machine::TargetDescription {
                                         .architecture = compiler::machine::Architecture::X64,
                                         .abi = HostOperatingSystemAbi,
                                         .features = features },
                                     mode,
                                     compiler::SpectreGuard::Enabled };
}

bool hostRunsCompiledCode() noexcept
{
    return HostArchitecture == compiler::machine::Architecture::X64;
}

std::expected<std::shared_ptr<CompiledModuleCode const>, compiler::CompileError> compileModule(
    validate::ValidatedModule const& module, compiler::CompileConfig const& config)
{
    auto target = compiler::x86_64::X64Target::create(config.target);
    if (!target)
        return std::unexpected { target.error() };
    auto arena = runtime::CountingArena { uint64_t { 4 } << 30, *std::pmr::new_delete_resource() };
    auto const cancellation = compiler::CompileCancellation {};
    auto const compiler = compiler::Compiler { config, *target, arena, cancellation };
    auto compiled = compiler.compile(module);
    if (!compiled)
        return std::unexpected { compiled.error() };
    auto memory = PortableCodeMemory {};
    auto sealed = CompiledModuleCode::create(
        std::make_shared<compiler::CompiledCode const>(std::move(*compiled)), memory);
    Require(sealed.has_value());
    return std::move(*sealed);
}

std::expected<std::unique_ptr<CompiledInstance>, compiler::CompileError> CompiledInstance::create(
    std::shared_ptr<validate::ValidatedModule const> module,
    compiler::CompileConfig const& config,
    InstanceOptions options)
{
    auto code = compileModule(*module, config);
    if (!code)
        return std::unexpected { code.error() };
    return std::unique_ptr<CompiledInstance> { new CompiledInstance {
        std::move(module), std::move(*code), std::move(options) } };
}

CompiledInstance::CompiledInstance(std::shared_ptr<validate::ValidatedModule const> module,
                                   std::shared_ptr<CompiledModuleCode const> code,
                                   InstanceOptions options):
    _module { std::move(module) },
    _code { std::move(code) },
    _options { std::move(options) },
    _store { _options.limits,
             runtime::StoreServices { .allocator = _allocator,
                                      .pollObserver = _options.observer,
                                      .dispatchRows = runtime::DispatchRows::Kept } }
{
    auto const& bounds = _options.bounds != nullptr ? *_options.bounds : _threadBounds;
    auto const grant = _options.nativeStackBytes.value_or(
        requiredNativeStack(compiler::x86_64::X64FrameRule, _store.limits()));
    auto executor =
        CompiledExecutor::create(CompiledExecutorConfig { _code->mode(), grant }, _store.limits(), bounds);
    Require(executor.has_value());
    _executor = std::move(*executor);
    auto const imports = _options.imports ? _options.imports(_store) : std::vector<runtime::ExternValue> {};
    auto const instantiated = runtime::instantiate(_store, *_executor, _module, _code, imports);
    Require(instantiated.has_value());
    _instance = *instantiated;
    if (_options.interruption == Interruption::Pending)
        _store.interrupter()->interrupt();
}

compiler::ExecutionState const& CompiledInstance::state() const noexcept
{
    return ExecutorInternals::state(*_executor, const_cast<runtime::Store&>(_store));
}

compiler::InstanceContext const& CompiledInstance::context() const noexcept
{
    return ExecutorInternals::context(*_executor, const_cast<runtime::Store&>(_store), _instance);
}

ExecutorRecord& CompiledInstance::record() noexcept
{
    return ExecutorInternals::record(*_executor, _store);
}

runtime::FuncAddr CompiledInstance::addressOf(uint32_t codeIndex) const
{
    auto const& instance = _store.moduleInstance(_instance);
    auto const imported = instance.functions.size() - _module->module().functions.size();
    return instance.functions.at(imported + codeIndex);
}

void CompiledInstance::lent(
    std::function<void(compiler::ExecutionState&, compiler::InstanceContext const&)> const& work)
{
    auto const scope = runtime::InvocationScope { _store };
    auto& state = ExecutorInternals::state(*_executor, _store);
    auto& record = ExecutorInternals::record(*_executor, _store);
    auto& gate = ExecutorInternals::gate(*_executor, _store);
    record.lent = gate.lend();
    state.stepsLeft = record.lent.steps;
    state.stackValuesLeft = record.lent.stackValues;
    state.callDepthLeft = _store.limits().maxCallDepth;
    state.pending = {};
    work(state, context());
    gate.settle(record.lent,
                runtime::Lease { .steps = state.stepsLeft, .stackValues = state.stackValuesLeft });
}

std::expected<std::vector<runtime::Value>, runtime::Abrupt> CompiledInstance::call(
    uint32_t codeIndex, std::span<runtime::Value const> arguments)
{
    return invoke(addressOf(codeIndex), arguments);
}

std::expected<std::vector<runtime::Value>, runtime::Abrupt> CompiledInstance::invoke(
    runtime::FuncAddr address, std::span<runtime::Value const> arguments)
{
    auto results = std::vector<runtime::Value>(_store.functionType(address).results.size());
    auto const invoked = runtime::invokeChecked(_store, *_executor, address, arguments, results);
    if (!invoked.outcome)
        return std::unexpected { invoked.outcome.error() };
    return results;
}

std::expected<std::vector<runtime::Value>, runtime::Abrupt> CompiledInstance::callThrough(
    compiler::EntryTrampoline entry, uint32_t codeIndex, std::span<runtime::Value const> arguments)
{
    auto const address = addressOf(codeIndex);
    auto results = std::vector<runtime::Value>(_store.functionType(address).results.size());
    auto const scope = runtime::InvocationScope { _store };
    auto const entered =
        ExecutorInternals::enterThrough(*_executor, _store, address, arguments, results, entry);
    if (!entered)
        return std::unexpected { entered.error() };
    return results;
}

ReferenceRun runReference(std::shared_ptr<validate::ValidatedModule const> const& module,
                          uint32_t codeIndex,
                          std::span<runtime::Value const> arguments,
                          InstanceOptions const& options)
{
    auto allocator = runtime::HeapMemoryAllocator {};
    auto store = runtime::Store {
        options.limits, runtime::StoreServices { .allocator = allocator, .pollObserver = options.observer }
    };
    auto interpreter = interpreter::ReferenceInterpreter {};
    auto const imports = options.imports ? options.imports(store) : std::vector<runtime::ExternValue> {};
    auto const instantiated = runtime::instantiate(store, interpreter, module, imports);
    Require(instantiated.has_value());
    auto const& instance = store.moduleInstance(*instantiated);
    auto const imported = instance.functions.size() - module->module().functions.size();
    auto const address = instance.functions[imported + codeIndex];
    auto results = std::vector<runtime::Value>(store.functionType(address).results.size());
    if (options.interruption == Interruption::Pending)
        store.interrupter()->interrupt();
    auto const invoked = runtime::invokeChecked(store, interpreter, address, arguments, results);
    auto run = ReferenceRun { .outcome = std::vector<runtime::Value> {},
                              .steps = store.used(runtime::Limit::Steps) };
    if (invoked.outcome)
        run.outcome = std::move(results);
    else
        run.outcome = std::unexpected { invoked.outcome.error() };
    return run;
}

} // namespace isoline::execution::testing
