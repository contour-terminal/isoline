// SPDX-License-Identifier: Apache-2.0
#pragma once

// What the x86-64 selection, lowering and target suites share: a module's function translated and selected
// for a target description, verified by the machine verifier, and the names of what it selected. Test
// support, linked into no shipped binary.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileLimits.hpp>
#include <isoline/compiler/machine/CodeBuffer.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/MachineVerifier.hpp>
#include <isoline/compiler/machine/Target.hpp>
#include <isoline/compiler/machine/TargetDescription.hpp>
#include <isoline/compiler/optimization/PassTable.hpp>
#include <isoline/compiler/registerallocation/LinearScan.hpp>
#include <isoline/compiler/translation/testing/Translation.hpp>
#include <isoline/compiler/x86_64/Instructions.hpp>
#include <isoline/compiler/x86_64/Lowering.hpp>
#include <isoline/compiler/x86_64/Selector.hpp>
#include <isoline/compiler/x86_64/Target.hpp>

namespace isoline::compiler::x86_64::testing
{

/// Every CPU feature a selection row requires.
inline constexpr auto AllFeatures = machine::CpuFeatures {}
                                        .with(machine::CpuFeature::Sse41)
                                        .with(machine::CpuFeature::Popcnt)
                                        .with(machine::CpuFeature::Lzcnt)
                                        .with(machine::CpuFeature::Bmi1);

/// Whether a test optimizes a function before selecting it.
enum class Optimization : uint8_t
{
    None,   ///< Its translation as it is.
    Passes, ///< PassTable's passes first, verified after each: what produces guardAddress and
            ///< inBoundsAddress.
};

/// Whether a test removes instructions before selecting, so that selection sees Removed ones.
enum class Removal : uint8_t
{
    None,       ///< Nothing removed.
    UnusedPure, ///< Every Pure instruction whose results are all unused, left in place as Removed.
};

/// How a test selects.
struct SelectionOptions
{
    Removal removal = Removal::None;                ///< Whether unused instructions are removed first.
    Optimization optimization = Optimization::None; ///< Whether the passes run first.
    machine::CpuFeatures features {};               ///< The target's features.
    SpectreGuard spectre = SpectreGuard::Enabled;   ///< The guard.
    machine::OperatingSystemAbi abi = machine::OperatingSystemAbi::SystemV; ///< The host ABI.
    translation::testing::Options translation {};                           ///< How the function translates.
    /// Applied to the translated function before selection: what builds IR no translation emits.
    std::function<void(intermediate::Function&)> rewrite {};
};

/// The x86-64 description of @p options.
[[nodiscard]] inline machine::TargetDescription descriptionOf(SelectionOptions const& options) noexcept
{
    return machine::TargetDescription { .architecture = machine::Architecture::X64,
                                        .abi = options.abi,
                                        .features = options.features };
}

/// How many functions @p module imports.
[[nodiscard]] inline uint32_t importedFunctionsOf(binary::Module const& module) noexcept
{
    return static_cast<uint32_t>(std::ranges::count_if(
        module.imports, [](binary::Import const& entry) { return entry.kind == binary::ExternKind::Func; }));
}

/// A function translated and selected, with the arena and module it lives on.
struct Selected
{
    std::unique_ptr<translation::testing::Translated> translated; ///< The module and its IR function.
    std::optional<X64Target> target {};                           ///< The target it was selected for.
    std::optional<machine::MachineFunction> function {};          ///< The machine function.
    SelectionStatistics statistics {};                            ///< The rows it selected.
    uint32_t codeIndex = 0;                                       ///< The function's code index.
};

/// Selects function @p options.translation.codeIndex of @p builder's module for @p options; the function
/// must translate, select and pass the machine verifier.
inline std::unique_ptr<Selected> selectOf(translation::testing::ModuleBuilder const& builder,
                                          SelectionOptions const& options = {})
{
    auto selected = std::make_unique<Selected>();
    selected->translated = translation::testing::translateOf(builder, options.translation);
    REQUIRE(selected->translated->function.has_value());
    auto target = X64Target::create(descriptionOf(options));
    REQUIRE(target.has_value());
    selected->target.emplace(std::move(*target));
    auto const config = CompileConfig { descriptionOf(options), options.translation.mode, options.spectre };
    auto const cancellation = CompileCancellation {};
    auto const& module = *selected->translated->module;
    auto const context =
        machine::SelectionContext { .config = config,
                                    .module = module,
                                    .codeIndex = options.translation.codeIndex,
                                    .importedFunctions = importedFunctionsOf(module.module()),
                                    .cancellation = cancellation,
                                    .memory = selected->translated->memory };
    if (options.optimization == Optimization::Passes)
    {
        auto const minimums = optimization::memoryMinimumBytesOf(module, selected->translated->memory);
        auto passContext =
            optimization::PassContext { .module = module,
                                        .config = config,
                                        .cancellation = cancellation,
                                        .memory = selected->translated->memory,
                                        .verification = optimization::Verification::AfterEachPass,
                                        .codeIndex = options.translation.codeIndex,
                                        .memoryMinimumBytes = minimums };
        auto const optimized =
            optimization::runPasses(*selected->translated->function, passContext, optimization::PassTable);
        REQUIRE(optimized.has_value());
    }
    if (options.rewrite)
        options.rewrite(*selected->translated->function);
    if (options.removal == Removal::UnusedPure)
    {
        auto& function = *selected->translated->function;
        auto uses = std::vector<uint32_t>(function.valueCount(), 0);
        for (auto const index: std::views::iota(uint32_t { 0 }, function.instructionCount()))
            for (auto const value: function.operands(function.instruction(index).operands))
                ++uses[std::to_underlying(value)];
        for (auto const index: std::views::iota(uint32_t { 0 }, function.instructionCount()))
        {
            auto const& instruction = function.instruction(index);
            auto const results = function.operands(instruction.results);
            if (intermediate::effectOf(instruction.operation) == intermediate::Effect::Pure
                && !results.empty() && std::ranges::all_of(results, [&](auto value) {
                       return uses[std::to_underlying(value)] == 0;
                   }))
                function.remove(index);
        }
    }
    auto selector = Selector { options.features, selected->target->conventions() };
    auto function = selector.select(*selected->translated->function, context);
    REQUIRE(function.has_value());
    auto const verified = machine::verifyMachineFunction(*function, *selected->target);
    INFO(selected->translated->text());
    REQUIRE(verified.has_value());
    selected->statistics = selector.statistics();
    selected->function.emplace(std::move(*function));
    selected->codeIndex = options.translation.codeIndex;
    return selected;
}

/// @p selected's function allocated, encoded and resolved, two functions and the trap exit bound as labels.
/// @return The rows it encoded to, in order.
[[nodiscard]] inline std::pmr::vector<TraceEntry> encodedOf(Selected& selected)
{
    auto& memory = selected.translated->memory;
    auto const cancellation = CompileCancellation {};
    auto& function = *selected.function;
    auto const allocator = registerallocation::LinearScanAllocator { CompileLimits {} };
    auto const frame = allocator.allocate(function,
                                          *selected.target,
                                          selected.translated->module->frameCost(selected.codeIndex),
                                          cancellation,
                                          memory);
    REQUIRE(frame.has_value());
    auto buffer = machine::CodeBuffer { memory };
    auto const functions = std::vector<machine::CodeLabel> { buffer.createLabel(), buffer.createLabel() };
    auto const labels = machine::ModuleLabels { .functions = functions, .trapExit = buffer.createLabel() };
    auto trace = std::pmr::vector<TraceEntry> { &memory };
    auto const encoded =
        encodeFunction(function, *frame, selected.target->conventions(), labels, buffer, &trace, memory);
    REQUIRE(encoded.has_value());
    for (auto const label: functions)
        buffer.bind(label);
    buffer.bind(labels.trapExit);
    REQUIRE(buffer.resolve().has_value());
    return trace;
}

/// The name of machine opcode @p opcode: its encoding row's, or its pseudo-instruction's.
[[nodiscard]] inline std::string_view opcodeName(uint16_t opcode) noexcept
{
    if (auto const mnemonic = mnemonicOf(opcode))
        return encodingRow(*mnemonic).name;
    return nameOf(*pseudoOf(opcode));
}

/// The names of @p block's instructions, in order; every block's, in block order, without @p block.
[[nodiscard]] inline std::vector<std::string_view> namesOf(
    machine::MachineFunction const& function, std::optional<machine::BlockId> block = std::nullopt)
{
    auto names = std::vector<std::string_view> {};
    auto const visit = [&](machine::BlockId visited) {
        for (auto const index: function.instructions(visited))
            names.push_back(opcodeName(function.instruction(index).opcode));
    };
    if (block)
        visit(*block);
    else
        for (auto const index: std::views::iota(uint32_t { 0 }, function.blockCount()))
            visit(machine::BlockId { index });
    return names;
}

/// The name of a traced row: its form's, and for a jcc, cmovcc or setcc the condition after a dot ("Jcc.ne").
[[nodiscard]] inline std::string_view traceName(TraceEntry const& entry) noexcept
{
    static constexpr auto Jcc = std::to_array<std::string_view>({ "Jcc.o",
                                                                  "Jcc.no",
                                                                  "Jcc.b",
                                                                  "Jcc.ae",
                                                                  "Jcc.e",
                                                                  "Jcc.ne",
                                                                  "Jcc.be",
                                                                  "Jcc.a",
                                                                  "Jcc.s",
                                                                  "Jcc.ns",
                                                                  "Jcc.p",
                                                                  "Jcc.np",
                                                                  "Jcc.l",
                                                                  "Jcc.ge",
                                                                  "Jcc.le",
                                                                  "Jcc.g" });
    static constexpr auto Cmov64 = std::to_array<std::string_view>({ "Cmovcc64RR.o",
                                                                     "Cmovcc64RR.no",
                                                                     "Cmovcc64RR.b",
                                                                     "Cmovcc64RR.ae",
                                                                     "Cmovcc64RR.e",
                                                                     "Cmovcc64RR.ne",
                                                                     "Cmovcc64RR.be",
                                                                     "Cmovcc64RR.a",
                                                                     "Cmovcc64RR.s",
                                                                     "Cmovcc64RR.ns",
                                                                     "Cmovcc64RR.p",
                                                                     "Cmovcc64RR.np",
                                                                     "Cmovcc64RR.l",
                                                                     "Cmovcc64RR.ge",
                                                                     "Cmovcc64RR.le",
                                                                     "Cmovcc64RR.g" });
    static constexpr auto Cmov32 = std::to_array<std::string_view>({ "Cmovcc32RR.o",
                                                                     "Cmovcc32RR.no",
                                                                     "Cmovcc32RR.b",
                                                                     "Cmovcc32RR.ae",
                                                                     "Cmovcc32RR.e",
                                                                     "Cmovcc32RR.ne",
                                                                     "Cmovcc32RR.be",
                                                                     "Cmovcc32RR.a",
                                                                     "Cmovcc32RR.s",
                                                                     "Cmovcc32RR.ns",
                                                                     "Cmovcc32RR.p",
                                                                     "Cmovcc32RR.np",
                                                                     "Cmovcc32RR.l",
                                                                     "Cmovcc32RR.ge",
                                                                     "Cmovcc32RR.le",
                                                                     "Cmovcc32RR.g" });
    static constexpr auto Setcc = std::to_array<std::string_view>({ "Setcc8R.o",
                                                                    "Setcc8R.no",
                                                                    "Setcc8R.b",
                                                                    "Setcc8R.ae",
                                                                    "Setcc8R.e",
                                                                    "Setcc8R.ne",
                                                                    "Setcc8R.be",
                                                                    "Setcc8R.a",
                                                                    "Setcc8R.s",
                                                                    "Setcc8R.ns",
                                                                    "Setcc8R.p",
                                                                    "Setcc8R.np",
                                                                    "Setcc8R.l",
                                                                    "Setcc8R.ge",
                                                                    "Setcc8R.le",
                                                                    "Setcc8R.g" });
    if (!entry.condition)
        return encodingRow(entry.mnemonic).name;
    auto const index = static_cast<std::size_t>(*entry.condition);
    switch (entry.mnemonic)
    {
        case Mnemonic::Jcc: return Jcc[index];
        case Mnemonic::Cmovcc64RR: return Cmov64[index];
        case Mnemonic::Cmovcc32RR: return Cmov32[index];
        default: return Setcc[index];
    }
}

/// The names of the rows of @p trace, in order.
[[nodiscard]] inline std::vector<std::string_view> namesOf(std::span<TraceEntry const> trace)
{
    auto names = std::vector<std::string_view> {};
    for (auto const& entry: trace)
        names.push_back(traceName(entry));
    return names;
}

/// Where @p needle occurs in @p names as a contiguous run; none when it does not.
[[nodiscard]] inline std::optional<std::size_t> findRun(std::span<std::string_view const> names,
                                                        std::span<std::string_view const> needle)
{
    auto const found = std::ranges::search(names, needle);
    if (found.empty())
        return std::nullopt;
    return static_cast<std::size_t>(found.begin() - names.begin());
}

/// @p names joined by spaces, for a failure's message.
[[nodiscard]] inline std::string joined(std::span<std::string_view const> names)
{
    auto text = std::string {};
    for (auto const name: names)
    {
        if (!text.empty())
            text += ' ';
        text += name;
    }
    return text;
}

} // namespace isoline::compiler::x86_64::testing
