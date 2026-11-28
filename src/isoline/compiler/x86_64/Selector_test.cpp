// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/translation/NumericRules.hpp>
#include <isoline/compiler/translation/testing/OpcodeSamples.hpp>
#include <isoline/compiler/x86_64/Selector.hpp>
#include <isoline/compiler/x86_64/testing/Selection.hpp>
#include <isoline/runtime/Interrupt.hpp>

namespace
{

using isoline::binary::FuncType;
using isoline::binary::ValueType;
using isoline::binary::testing::code;
using isoline::binary::testing::i32Const;
using isoline::binary::testing::i64Const;
using isoline::binary::testing::localGet;
using isoline::binary::testing::memoryAccess;
using isoline::compiler::SpectreGuard;
using isoline::compiler::intermediate::OperandType;
using isoline::compiler::intermediate::Operation;
using isoline::compiler::translation::numericOperationOf;
using isoline::compiler::translation::testing::Bytes;
using isoline::compiler::translation::testing::everyCompiledOpcode;
using isoline::compiler::translation::testing::ModuleBuilder;
using isoline::compiler::translation::testing::opcodeBytes;
using isoline::compiler::x86_64::Emission;
using isoline::compiler::x86_64::encodingRow;
using isoline::compiler::x86_64::isSelectionTableComplete;
using isoline::compiler::x86_64::Mnemonic;
using isoline::compiler::x86_64::Pattern;
using isoline::compiler::x86_64::RowsByOperation;
using isoline::compiler::x86_64::SelectionRow;
using isoline::compiler::x86_64::SelectionRowCount;
using isoline::compiler::x86_64::SelectionRowId;
using isoline::compiler::x86_64::SelectionTable;
using isoline::compiler::x86_64::testing::AllFeatures;
using isoline::compiler::x86_64::testing::joined;
using isoline::compiler::x86_64::testing::namesOf;
using isoline::compiler::x86_64::testing::Optimization;
using isoline::compiler::x86_64::testing::SelectionOptions;
using isoline::compiler::x86_64::testing::selectOf;
using MachineCpuFeatures = isoline::compiler::machine::CpuFeatures;

/// The value type of an operand type.
ValueType valueTypeOf(OperandType type)
{
    switch (type)
    {
        case OperandType::I32: return isoline::binary::I32;
        case OperandType::I64: return isoline::binary::I64;
        case OperandType::F32: return isoline::binary::F32;
        default: return isoline::binary::F64;
    }
}

/// The full-width load of an operand type, at offset 0 of the address in local 2.
Bytes loadOf(OperandType type)
{
    auto const opcode = [type]() -> uint8_t {
        switch (type)
        {
            case OperandType::I32: return 0x28;
            case OperandType::I64: return 0x29;
            case OperandType::F32: return 0x2A;
            default: return 0x2B;
        }
    }();
    return code({ localGet(2), memoryAccess(opcode, 0, 0) });
}

/// A module of one function (param @p first, @p second, i32) with @p body, a memory and a table.
ModuleBuilder moduleOf(ValueType first, ValueType second, Bytes const& body)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { first, second, isoline::binary::I32 }, .results = {} });
    builder.function(0, {}, body);
    builder.memory(isoline::compiler::translation::testing::memoryOf(isoline::binary::AddressType::I32));
    return builder;
}

/// One example of selection: a module and how it is selected.
struct Example
{
    std::string name;         ///< What it is.
    ModuleBuilder module;     ///< Its module; function 0 is selected.
    SelectionOptions options; ///< How.
};

/// Every opcode sample of the translation suite at @p features, and every numeric operation of two operands
/// with its right operand a local, a constant and a load, with its comparisons into a branch and a select.
std::vector<Example> examples()
{
    auto result = std::vector<Example> {};
    for (auto const features: { MachineCpuFeatures {}, AllFeatures })
        for (auto& sample: everyCompiledOpcode())
            result.push_back(Example { .name = std::string { sample.info->text },
                                       .module = std::move(sample.module),
                                       .options = SelectionOptions { .features = features } });
    for (auto const& info: isoline::binary::OpcodeTable)
    {
        auto const operation = numericOperationOf(info.opcode);
        if (!operation)
            continue;
        auto const operands = isoline::compiler::intermediate::operandSignatureRow(
                                  isoline::compiler::intermediate::operationRow(*operation).operands)
                                  .fixed();
        auto const name = std::string { info.text };
        auto const op = opcodeBytes(info.opcode);
        if (operands.size() == 1)
        {
            auto const type = valueTypeOf(operands[0]);
            result.push_back(Example { .name = name + " of a local",
                                       .module = moduleOf(type, type, code({ localGet(0), op, { 0x1A } })),
                                       .options = {} });
            continue;
        }
        if (operands.size() != 2 || operands[0] != operands[1])
            continue;
        auto const type = valueTypeOf(operands[0]);
        auto const isInteger = operands[0] == OperandType::I32 || operands[0] == OperandType::I64;
        result.push_back(
            Example { .name = name + " of two locals",
                      .module = moduleOf(type, type, code({ localGet(0), localGet(1), op, { 0x1A } })),
                      .options = {} });
        result.push_back(Example {
            .name = name + " of a load",
            .module = moduleOf(type, type, code({ localGet(0), loadOf(operands[0]), op, { 0x1A } })),
            .options = {} });
        if (!isInteger)
            continue;
        auto const constant = operands[0] == OperandType::I32 ? i32Const(7) : i64Const(-7);
        result.push_back(
            Example { .name = name + " of a constant",
                      .module = moduleOf(type, type, code({ localGet(0), constant, op, { 0x1A } })),
                      .options = {} });
        if (isoline::compiler::intermediate::operationRow(*operation).result
            == isoline::compiler::intermediate::ResultRule::I32)
        {
            // A comparison: into an if's branch and into a select, with a local and with a constant.
            for (auto const& right: { localGet(1), constant })
            {
                result.push_back(Example {
                    .name = name + " into a branch",
                    .module = moduleOf(type, type, code({ localGet(0), right, op, { 0x04, 0x40, 0x0B } })),
                    .options = {} });
                result.push_back(Example {
                    .name = name + " into a select",
                    .module =
                        moduleOf(type,
                                 type,
                                 code({ localGet(2), localGet(2), localGet(0), right, op, { 0x1B, 0x1A } })),
                    .options = {} });
            }
        }
    }
    for (auto const& eqz: { Bytes { 0x45 }, Bytes { 0x50 } })
    {
        auto const type = eqz[0] == 0x45 ? isoline::binary::I32 : isoline::binary::I64;
        result.push_back(
            Example { .name = "eqz into a branch",
                      .module = moduleOf(type, type, code({ localGet(0), eqz, { 0x04, 0x40, 0x0B } })),
                      .options = {} });
        result.push_back(Example {
            .name = "eqz into a select",
            .module =
                moduleOf(type, type, code({ localGet(2), localGet(2), localGet(0), eqz, { 0x1B, 0x1A } })),
            .options = {} });
    }
    // Bounds-check elimination's two results: an access a check of the same index dominates (guardAddress
    // under SpectreGuard::Enabled), and a constant index within the minimum (inBoundsAddress).
    auto const i32 = isoline::binary::I32;
    result.push_back(Example { .name = "a dominated access",
                               .module = moduleOf(i32,
                                                  i32,
                                                  code({ localGet(2),
                                                         memoryAccess(0x28, 0, 0),
                                                         { 0x1A },
                                                         localGet(2),
                                                         memoryAccess(0x2D, 0, 2),
                                                         { 0x1A } })),
                               .options = SelectionOptions { .optimization = Optimization::Passes } });
    result.push_back(
        Example { .name = "a constant access",
                  .module = moduleOf(i32, i32, code({ i32Const(16), memoryAccess(0x28, 0, 0), { 0x1A } })),
                  .options = SelectionOptions { .optimization = Optimization::Passes } });
    // A loop's back edge polls its countdown; a Deterministic f32.add is canonicalized; memory 1's base and
    // bound come through the instance's views; an imported function is called through its dispatch row; and
    // an instruction a pass removed (left in place) selects nothing.
    result.push_back(
        Example { .name = "a loop",
                  .module = moduleOf(i32, i32, code({ { 0x03, 0x40 }, localGet(0), { 0x0D, 0x00, 0x0B } })),
                  .options = {} });
    auto const f32 = isoline::binary::F32;
    result.push_back(Example {
        .name = "a Deterministic f32.add",
        .module = moduleOf(f32, f32, code({ localGet(0), localGet(1), { 0x92, 0x1A } })),
        .options =
            SelectionOptions { .translation = { .mode = isoline::runtime::ExecutionMode::Deterministic } } });
    auto memories = moduleOf(i32, i32, code({ localGet(2), { 0x28, 0x42, 0x01, 0x00, 0x1A } }));
    memories.memory(isoline::compiler::translation::testing::memoryOf(isoline::binary::AddressType::I32));
    result.push_back(
        Example { .name = "an access of memory 1", .module = std::move(memories), .options = {} });
    auto imports = ModuleBuilder {};
    imports.type(FuncType { .params = { i32, i32, i32 }, .results = {} });
    imports.type(FuncType { .params = { i32 }, .results = { i32 } });
    imports.importFunction("host", "f", 1);
    imports.function(0, {}, code({ localGet(0), isoline::binary::testing::call(0), { 0x1A } }));
    result.push_back(Example { .name = "a call of an import", .module = std::move(imports), .options = {} });
    result.push_back(Example {
        .name = "an unused constant, removed",
        .module = moduleOf(i32, i32, code({ i32Const(3), { 0x1A } })),
        .options = SelectionOptions { .removal = isoline::compiler::x86_64::testing::Removal::UnusedPure } });
    // A Ref global read and written: its RawValue cell's two words; arguments past the internal convention's
    // registers read from the caller's frame; and a dispatched call of a wide signature, Refs and stack
    // arguments and results in its host-call path, under either guard.
    auto const externRef = isoline::binary::ExternRef;
    auto const f64 = isoline::binary::F64;
    auto refGlobal = ModuleBuilder {};
    refGlobal.type(FuncType {});
    refGlobal.global(
        isoline::binary::GlobalType { .type = externRef, .mutability = isoline::binary::Mutability::Var },
        { 0xD0, 0x6F });
    refGlobal.function(0, {}, code({ { 0x23, 0x00, 0x24, 0x00 } }));
    result.push_back(
        Example { .name = "a Ref global read and written", .module = std::move(refGlobal), .options = {} });
    auto wideParams = std::vector<isoline::binary::ValueType>(8, i32);
    wideParams.insert(wideParams.end(), 10, f64);
    auto incoming = ModuleBuilder {};
    incoming.type(FuncType { .params = wideParams, .results = {} });
    incoming.function(0, {}, code({ localGet(7), { 0x1A }, localGet(17), { 0x1A } }));
    result.push_back(
        Example { .name = "arguments on the stack", .module = std::move(incoming), .options = {} });
    for (auto const abi: { isoline::compiler::machine::OperatingSystemAbi::SystemV,
                           isoline::compiler::machine::OperatingSystemAbi::Windows })
        for (auto const spectre: { SpectreGuard::Enabled, SpectreGuard::Disabled })
        {
            auto params = std::vector<isoline::binary::ValueType>(7, i32);
            params.push_back(externRef);
            params.insert(params.end(), 9, f64);
            params.push_back(f32);
            auto const results =
                std::vector<isoline::binary::ValueType> { externRef, i32, f64, externRef, f32, f64, f32 };
            auto arguments = Bytes {};
            for ([[maybe_unused]] auto const index: std::views::iota(0, 7))
                arguments = code({ arguments, i32Const(1) });
            arguments = code({ arguments, { 0xD0, 0x6F } });
            for ([[maybe_unused]] auto const index: std::views::iota(0, 9))
                arguments = code({ arguments, { 0x44, 0, 0, 0, 0, 0, 0, 0, 0 } });
            arguments = code({ arguments, { 0x43, 0, 0, 0, 0 } });
            auto dispatched = ModuleBuilder {};
            dispatched.type(FuncType {});
            dispatched.type(FuncType { .params = params, .results = results });
            dispatched.table(
                isoline::compiler::translation::testing::tableOf(isoline::binary::AddressType::I32));
            dispatched.function(0,
                                {},
                                code({ arguments,
                                       i32Const(0),
                                       { 0x11, 0x01, 0x00 },
                                       { 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A, 0x1A } }));
            result.push_back(Example { .name = "a wide dispatched call",
                                       .module = std::move(dispatched),
                                       .options = SelectionOptions { .spectre = spectre, .abi = abi } });
        }
    // The entry poll and charge where the prologue cannot run them: after another poll, after another
    // instruction, in another block. No translation emits that; the rewrite builds it.
    auto late = ModuleBuilder {};
    late.type(FuncType {});
    late.function(0, {}, {});
    result.push_back(Example {
        .name = "an entry poll and charge where the prologue cannot run them",
        .module = std::move(late),
        .options = SelectionOptions { .rewrite = [](isoline::compiler::intermediate::Function& function) {
            using isoline::compiler::intermediate::Type;
            using isoline::runtime::PollSite;
            auto replacement = isoline::compiler::intermediate::Function { function.memory() };
            auto builder = isoline::compiler::intermediate::Builder { replacement };
            auto const pollAt = [&](PollSite site) {
                std::ignore = builder.emit(Operation::PollStored,
                                           {},
                                           {},
                                           std::array { static_cast<uint64_t>(std::to_underlying(site)) });
            };
            auto const charge = [&] {
                std::ignore = builder.emit(Operation::EnterFrame, {}, {}, std::array { uint64_t { 2 } });
            };
            auto const entry = builder.createBlock();
            auto const next = builder.createBlock();
            builder.switchTo(entry);
            pollAt(PollSite::LoopBackEdge);
            charge();
            std::ignore = builder.emit(
                Operation::I32Const, std::array { Type::I32 }, {}, std::array { uint64_t { 1 } });
            pollAt(PollSite::FunctionEntry);
            charge();
            builder.jump(next, {});
            builder.switchTo(next);
            pollAt(PollSite::FunctionEntry);
            charge();
            std::ignore = builder.emit(Operation::LeaveFrame, {}, {}, std::array { uint64_t { 2 } });
            builder.ret({});
            function = std::move(replacement);
        } } });
    // A branch to two cold blocks, neither of which follows it: jcc, then jmp.
    result.push_back(
        Example { .name = "a branch to two cold blocks",
                  .module = moduleOf(i32, i32, code({ localGet(0), { 0x04, 0x40, 0x00, 0x05, 0x00, 0x0B } })),
                  .options = {} });
    // Results: in registers, and past them in the caller's result area; a select of floats; a narrow load
    // that cannot fold into a wider operation.
    auto returns = ModuleBuilder {};
    returns.type(FuncType { .params = { i32, f64 }, .results = { i32 } });
    returns.type(FuncType { .params = { i32, f64 }, .results = { i32, f64, externRef, i32, f64, i32 } });
    returns.function(0, {}, localGet(0));
    returns.function(
        1, {}, code({ localGet(0), localGet(1), { 0xD0, 0x6F }, localGet(0), localGet(1), localGet(0) }));
    result.push_back(Example { .name = "a result in a register", .module = returns, .options = {} });
    result.push_back(Example { .name = "results in the caller's area",
                               .module = returns,
                               .options = SelectionOptions { .translation = { .codeIndex = 1 } } });
    result.push_back(Example {
        .name = "a select of floats",
        .module = moduleOf(f64, i32, code({ localGet(0), localGet(0), localGet(1), { 0x1B, 0x1A } })),
        .options = {} });
    result.push_back(Example {
        .name = "a narrow load beside an add",
        .module =
            moduleOf(i32, i32, code({ localGet(0), localGet(2), memoryAccess(0x2D, 0, 0), { 0x6A, 0x1A } })),
        .options = {} });
    return result;
}

/// The forms @p emitter names: what the row's selection must contain.
std::vector<std::string_view> formsOf(Emission const& emitter)
{
    auto forms = std::vector<std::string_view> {};
    if (emitter.form != Mnemonic::Ud2)
        forms.push_back(encodingRow(emitter.form).name);
    return forms;
}

} // namespace

TEST_CASE("Selector.tableIsComplete")
{
    // Rows in Operation order, each operation's last row its baseline (also static_asserted).
    CHECK(isSelectionTableComplete());
    // And refuses a table out of order, an operation without rows, and a baseline that needs a feature.
    using isoline::compiler::x86_64::RowRange;
    using isoline::compiler::x86_64::RowsByOperation;
    auto const ranges = std::vector<RowRange>(RowsByOperation.begin(), RowsByOperation.end());
    auto swapped = std::vector<SelectionRow>(SelectionTable.begin(), SelectionTable.end());
    std::swap(swapped[0], swapped[1]);
    CHECK_FALSE(isSelectionTableComplete(swapped, ranges));
    auto emptied = ranges;
    emptied[0].end = emptied[0].begin;
    CHECK_FALSE(isSelectionTableComplete(SelectionTable, emptied));
    auto gated = std::vector<SelectionRow>(SelectionTable.begin(), SelectionTable.end());
    gated[ranges[0].end - 1U].requirement = isoline::compiler::machine::CpuFeature::Sse41;
    CHECK_FALSE(isSelectionTableComplete(gated, ranges));
    auto names = std::set<std::string_view> {};
    for (auto const& row: SelectionTable)
    {
        INFO(row.name);
        CHECK(names.insert(row.name).second);
        CHECK(static_cast<std::size_t>(row.row) == static_cast<std::size_t>(&row - SelectionTable.data()));
    }
    for (auto const operation:
         std::views::iota(std::size_t { 0 }, isoline::compiler::intermediate::OperationCount))
    {
        auto const& range = RowsByOperation[operation];
        INFO(isoline::compiler::intermediate::nameOf(static_cast<Operation>(operation)));
        REQUIRE(range.begin < range.end);
        auto const& baseline = SelectionTable[range.end - 1];
        CHECK_FALSE(baseline.requirement.has_value());
        CHECK(baseline.pattern == Pattern::Any);
        for (auto const index: std::views::iota(range.begin, range.end))
            CHECK(SelectionTable[index].root == static_cast<Operation>(operation));
    }
}

TEST_CASE("Selector.everyRowIsExercised")
{
    // Every row is selected by at least one example; and whenever a row is selected, the function holds the
    // form its emitter names. Every selected function passes the machine verifier (selectOf()).
    auto hits = std::vector<uint64_t>(SelectionRowCount, 0);
    for (auto& example: examples())
    {
        INFO(example.name);
        auto const selected = selectOf(example.module, example.options);
        auto const names = namesOf(*selected->function);
        // It allocates and encodes too: every row's forms take the operands its emitter gives them.
        CHECK_FALSE(isoline::compiler::x86_64::testing::encodedOf(*selected).empty());
        for (auto const& row: SelectionTable)
        {
            auto const rowHits = selected->statistics.hitsOf(row.row);
            if (rowHits == 0)
                continue;
            hits[static_cast<std::size_t>(row.row)] += rowHits;
            INFO(row.name << ": " << joined(names));
            for (auto const form: formsOf(row.emitter))
            {
                auto const holdsForm =
                    std::ranges::contains(names, form)
                    || (row.emitter.alternate != Mnemonic::Ud2
                        && std::ranges::contains(names, encodingRow(row.emitter.alternate).name));
                CHECK(holdsForm);
            }
        }
    }
    for (auto const& row: SelectionTable)
    {
        INFO("row never selected: " << row.name);
        CHECK(hits[static_cast<std::size_t>(row.row)] != 0);
    }
}

TEST_CASE("Selector.foldsSingleUseLoadOnlyAcrossAllowedEffects")
{
    auto const i32 = isoline::binary::I32;
    auto const add = Bytes { 0x6A };
    auto const load = code({ localGet(1), memoryAccess(0x28, 0, 0) });
    SECTION("nothing between: the load is add's memory operand")
    {
        auto const selected = selectOf(moduleOf(i32, i32, code({ localGet(0), load, add, { 0x1A } })));
        auto const names = namesOf(*selected->function);
        INFO(joined(names));
        CHECK(std::ranges::contains(names, std::string_view { "Add32RM" }));
        CHECK_FALSE(std::ranges::contains(names, std::string_view { "Add32RR" }));
        CHECK(selected->statistics.hitsOf(SelectionRowId::I32AddMemory) == 1);
    }
    SECTION("a trapping division between (MayTrap): still folded")
    {
        auto const divide = code({ localGet(0), localGet(2), { 0x6E, 0x1A } }); // i32.div_u, drop
        auto const selected =
            selectOf(moduleOf(i32, i32, code({ localGet(0), load, divide, add, { 0x1A } })));
        auto const names = namesOf(*selected->function);
        INFO(joined(names));
        CHECK(std::ranges::contains(names, std::string_view { "Add32RM" }));
    }
    SECTION("a store between (WritesMemory): a separate load")
    {
        auto const store = code({ localGet(2), i32Const(0), memoryAccess(0x36, 0, 0) });
        auto const selected = selectOf(moduleOf(i32, i32, code({ localGet(0), load, store, add, { 0x1A } })));
        auto const names = namesOf(*selected->function);
        INFO(joined(names));
        CHECK_FALSE(std::ranges::contains(names, std::string_view { "Add32RM" }));
        CHECK(std::ranges::contains(names, std::string_view { "Mov32RM" }));
        CHECK(std::ranges::contains(names, std::string_view { "Add32RR" }));
    }
    SECTION("a call between (Call): a separate load")
    {
        auto builder =
            moduleOf(i32, i32, code({ localGet(0), load, isoline::binary::testing::call(1), add, { 0x1A } }));
        builder.type(FuncType {});
        builder.function(1, {}, {});
        auto const selected = selectOf(builder);
        auto const names = namesOf(*selected->function);
        INFO(joined(names));
        CHECK_FALSE(std::ranges::contains(names, std::string_view { "Add32RM" }));
    }
    SECTION("a load of two uses is not folded")
    {
        auto builder =
            moduleOf(i32, i32, code({ localGet(0), load, { 0x22, 0x01 }, add, localGet(1), { 0x6A, 0x1A } }));
        auto const selected = selectOf(builder);
        auto const names = namesOf(*selected->function);
        INFO(joined(names));
        CHECK_FALSE(std::ranges::contains(names, std::string_view { "Add32RM" }));
    }
}

TEST_CASE("Selector.featureGatedRowsFallBack")
{
    struct Case
    {
        std::string_view name;   ///< The instruction.
        Bytes body;              ///< Its function body.
        SelectionRowId gated;    ///< The row its feature enables.
        SelectionRowId baseline; ///< The row it falls back to.
        std::string_view form;   ///< The gated row's instruction.
    };
    auto const i32 = isoline::binary::I32;
    auto const f32 = isoline::binary::F32;
    auto const cases = std::vector<Case> {
        { "i32.popcnt",
          code({ localGet(0), { 0x69, 0x1A } }),
          SelectionRowId::I32PopcntPopcnt,
          SelectionRowId::I32Popcnt,
          "Popcnt32RR" },
        { "i32.clz",
          code({ localGet(0), { 0x67, 0x1A } }),
          SelectionRowId::I32ClzLzcnt,
          SelectionRowId::I32Clz,
          "Lzcnt32RR" },
        { "i32.ctz",
          code({ localGet(0), { 0x68, 0x1A } }),
          SelectionRowId::I32CtzTzcnt,
          SelectionRowId::I32Ctz,
          "Tzcnt32RR" },
        { "f32.nearest",
          code({ localGet(0), { 0x90, 0x1A } }),
          SelectionRowId::F32NearestSse41,
          SelectionRowId::F32Nearest,
          "RoundssRRI" },
    };
    for (auto const& entry: cases)
    {
        INFO(entry.name);
        auto const type = entry.name.starts_with("f32") ? f32 : i32;
        auto const with =
            selectOf(moduleOf(type, type, entry.body), SelectionOptions { .features = AllFeatures });
        auto const without = selectOf(moduleOf(type, type, entry.body));
        CHECK(with->statistics.hitsOf(entry.gated) == 1);
        CHECK(with->statistics.hitsOf(entry.baseline) == 0);
        CHECK(without->statistics.hitsOf(entry.gated) == 0);
        CHECK(without->statistics.hitsOf(entry.baseline) == 1);
        CHECK(std::ranges::contains(namesOf(*with->function), entry.form));
        CHECK_FALSE(std::ranges::contains(namesOf(*without->function), entry.form));
    }
    // Without SSE4.1, a rounding calls the FloatRound stub.
    auto const baseline = selectOf(moduleOf(f32, f32, code({ localGet(0), { 0x8D, 0x1A } })));
    CHECK(std::ranges::contains(namesOf(*baseline->function), std::string_view { "call-stub" }));
}

TEST_CASE("Selector.spectreGuardFollowsConfig")
{
    // checkMemory's cmova replaces the effective pointer (base + index + offset, a lea) by 0, after the
    // trap's ja: only under SpectreGuard::Enabled.
    auto const i32 = isoline::binary::I32;
    auto const body = code({ localGet(2), memoryAccess(0x28, 0, 8), { 0x1A } });
    auto const run = [&](SpectreGuard spectre) {
        return namesOf(
            *selectOf(moduleOf(i32, i32, body), SelectionOptions { .spectre = spectre })->function);
    };
    auto const enabled = run(SpectreGuard::Enabled);
    auto const disabled = run(SpectreGuard::Disabled);
    INFO(joined(enabled) << "\n" << joined(disabled));
    auto const guardEnabled = std::vector<std::string_view> { "Cmp64RR", "trap-if", "Cmovcc64RR", "Mov32RM" };
    auto const guardDisabled = std::vector<std::string_view> { "Cmp64RR", "trap-if", "Mov32RM" };
    CHECK(isoline::compiler::x86_64::testing::findRun(enabled, guardEnabled).has_value());
    CHECK_FALSE(std::ranges::contains(disabled, std::string_view { "Cmovcc64RR" }));
    CHECK(isoline::compiler::x86_64::testing::findRun(disabled, guardDisabled).has_value());
}

TEST_CASE("Selector.wideConstantsStayInRegisters")
{
    // A 64-bit comparison's or operation's constant past a sign-extended 32 bits is no immediate: it is
    // materialized, and the register form compares it (the operands' type decides, not the result's I32).
    auto const i64 = isoline::binary::I64;
    for (auto const& op: { Bytes { 0x53 }, Bytes { 0x7D } }) // i64.lt_s, i64.sub
    {
        auto const selected =
            selectOf(moduleOf(i64, i64, code({ localGet(0), i64Const(int64_t { 1 } << 40), op, { 0x1A } })));
        auto const names = namesOf(*selected->function);
        INFO(joined(names));
        CHECK(std::ranges::contains(names, std::string_view { "Mov64RI" }));
        CHECK_FALSE(std::ranges::contains(names, std::string_view { "Cmp64RI" }));
        CHECK((std::ranges::contains(names, std::string_view { "Cmp64RR" })
               || std::ranges::contains(names, std::string_view { "Sub64RR" })));
    }
}

TEST_CASE("Selector.stopsWhenCancelled")
{
    // One count of instructions across the phases: a function of N additions polls N times counting uses, N
    // deciding and N emitting (and some more for its entry), so a cancelled compile stops in the phase
    // where the count crosses the interval.
    constexpr auto Interval = 65'536;
    for (auto const additions: { Interval + 10, (Interval / 2) + 10, (Interval / 3) + 10 })
    {
        INFO(additions << " additions");
        auto body = Bytes {};
        body.reserve(static_cast<std::size_t>(additions) * 4);
        for ([[maybe_unused]] auto const index: std::views::iota(0, additions))
            body.insert(body.end(), { 0x20, 0x00, 0x20, 0x00, 0x6A, 0x21, 0x00 });
        auto builder = ModuleBuilder {};
        builder.type(FuncType { .params = { isoline::binary::I32 }, .results = {} });
        builder.function(0, {}, body);
        auto const translated = isoline::compiler::translation::testing::translateOf(builder);
        REQUIRE(translated->function.has_value());
        auto const options = SelectionOptions {};
        auto const target = isoline::compiler::x86_64::X64Target::create(
            isoline::compiler::x86_64::testing::descriptionOf(options));
        REQUIRE(target.has_value());
        auto const config =
            isoline::compiler::CompileConfig { isoline::compiler::x86_64::testing::descriptionOf(options),
                                               isoline::runtime::ExecutionMode::Production,
                                               SpectreGuard::Enabled };
        auto cancellation = isoline::compiler::CompileCancellation {};
        cancellation.cancel();
        auto const context = isoline::compiler::machine::SelectionContext { .config = config,
                                                                            .module = *translated->module,
                                                                            .codeIndex = 0,
                                                                            .importedFunctions = 0,
                                                                            .cancellation = cancellation,
                                                                            .memory = translated->memory };
        auto selector = isoline::compiler::x86_64::Selector { options.features, target->conventions() };
        auto const selected = selector.select(*translated->function, context);
        REQUIRE_FALSE(selected.has_value());
        CHECK(selected.error().code == isoline::compiler::CompileErrorCode::Cancelled);
    }
}

namespace
{

/// Every row's emission, built again at run time from Selection.def's Emitter column.
std::vector<Emission> emissionsAtRunTime()
{
    using namespace isoline::compiler::x86_64::emitters;
    using isoline::compiler::x86_64::EmitterKind;
    using enum isoline::compiler::x86_64::EmitterKind;
    using enum isoline::compiler::x86_64::ConditionCode;
    using enum isoline::compiler::x86_64::Sequence;
    using enum isoline::compiler::x86_64::OperandOrder;
    using enum isoline::compiler::x86_64::RoundingMode;
    using enum isoline::compiler::FloatRoundOperation;
    auto emissions = std::vector<Emission> {};
#define ISOLINE_SELECTION(rowName, rowRoot, rowPattern, rowRequires, rowCost, rowEmitter) \
    emissions.push_back(rowEmitter);
#include <isoline/compiler/x86_64/Selection.def>
#undef ISOLINE_SELECTION
    return emissions;
}

} // namespace

TEST_CASE("Selector.emittersSpellTheirRows")
{
    // The Emitter column's helpers, run at run time, give what the table holds; and every row is named.
    auto const emissions = emissionsAtRunTime();
    REQUIRE(emissions.size() == SelectionTable.size());
    for (auto const& row: SelectionTable)
    {
        INFO(row.name);
        CHECK(emissions[static_cast<std::size_t>(row.row)] == row.emitter);
        CHECK(isoline::compiler::x86_64::nameOf(row.row) == row.name);
    }
}
