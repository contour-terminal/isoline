// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/Compiler.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory_resource>
#include <new>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#include <isoline/binary/BodyUnits.hpp>
#include <isoline/compiler/FeatureSupport.hpp>
#include <isoline/compiler/machine/CodeBuffer.hpp>
#include <isoline/compiler/registerallocation/LinearScan.hpp>
#include <isoline/runtime/FloatEnvironment.hpp>

namespace isoline::compiler
{

namespace
{
    /// When the passes verify on this build (ISOLINE_COMPILE_VERIFICATION, set by CMake from the build type):
    /// after each pass in a debug build, once after the pipeline in a release one.
    constexpr auto BuildVerification = optimization::Verification::ISOLINE_COMPILE_VERIFICATION;

    /// The bytes the code starts with before its first entry: a caller instrumented by UBSan's function check
    /// reads the 8 bytes before an indirect call's target, which must lie in the mapping (P7's carry: code
    /// memory keeps call targets off a region's first 16 bytes).
    constexpr auto LeadingPadBytes = std::size_t { 16 };

    /// How many functions @p module imports.
    [[nodiscard]] uint32_t importedFunctionsOf(binary::Module const& module) noexcept
    {
        return static_cast<uint32_t>(std::ranges::count_if(module.imports, [](binary::Import const& entry) {
            return entry.kind == binary::ExternKind::Func;
        }));
    }

    /// Cancelled, at @p function when one is being compiled.
    [[nodiscard]] CompileError cancelled(std::optional<uint32_t> function) noexcept
    {
        return CompileError { .code = CompileErrorCode::Cancelled, .function = function };
    }

    /// What a compile builds across its functions; every container draws on the compile's arena.
    struct ModuleState
    {
        ModuleState(validate::ValidatedModule const& module, std::pmr::memory_resource& arena):
            index { module.module(), arena },
            minimums { optimization::memoryMinimumBytesOf(module, arena) },
            buffer { arena },
            labels { &arena }
        {
        }

        translation::ModuleIndex index;
        std::pmr::vector<uint64_t> minimums;
        machine::CodeBuffer buffer;
        std::pmr::vector<machine::CodeLabel> labels;
    };

    /// The arena's refusal as the compile's error, at @p function.
    [[nodiscard]] CompileError refusalOf(runtime::ICountingArena const& arena,
                                         std::optional<uint32_t> function)
    {
        auto const refusal = arena.lastRefusal();
        auto const code = refusal && refusal->reason == runtime::ArenaRefusalReason::Budget
                              ? CompileErrorCode::BudgetExceeded
                              : CompileErrorCode::HostMemoryExhausted;
        return CompileError { .code = code,
                              .function = function,
                              .requested = refusal ? refusal->requested : 0,
                              .limit = arena.budget() };
    }

    /// The imported functions' host-call scratch cells (CompiledCode::hostScratchValues).
    [[nodiscard]] std::vector<uint32_t> hostScratchValuesOf(binary::Module const& module)
    {
        auto values = std::vector<uint32_t> {};
        for (auto const& entry: module.imports)
        {
            if (entry.kind != binary::ExternKind::Func)
                continue;
            auto const& type = module.types[entry.typeIndex];
            values.push_back(static_cast<uint32_t>(std::max(type.params.size(), type.results.size())));
        }
        return values;
    }

    /// The offsets @p offsets as CompiledCode keeps them: within the artifact's cap, so each fits 32 bits.
    [[nodiscard]] std::vector<uint32_t> narrowed(std::span<uint64_t const> offsets)
    {
        auto narrow = std::vector<uint32_t> {};
        narrow.reserve(offsets.size());
        for (auto const offset: offsets)
        {
            Require(offset <= std::numeric_limits<uint32_t>::max());
            narrow.push_back(static_cast<uint32_t>(offset));
        }
        return narrow;
    }

    /// One compile of one module.
    class ModuleCompile
    {
      public:
        ModuleCompile(CompileConfig const& config,
                      machine::ITarget const& target,
                      runtime::ICountingArena& arena,
                      CompileCancellation const& cancellation,
                      validate::ValidatedModule const& module):
            _config { config },
            _target { target },
            _arena { arena },
            _cancellation { cancellation },
            _module { module },
            _cap { artifactCapOf(config.limits, module.sourceBytes()) }
        {
        }

        /// Compiles every function, then the trampoline and the adapters.
        [[nodiscard]] std::expected<CompiledCode, CompileError> run()
        {
            if (auto const checked = checkModule(_module); !checked)
                return std::unexpected { checked.error() };
            try
            {
                auto compiled = compileModule();
                _arena.release(_arena.reserved());
                return compiled;
            }
            catch (std::bad_alloc const&)
            {
                // What was allocated unwound with the throw; what is left of the reservation goes back.
                _arena.release(_arena.reserved());
                return std::unexpected { refusalOf(_arena, _current) };
            }
        }

      private:
        [[nodiscard]] std::expected<CompiledCode, CompileError> compileModule()
        {
            auto const& module = _module.module();
            auto const functionCount = static_cast<uint32_t>(module.code.size());
            auto state = ModuleState { _module, _arena };
            auto& buffer = state.buffer;
            state.labels.reserve(functionCount);
            for ([[maybe_unused]] auto const function: std::views::iota(uint32_t { 0 }, functionCount))
                state.labels.push_back(buffer.createLabel());
            auto const labels =
                machine::ModuleLabels { .functions = state.labels, .trapExit = buffer.createLabel() };
            // Never executed: zeros, which no target needs to decode.
            buffer.emit(std::array<uint8_t, LeadingPadBytes> {});
            auto functions = std::vector<uint64_t> {};
            functions.reserve(functionCount);
            for (auto const codeIndex: std::views::iota(uint32_t { 0 }, functionCount))
            {
                _current = codeIndex;
                if (_cancellation.state() == CancellationState::Cancelled)
                    return std::unexpected { cancelled(codeIndex) };
                buffer.bind(state.labels[codeIndex]);
                functions.push_back(buffer.offset());
                if (auto const compiled = compileFunction(state, labels, codeIndex); !compiled)
                    return std::unexpected { compiled.error() };
                if (auto const capped = checkCap(buffer, codeIndex); !capped)
                    return std::unexpected { capped.error() };
            }
            _current = std::nullopt;
            auto const trampoline = buffer.offset();
            _target.emitEntryTrampoline(buffer, labels.trapExit);
            auto adapters = std::vector<uint64_t> {};
            adapters.reserve(functionCount);
            auto adapterAreas = std::vector<uint64_t> {};
            adapterAreas.reserve(functionCount);
            for (auto const typeIndex: module.functions)
            {
                adapters.push_back(buffer.offset());
                _target.emitEntryAdapter(module.types[typeIndex], buffer);
                adapterAreas.push_back(_target.entryAdapterAreaBytes(module.types[typeIndex]));
            }
            if (auto const capped = checkCap(buffer, std::nullopt); !capped)
                return std::unexpected { capped.error() };
            if (auto const resolved = buffer.resolve(); !resolved)
                return std::unexpected { resolved.error() };
            auto frameCosts = std::vector<uint64_t> {};
            frameCosts.reserve(functionCount);
            for (auto const codeIndex: std::views::iota(uint32_t { 0 }, functionCount))
                frameCosts.push_back(_module.frameCost(codeIndex));
            auto const bytes = std::as_bytes(buffer.bytes());
            return CompiledCode { .config = _config,
                                  .code = std::vector<std::byte>(bytes.begin(), bytes.end()),
                                  .functionOffsets = narrowed(functions),
                                  .adapterOffsets = narrowed(adapters),
                                  .trampolineOffset = narrowed(std::span { &trampoline, 1 }).front(),
                                  .frameCosts = std::move(frameCosts),
                                  .hostScratchValues = hostScratchValuesOf(module),
                                  .adapterAreaBytes = std::move(adapterAreas) };
        }

        /// Compiles the function of @p codeIndex at the buffer's offset, its bound reserved first.
        [[nodiscard]] std::expected<void, CompileError> compileFunction(ModuleState& state,
                                                                        machine::ModuleLabels const& labels,
                                                                        uint32_t codeIndex)
        {
            auto const& source = _module.module().code[codeIndex];
            auto const units = binary::bodyUnits(source);
            if (units > _config.limits.maxFunctionBodyUnits)
                return std::unexpected { CompileError { .code = CompileErrorCode::FunctionTooLarge,
                                                        .function = codeIndex,
                                                        .requested = units,
                                                        .limit = _config.limits.maxFunctionBodyUnits } };
            // The function's bound, charged before any of its work: what is left of the previous one's goes
            // back first, so that a reservation never piles on another.
            _arena.release(_arena.reserved());
            auto const reservation = compileReservationOf(units);
            if (auto const reserved = _arena.reserve(reservation); !reserved)
                return std::unexpected { CompileError { .code = CompileErrorCode::BudgetExceeded,
                                                        .function = codeIndex,
                                                        .requested = reservation,
                                                        .limit = _arena.budget() } };
            auto const input = FunctionInput { .module = _module,
                                               .index = state.index,
                                               .config = _config,
                                               .target = _target,
                                               .cancellation = _cancellation,
                                               .memoryMinimumBytes = state.minimums,
                                               .verification = BuildVerification,
                                               .importedFunctions = importedFunctionsOf(_module.module()),
                                               .codeIndex = codeIndex };
            auto function = selectFunction(input, _arena);
            if (!function)
                return std::unexpected { function.error() };
            auto const allocator = registerallocation::LinearScanAllocator { _config.limits };
            auto const frame =
                allocator.allocate(*function, _target, _module.frameCost(codeIndex), _cancellation, _arena);
            if (!frame)
                return std::unexpected { frame.error() };
            return _target.encode(*function, *frame, labels, state.buffer);
        }

        /// Nothing while the code built so far is within the cap; ArtifactTooLarge at @p function otherwise.
        [[nodiscard]] std::expected<void, CompileError> checkCap(machine::CodeBuffer const& buffer,
                                                                 std::optional<uint32_t> function) const
        {
            if (buffer.offset() <= _cap)
                return {};
            return std::unexpected { CompileError { .code = CompileErrorCode::ArtifactTooLarge,
                                                    .function = function,
                                                    .requested = buffer.offset(),
                                                    .limit = _cap } };
        }

        CompileConfig const& _config;
        machine::ITarget const& _target;
        runtime::ICountingArena& _arena;
        CompileCancellation const& _cancellation;
        validate::ValidatedModule const& _module;
        uint64_t _cap;
        std::optional<uint32_t> _current {}; ///< The function being compiled, which a refusal names.
    };
} // namespace

uint64_t compileReservationOf(uint64_t units) noexcept
{
    constexpr auto Largest = std::numeric_limits<uint64_t>::max();
    if (units > (Largest - CompileFunctionOverheadBytes) / CompileBytesPerBodyByte)
        return Largest;
    return (CompileBytesPerBodyByte * units) + CompileFunctionOverheadBytes;
}

uint64_t artifactCapOf(CompileLimits const& limits, uint64_t sourceBytes) noexcept
{
    constexpr auto Addressable = uint64_t { std::numeric_limits<uint32_t>::max() };
    auto const perByte = uint64_t { limits.artifactBytesPerModuleByte };
    auto const scaled =
        perByte != 0 && sourceBytes > Addressable / perByte ? Addressable : perByte * sourceBytes;
    return std::min(std::max(limits.artifactBytesFloor, scaled), Addressable);
}

std::expected<machine::MachineFunction, CompileError> selectFunction(FunctionInput const& input,
                                                                     std::pmr::memory_resource& memory)
{
    auto translated =
        translation::translate(translation::TranslationInput { .module = input.module,
                                                               .index = input.index,
                                                               .codeIndex = input.codeIndex,
                                                               .mode = input.config.mode,
                                                               .limits = input.config.limits,
                                                               .cancellation = input.cancellation },
                               memory);
    if (!translated)
        return std::unexpected { translated.error() };
    auto passes = optimization::PassContext { .module = input.module,
                                              .config = input.config,
                                              .cancellation = input.cancellation,
                                              .memory = memory,
                                              .verification = input.verification,
                                              .codeIndex = input.codeIndex,
                                              .memoryMinimumBytes = input.memoryMinimumBytes };
    if (auto const optimized = optimization::runPasses(*translated, passes, optimization::PassTable);
        !optimized)
        return std::unexpected { optimized.error() };
    auto const selection = machine::SelectionContext { .config = input.config,
                                                       .module = input.module,
                                                       .codeIndex = input.codeIndex,
                                                       .importedFunctions = input.importedFunctions,
                                                       .cancellation = input.cancellation,
                                                       .memory = memory };
    return input.target.select(*translated, selection);
}

Compiler::Compiler(CompileConfig config,
                   machine::ITarget const& target,
                   runtime::ICountingArena& arena,
                   CompileCancellation const& cancellation):
    _config { config }, _target { &target }, _arena { &arena }, _cancellation { &cancellation }
{
    Require(target.description() == _config.target);
}

std::expected<CompiledCode, CompileError> Compiler::compile(validate::ValidatedModule const& module) const
{
    // Constant folding runs on host float arithmetic: the IEEE defaults, whatever the caller's environment.
    auto const environment = runtime::FloatEnvironmentScope { runtime::libraryRoundingMode() };
    return ModuleCompile { _config, *_target, *_arena, *_cancellation, module }.run();
}

} // namespace isoline::compiler
