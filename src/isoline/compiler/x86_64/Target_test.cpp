// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

#include <isoline/binary/testing/Code.hpp>
#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileLimits.hpp>
#include <isoline/compiler/machine/CodeBuffer.hpp>
#include <isoline/compiler/registerallocation/LinearScan.hpp>
#include <isoline/compiler/registerallocation/testing/AssignmentChecker.hpp>
#include <isoline/compiler/translation/testing/OpcodeSamples.hpp>
#include <isoline/compiler/x86_64/Instructions.hpp>
#include <isoline/compiler/x86_64/Target.hpp>
#include <isoline/compiler/x86_64/testing/Selection.hpp>

namespace
{

using isoline::compiler::CompileErrorCode;
using isoline::compiler::machine::Architecture;
using isoline::compiler::machine::CodeBuffer;
using isoline::compiler::machine::CodeLabel;
using isoline::compiler::machine::Convention;
using isoline::compiler::machine::ModuleLabels;
using isoline::compiler::machine::OperatingSystemAbi;
using isoline::compiler::machine::RegisterClass;
using isoline::compiler::machine::TargetDescription;
using isoline::compiler::translation::testing::everyCompiledOpcode;
using isoline::compiler::translation::testing::Placement;
using isoline::compiler::x86_64::X64Target;
using isoline::compiler::x86_64::testing::AllFeatures;
using isoline::compiler::x86_64::testing::Optimization;
using isoline::compiler::x86_64::testing::SelectionOptions;
using isoline::compiler::x86_64::testing::selectOf;

} // namespace

TEST_CASE("Target.refusesAnotherArchitecture")
{
    auto const arm = X64Target::create(TargetDescription { .architecture = Architecture::Arm64 });
    REQUIRE_FALSE(arm.has_value());
    CHECK(arm.error().code == CompileErrorCode::UnsupportedTarget);
    // An arm64 feature on x86-64 names no instruction it may use.
    auto const cssc =
        X64Target::create(TargetDescription { .architecture = Architecture::X64,
                                              .features = isoline::compiler::machine::CpuFeatures {}.with(
                                                  isoline::compiler::machine::CpuFeature::Cssc) });
    REQUIRE_FALSE(cssc.has_value());
    CHECK(cssc.error().code == CompileErrorCode::UnsupportedTarget);
    for (auto const abi:
         { OperatingSystemAbi::SystemV, OperatingSystemAbi::Windows, OperatingSystemAbi::Apple })
    {
        auto const target =
            X64Target::create(TargetDescription { .architecture = Architecture::X64, .abi = abi });
        REQUIRE(target.has_value());
        CHECK(target->description().abi == abi);
        CHECK(target->allocatable(RegisterClass::General).size() == 12);
        CHECK(target->allocatable(RegisterClass::Float).size() == 16);
        // The host convention its ABI names, and the internal one; the stubs' rows are the host's.
        auto const host = abi == OperatingSystemAbi::Windows ? Convention::Windows64 : Convention::SystemV;
        CHECK(target->conventionRow(host).convention == host);
        CHECK(target->conventionRow(Convention::WasmInternal).convention == Convention::WasmInternal);
        CHECK(&target->conventions().host == &target->conventionRow(host));
    }
}

TEST_CASE("Target.constraintsFollowTheEncodingTable")
{
    auto const target = X64Target::create(TargetDescription {});
    REQUIRE(target.has_value());
    using isoline::compiler::x86_64::Mnemonic;
    using isoline::compiler::x86_64::opcodeOf;
    using isoline::compiler::x86_64::Pseudo;
    // div: its divisor, then rax and rdx fixed and tied; a push names no stack pointer.
    auto const divide = target->constraints(opcodeOf(Mnemonic::Div64R));
    REQUIRE(divide.size() == 3);
    CHECK(divide[1].fixed == 0);
    CHECK(divide[1].role == isoline::compiler::machine::OperandRole::UseDef);
    CHECK(divide[2].fixed == 2);
    CHECK(target->constraints(opcodeOf(Mnemonic::Push64R)).size() == 1);
    // A Wasm call's rows: the target, six General and eight Float arguments, every clobber of the internal
    // convention; a stub's the host's.
    auto const direct = target->constraints(opcodeOf(Pseudo::CallDirect));
    CHECK(direct.size() == 1 + 6 + 8 + 25);
    auto const windows = X64Target::create(TargetDescription { .abi = OperatingSystemAbi::Windows });
    REQUIRE(windows.has_value());
    CHECK(windows->constraints(opcodeOf(Pseudo::CallStub)).size() == 1 + 4 + 13);
    CHECK(target->constraints(opcodeOf(Pseudo::CallStub)).size() == 1 + 6 + 25);
}

TEST_CASE("Target.allocatesAndEncodesEveryOpcodeOnItsRegisterFile")
{
    // Task 10's allocator on x86-64's real register file and real operand rows (R6): every opcode sample,
    // in a function body and in a loop, selected, allocated, its assignment checked independently
    // (AssignmentChecker), encoded and resolved, under both host conventions and both feature levels.
    struct Run
    {
        std::string name;
        SelectionOptions options;
    };
    auto const runs = std::vector<Run> {
        { "System V, baseline", SelectionOptions {} },
        { "System V, every feature, optimized",
          SelectionOptions { .optimization = Optimization::Passes, .features = AllFeatures } },
        { "Microsoft x64, baseline", SelectionOptions { .abi = OperatingSystemAbi::Windows } },
    };
    auto const cancellation = isoline::compiler::CompileCancellation {};
    auto const allocator =
        isoline::compiler::registerallocation::LinearScanAllocator { isoline::compiler::CompileLimits {} };
    for (auto const& run: runs)
        for (auto const placement: { Placement::Body, Placement::InLoop })
            for (auto const& sample: everyCompiledOpcode(placement))
            {
                INFO(run.name << ": " << sample.info->text);
                auto const before = selectOf(sample.module, run.options);
                auto after = selectOf(sample.module, run.options);
                auto& memory = after->translated->memory;
                auto const frameCost = after->translated->module->frameCost(0);
                auto const frame =
                    allocator.allocate(*after->function, *after->target, frameCost, cancellation, memory);
                REQUIRE(frame.has_value());
                auto const checked = isoline::compiler::registerallocation::testing::checkAssignment(
                    *before->function, *after->function, *after->target, *frame, frameCost, memory);
                if (!checked)
                    FAIL_CHECK("assignment fault " << static_cast<int>(checked.error().kind)
                                                   << " at instruction "
                                                   << checked.error().instruction.value_or(~0U) << " operand "
                                                   << checked.error().operand.value_or(~0U));
                auto buffer = CodeBuffer { memory };
                auto const functions = std::vector<CodeLabel> { buffer.createLabel(), buffer.createLabel() };
                auto const labels = ModuleLabels { .functions = functions, .trapExit = buffer.createLabel() };
                auto const encoded = after->target->encode(*after->function, *frame, labels, buffer);
                REQUIRE(encoded.has_value());
                for (auto const label: functions)
                    buffer.bind(label);
                buffer.bind(labels.trapExit);
                CHECK(buffer.resolve().has_value());
                CHECK(buffer.offset() > 0);
            }
}

TEST_CASE("Target.selectsThroughItsSelector")
{
    // The target's select() is its Selector over its own features and conventions.
    auto builder = isoline::compiler::translation::testing::ModuleBuilder {};
    builder.type(isoline::binary::FuncType { .params = { isoline::binary::I32 },
                                             .results = { isoline::binary::I32 } });
    builder.function(0, {}, isoline::binary::testing::localGet(0));
    auto const options = SelectionOptions {};
    auto const expected = selectOf(builder, options);
    auto const translated = isoline::compiler::translation::testing::translateOf(builder);
    auto const description = isoline::compiler::x86_64::testing::descriptionOf(options);
    auto const target = X64Target::create(description);
    REQUIRE(target.has_value());
    auto const config =
        isoline::compiler::CompileConfig { description, isoline::runtime::ExecutionMode::Production };
    auto const cancellation = isoline::compiler::CompileCancellation {};
    auto const context = isoline::compiler::machine::SelectionContext { .config = config,
                                                                        .module = *translated->module,
                                                                        .codeIndex = 0,
                                                                        .importedFunctions = 0,
                                                                        .cancellation = cancellation,
                                                                        .memory = translated->memory };
    auto const selected = target->select(*translated->function, context);
    REQUIRE(selected.has_value());
    CHECK(isoline::compiler::x86_64::testing::namesOf(*selected)
          == isoline::compiler::x86_64::testing::namesOf(*expected->function));
}

TEST_CASE("Target.constraintTablesRebuildAtRuntime")
{
    // The constraint tables are built at compile time; built again at run time, they agree.
    namespace x64 = isoline::compiler::x86_64;
    for (auto const& row: x64::EncodingTable)
    {
        INFO(row.name);
        auto const rebuilt = x64::constraintsOf(row);
        auto const& built = x64::MnemonicConstraintTable[static_cast<std::size_t>(row.mnemonic)];
        CHECK(rebuilt.count == built.count);
        CHECK(std::ranges::equal(std::span { rebuilt.rows }.first(rebuilt.count),
                                 std::span { built.rows }.first(built.count)));
    }
    using Constraints = x64::PseudoConstraints<x64::SystemVConvention>;
    auto lead = std::array { x64::detail::NotRead };
    auto const direct = x64::detail::callRows<Constraints::DirectLayout.size()>(
        lead, &x64::WasmInternalConvention, x64::WasmInternalConvention);
    CHECK(std::ranges::equal(direct, Constraints::CallDirect));
    auto const leadCount = lead.size();
    CHECK(Constraints::wasmCall(leadCount).size() == Constraints::DirectLayout.size());
    CHECK(x64::detail::general(isoline::compiler::machine::OperandRole::Use).registerClass
          == RegisterClass::General);
}

TEST_CASE("Target.codeLimitIsARowPerTarget")
{
    // x86-64's call and jump forms take a rel32: a module's code spans at most 2 GiB − 1, under every ABI.
    // (arm64's 128 MiB row is checked with Arm64Target, P6 Task 9.)
    for (auto const abi:
         { OperatingSystemAbi::SystemV, OperatingSystemAbi::Windows, OperatingSystemAbi::Apple })
    {
        auto const target =
            X64Target::create(TargetDescription { .architecture = Architecture::X64, .abi = abi });
        REQUIRE(target.has_value());
        CHECK(target->codeLimit() == (uint64_t { 1 } << 31U) - 1);
    }
}
