// SPDX-License-Identifier: Apache-2.0
#include <isoline/execution/testing/CompileOne.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/HostTarget.hpp>
#include <isoline/compiler/CompileLimits.hpp>
#include <isoline/compiler/machine/CodeBuffer.hpp>
#include <isoline/compiler/machine/Target.hpp>
#include <isoline/compiler/optimization/PassTable.hpp>
#include <isoline/compiler/registerallocation/LinearScan.hpp>
#include <isoline/compiler/translation/Translator.hpp>
#include <isoline/compiler/x86_64/Instructions.hpp>
#include <isoline/compiler/x86_64/Lowering.hpp>
#include <isoline/compiler/x86_64/Target.hpp>
#include <isoline/runtime/FloatEnvironment.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/testing/StackAddress.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

namespace isoline::execution::testing
{

namespace
{
    /// The bytes the code starts with before its first entry: a caller instrumented by UBSan's function check
    /// reads the 8 bytes before an indirect call's target, which must lie in the mapping (P7's carry: code
    /// memory keeps call targets off a region's first 16 bytes).
    constexpr auto LeadingPadBytes = uint64_t { 16 };

    /// How many functions @p module imports.
    [[nodiscard]] uint32_t importedFunctionsOf(binary::Module const& module) noexcept
    {
        return static_cast<uint32_t>(std::ranges::count_if(module.imports, [](binary::Import const& entry) {
            return entry.kind == binary::ExternKind::Func;
        }));
    }
} // namespace

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

compiler::EntryTrampoline CompiledModule::entry() const noexcept
{
    return reinterpret_cast<compiler::EntryTrampoline>(reinterpret_cast<std::uintptr_t>(at(trampoline)));
}

void const* CompiledModule::at(uint64_t offset) const noexcept
{
    auto const code = sealed->code();
    Require(offset < code.size());
    return code.subspan(static_cast<std::size_t>(offset)).data();
}

std::expected<CompiledModule, compiler::CompileError> compileOne(validate::ValidatedModule const& module,
                                                                 compiler::CompileConfig const& config,
                                                                 ICodeMemory& memory)
{
    auto arena = std::pmr::monotonic_buffer_resource {};
    auto target = compiler::x86_64::X64Target::create(config.target);
    if (!target)
        return std::unexpected { target.error() };
    auto const& limits = config.limits;
    auto const cancellation = compiler::CompileCancellation {};
    auto const index = compiler::translation::ModuleIndex { module.module(), arena };
    auto const imported = importedFunctionsOf(module.module());
    auto const minimums = compiler::optimization::memoryMinimumBytesOf(module, arena);
    auto const functionCount = static_cast<uint32_t>(module.module().functions.size());
    auto buffer = compiler::machine::CodeBuffer { arena };
    auto labels = std::pmr::vector<compiler::machine::CodeLabel> { &arena };
    for ([[maybe_unused]] auto const function: std::views::iota(uint32_t { 0 }, functionCount))
        labels.push_back(buffer.createLabel());
    auto const moduleLabels =
        compiler::machine::ModuleLabels { .functions = labels, .trapExit = buffer.createLabel() };
    auto pad = compiler::x86_64::Assembler { buffer, {}, nullptr };
    while (buffer.offset() < LeadingPadBytes)
        pad.emit(compiler::x86_64::Mnemonic::Ud2, {});
    auto compiled = CompiledModule {};
    auto const allocator = compiler::registerallocation::LinearScanAllocator { limits };
    for (auto const codeIndex: std::views::iota(uint32_t { 0 }, functionCount))
    {
        auto translated = compiler::translation::translate(
            compiler::translation::TranslationInput { .module = module,
                                                      .index = index,
                                                      .codeIndex = codeIndex,
                                                      .mode = config.mode,
                                                      .limits = limits,
                                                      .cancellation = cancellation },
            arena);
        if (!translated)
            return std::unexpected { translated.error() };
        auto passes =
            compiler::optimization::PassContext { .module = module,
                                                  .config = config,
                                                  .cancellation = cancellation,
                                                  .memory = arena,
                                                  .verification =
                                                      compiler::optimization::Verification::AfterEachPass,
                                                  .codeIndex = codeIndex,
                                                  .memoryMinimumBytes = minimums };
        if (auto const optimized =
                compiler::optimization::runPasses(*translated, passes, compiler::optimization::PassTable);
            !optimized)
            return std::unexpected { optimized.error() };
        auto const selection = compiler::machine::SelectionContext { .config = config,
                                                                     .module = module,
                                                                     .codeIndex = codeIndex,
                                                                     .importedFunctions = imported,
                                                                     .cancellation = cancellation,
                                                                     .memory = arena };
        auto function = target->select(*translated, selection);
        if (!function)
            return std::unexpected { function.error() };
        auto const frame =
            allocator.allocate(*function, *target, module.frameCost(codeIndex), cancellation, arena);
        if (!frame)
            return std::unexpected { frame.error() };
        buffer.bind(labels[codeIndex]);
        compiled.functions.push_back(buffer.offset());
        if (auto const encoded = target->encode(*function, *frame, moduleLabels, buffer); !encoded)
            return std::unexpected { encoded.error() };
    }
    compiled.trampoline = buffer.offset();
    target->emitEntryTrampoline(buffer, moduleLabels.trapExit);
    for (auto const typeIndex: module.module().functions)
    {
        compiled.adapters.push_back(buffer.offset());
        target->emitEntryAdapter(module.module().types[typeIndex], buffer);
    }
    if (auto const resolved = buffer.resolve(); !resolved)
        return std::unexpected { resolved.error() };
    auto sealed = memory.seal(std::as_bytes(buffer.bytes()));
    Require(sealed.has_value());
    compiled.sealed = std::move(*sealed);
    return compiled;
}

std::expected<std::unique_ptr<CompiledInstance>, compiler::CompileError> CompiledInstance::create(
    std::shared_ptr<validate::ValidatedModule const> module,
    compiler::CompileConfig const& config,
    InstanceOptions options)
{
    auto memory = PortableCodeMemory {};
    auto code = compileOne(*module, config, memory);
    if (!code)
        return std::unexpected { code.error() };
    return std::unique_ptr<CompiledInstance> { new CompiledInstance {
        std::move(module), std::move(*code), std::move(options) } };
}

CompiledInstance::CompiledInstance(std::shared_ptr<validate::ValidatedModule const> module,
                                   CompiledModule code,
                                   InstanceOptions options):
    _module { std::move(module) },
    _code { std::move(code) },
    _options { std::move(options) },
    _store { _options.limits,
             runtime::StoreServices { .allocator = _allocator,
                                      .pollObserver = _options.observer,
                                      .dispatchRows = runtime::DispatchRows::Kept } },
    _gate { _store }
{
    auto const imports = _options.imports ? _options.imports(_store) : std::vector<runtime::ExternValue> {};
    auto const instantiated = runtime::instantiate(_store, _interpreter, _module, imports);
    Require(instantiated.has_value());
    _instance = *instantiated;
    auto const& instance = _store.moduleInstance(_instance);
    for (auto const address: instance.memories)
        _memories.push_back(&_store.memory(address).view());
    for (auto const address: instance.tables)
        _tables.push_back(&_store.table(address).view());
    for (auto const address: instance.globals)
        _globals.push_back(&_store.global(address).cell);
    _context = compiler::InstanceContext { .memory0 = _memories.empty() ? nullptr : _memories.front(),
                                           .memories = _memories.data(),
                                           .tables = _tables.data(),
                                           .globals = _globals.data(),
                                           .typeIds = instance.types.data(),
                                           .functions = instance.functions.data(),
                                           .moduleInstance = _instance };
    // The instance's own functions, past its imports, run its compiled code
    // (ICodeExecutor::prepareInstance()).
    auto const imported = instance.functions.size() - _code.functions.size();
    for (auto const [codeIndex, offset]: core::views::enumerate(_code.functions))
        _store.bindDispatch(instance.functions[imported + static_cast<std::size_t>(codeIndex)],
                            _instance,
                            _code.at(offset),
                            &_context);
    _hostScratch.resize(hostScratchValuesOf(_store));
    if (_options.interruption == Interruption::Pending)
        _store.interrupter()->interrupt();
    _record = ExecutorRecord { .store = &_store, .executor = &_interpreter, .gate = &_gate };
    _state = compiler::ExecutionState { .interruptFlag = _store.interrupter()->flagForGeneratedCode(),
                                        .stubs = stubEntries().data(),
                                        .dispatch = &_store.dispatchView(),
                                        .hostScratch = _hostScratch.data(),
                                        .hostScratchCapacity = static_cast<uint32_t>(_hostScratch.size()),
                                        .executorState = &_record };
}

void CompiledInstance::lent(
    std::function<void(compiler::ExecutionState&, compiler::InstanceContext const&)> const& work)
{
    auto const scope = runtime::InvocationScope { _store };
    _record.lent = _gate.lend();
    _state.stepsLeft = _record.lent.steps;
    _state.stackValuesLeft = _record.lent.stackValues;
    _state.callDepthLeft = _store.limits().maxCallDepth;
    _state.pending = {};
    work(_state, _context);
    _gate.settle(_record.lent,
                 runtime::Lease { .steps = _state.stepsLeft, .stackValues = _state.stackValuesLeft });
}

std::expected<std::vector<runtime::Value>, runtime::Abrupt> CompiledInstance::call(
    uint32_t codeIndex, std::span<runtime::Value const> arguments)
{
    return callThrough(_code.entry(), codeIndex, arguments);
}

std::expected<std::vector<runtime::Value>, runtime::Abrupt> CompiledInstance::callThrough(
    compiler::EntryTrampoline entry, uint32_t codeIndex, std::span<runtime::Value const> arguments)
{
    auto const& module = _module->module();
    auto const& type = module.types[module.functions.at(codeIndex)];
    Require(arguments.size() == type.params.size());
    auto values = std::vector<runtime::RawValue>(std::max(type.params.size(), type.results.size()));
    for (auto const [index, argument]: core::views::enumerate(arguments))
        values[static_cast<std::size_t>(index)] = runtime::encodeValue(argument);
    auto const scope = runtime::InvocationScope { _store };
    auto const environment = runtime::FloatEnvironmentScope { runtime::libraryRoundingMode() };
    _record.lent = _gate.lend();
    auto const entryValues = _record.lent.stackValues;
    _state.stepsLeft = _record.lent.steps;
    _state.stackValuesLeft = entryValues;
    _state.callDepthLeft = _store.limits().maxCallDepth;
    _state.pending = {};
    auto const grant = _options.nativeStackBytes.value_or(
        requiredNativeStack(compiler::x86_64::X64FrameRule, _store.limits()));
    auto const lowest = (_options.bounds != nullptr ? *_options.bounds : _bounds).lowestUsableAddress();
    if (!lowest)
    {
        // No limit can be set without the thread's end: the entry is refused, nothing ran.
        _gate.settle(_record.lent, _record.lent);
        return std::unexpected { runtime::Abrupt {
            runtime::Trap { runtime::TrapCode::CallStackExhausted } } };
    }
    _state.nativeStackLimit = nativeStackLimitOf(runtime::testing::stackAddress(), grant, *lowest);
    auto const savedEntry = _state.entryStackPointer;
    auto const status = entry(&_state,
                              &_context,
                              _code.at(_code.adapters.at(codeIndex)),
                              _code.at(_code.functions.at(codeIndex)),
                              values.data());
    _state.entryStackPointer = savedEntry;
    if (status == std::to_underlying(compiler::EntryStatus::Pending))
    {
        // The frames the trap exit dropped gave nothing back: the entry's values are what is charged again.
        _state.stackValuesLeft = entryValues;
        _gate.settle(_record.lent,
                     runtime::Lease { .steps = _state.stepsLeft, .stackValues = _state.stackValuesLeft });
        auto const& pending = _state.pending;
        if (pending.kind == compiler::PendingKind::Trap)
        {
            auto subject = pending.subject == compiler::SubjectPresence::Present
                               ? std::optional { static_cast<int64_t>(pending.subjectBits) }
                               : std::nullopt;
            return std::unexpected { runtime::Abrupt { runtime::Trap { pending.code, subject } } };
        }
        Require(pending.kind == compiler::PendingKind::StubAbrupt && _record.abrupt.has_value());
        auto const abrupt = *_record.abrupt;
        _record.abrupt.reset();
        return std::unexpected { abrupt };
    }
    Require(status == std::to_underlying(compiler::EntryStatus::Returned));
    _gate.settle(_record.lent,
                 runtime::Lease { .steps = _state.stepsLeft, .stackValues = _state.stackValuesLeft });
    auto results = std::vector<runtime::Value> {};
    for (auto const [index, result]: core::views::enumerate(type.results))
        results.push_back(runtime::decodeValue(values[static_cast<std::size_t>(index)], result));
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
