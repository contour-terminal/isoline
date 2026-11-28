// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/x86_64/EncodingSamples.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <memory_resource>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <isoline/compiler/machine/CodeBuffer.hpp>

namespace isoline::compiler::x86_64
{

namespace
{
    using machine::ConstantOperand;
    using machine::LabelOperand;
    using machine::MachineOperand;
    using machine::MemoryOperand;
    using machine::RegisterClass;
    using machine::RegisterName;
    using machine::RegisterOperand;

    /// One value an operand slot takes in the samples.
    struct SlotValue
    {
        MachineOperand operand {};
        uint32_t labelDistance = 0;
    };

    /// The immediates every immediate slot is sampled with, where its kind takes them.
    constexpr auto SampleImmediates = std::to_array<int64_t>({ 0, 1, -1, 127, -128, 2'147'483'647 });

    /// What a 64-bit immediate slot (movabs) takes beyond those: a value of all eight bytes.
    constexpr auto SampleWideImmediates =
        std::to_array<int64_t>({ 0, 1, -1, 127, -128, 2'147'483'647, 0x1234'5678'9abc'def0 });

    /// The rounding modes a rounding slot is sampled with: each of the four, with and without suppressing
    /// the precision exception (bit 3); never bit 2.
    constexpr auto SampleRoundings = std::to_array<int64_t>({ 0, 1, 2, 3, 8, 9, 10, 11 });

    /// The displacements a memory operand is sampled with: none, three that fit disp8, four that need disp32.
    constexpr auto SampleDisplacements = std::to_array<int64_t>({ 0,
                                                                  8,
                                                                  -128,
                                                                  127,
                                                                  128,
                                                                  -129,
                                                                  std::numeric_limits<int32_t>::max(),
                                                                  std::numeric_limits<int32_t>::min() });

    /// Where a label lies past the instruction's end in the samples of a branch, and of a rip-relative
    /// operand.
    constexpr auto SampleBranchDistances = std::to_array<uint32_t>({ 0, 1, 127, 128, 65'536 });
    constexpr auto SampleRipDistances = std::to_array<uint32_t>({ 0, 8, 65'536 });

    constexpr auto IndexScales = std::to_array<uint8_t>({ 1, 2, 4, 8 });

    /// The register names Intel syntax gives a General register's narrower widths.
    constexpr auto General32Names = std::to_array<std::string_view>({ "eax",
                                                                      "ecx",
                                                                      "edx",
                                                                      "ebx",
                                                                      "esp",
                                                                      "ebp",
                                                                      "esi",
                                                                      "edi",
                                                                      "r8d",
                                                                      "r9d",
                                                                      "r10d",
                                                                      "r11d",
                                                                      "r12d",
                                                                      "r13d",
                                                                      "r14d",
                                                                      "r15d" });
    constexpr auto General16Names = std::to_array<std::string_view>({ "ax",
                                                                      "cx",
                                                                      "dx",
                                                                      "bx",
                                                                      "sp",
                                                                      "bp",
                                                                      "si",
                                                                      "di",
                                                                      "r8w",
                                                                      "r9w",
                                                                      "r10w",
                                                                      "r11w",
                                                                      "r12w",
                                                                      "r13w",
                                                                      "r14w",
                                                                      "r15w" });
    constexpr auto General8Names = std::to_array<std::string_view>({ "al",
                                                                     "cl",
                                                                     "dl",
                                                                     "bl",
                                                                     "spl",
                                                                     "bpl",
                                                                     "sil",
                                                                     "dil",
                                                                     "r8b",
                                                                     "r9b",
                                                                     "r10b",
                                                                     "r11b",
                                                                     "r12b",
                                                                     "r13b",
                                                                     "r14b",
                                                                     "r15b" });

    /// The condition-code suffixes llvm-mc prints, by ConditionCode.
    constexpr auto ConditionSuffixes = std::to_array<std::string_view>(
        { "o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g" });
    static_assert(ConditionSuffixes.size() == ConditionCodeCount);

    RegisterName generalName(std::size_t number)
    {
        return RegisterName::of(machine::PhysicalRegister { .registerClass = RegisterClass::General,
                                                            .number = static_cast<uint8_t>(number) });
    }

    /// Every register of a register slot's file.
    std::vector<SlotValue> registerValues(OperandKind kind)
    {
        auto const registerClass =
            kind == OperandKind::Vector ? RegisterClass::Float : RegisterClass::General;
        auto values = std::vector<SlotValue> {};
        for (auto const number: std::views::iota(std::size_t { 0 }, RegistersPerFile))
            values.push_back(
                { .operand = RegisterOperand {
                      .name = RegisterName::of(machine::PhysicalRegister {
                          .registerClass = registerClass, .number = static_cast<uint8_t>(number) }) } });
        return values;
    }

    /// Every addressing mode a memory slot is sampled with: each base at each displacement (the rsp and r12
    /// bases among them, and rbp and r13 with none), each index at each scale over a varying base and
    /// displacement, and rip at each distance.
    std::vector<SlotValue> memoryValues()
    {
        auto values = std::vector<SlotValue> {};
        for (auto const base: std::views::iota(std::size_t { 0 }, RegistersPerFile))
            for (auto const displacement: SampleDisplacements)
                values.push_back(
                    { .operand = MemoryOperand { .base = generalName(base), .displacement = displacement } });
        auto rotation = std::size_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, RegistersPerFile))
        {
            if (index == static_cast<std::size_t>(Gpr::Rsp))
                continue;
            for (auto const scale: IndexScales)
            {
                // The base runs through every register before the displacement moves on, so that each base,
                // rbp and r13 among them, meets an index with no displacement.
                values.push_back({ .operand = MemoryOperand {
                                       .base = generalName(rotation % RegistersPerFile),
                                       .index = generalName(index),
                                       .scale = scale,
                                       .displacement = SampleDisplacements[(rotation / RegistersPerFile)
                                                                           % SampleDisplacements.size()] } });
                ++rotation;
            }
        }
        for (auto const distance: SampleRipDistances)
            values.push_back({ .operand = ConstantOperand { .slot = 0 }, .labelDistance = distance });
        return values;
    }

    /// The immediates of @p kind's slot: those of @p candidates it takes.
    std::vector<SlotValue> immediateValues(OperandKind kind, std::span<int64_t const> candidates)
    {
        auto const [lowest, highest] = [kind]() -> std::pair<int64_t, int64_t> {
            switch (kind)
            {
                case OperandKind::Signed8: return { -128, 127 };
                case OperandKind::Unsigned8: return { 0, 255 };
                default: return { std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max() };
            }
        }();
        auto values = std::vector<SlotValue> {};
        for (auto const value: candidates)
            if (value >= lowest && value <= highest)
                values.push_back({ .operand = machine::Immediate { .value = value } });
        return values;
    }

    /// The values the samples give a slot of @p kind.
    std::vector<SlotValue> valuesOf(OperandKind kind)
    {
        if (isRegisterKind(kind))
            return registerValues(kind);
        if (isMemoryKind(kind))
            return memoryValues();
        if (kind == OperandKind::Rounding)
            return immediateValues(kind, SampleRoundings);
        auto values = std::vector<SlotValue> {};
        if (kind == OperandKind::Condition)
            for (auto const code: std::views::iota(std::size_t { 0 }, ConditionCodeCount))
                values.push_back({ .operand = machine::Immediate { .value = static_cast<int64_t>(code) } });
        else if (kind == OperandKind::Label)
            for (auto const distance: SampleBranchDistances)
                values.push_back({ .operand = LabelOperand {}, .labelDistance = distance });
        else
            values = immediateValues(kind,
                                     kind == OperandKind::Signed64
                                         ? std::span<int64_t const> { SampleWideImmediates }
                                         : std::span<int64_t const> { SampleImmediates });
        return values;
    }

    /// The sample of @p mnemonic taking value @p picks[i] of each slot's @p values[i].
    EncodingSample sampleOf(Mnemonic mnemonic,
                            std::span<std::vector<SlotValue> const> values,
                            std::span<std::size_t const> picks)
    {
        auto sample =
            EncodingSample { .mnemonic = mnemonic, .operandCount = static_cast<uint8_t>(values.size()) };
        for (auto const slot: std::views::iota(std::size_t { 0 }, values.size()))
        {
            auto const& value = values[slot][picks[slot]];
            sample.operands[slot] = value.operand;
            sample.labelDistance = std::max(sample.labelDistance, value.labelDistance);
        }
        return sample;
    }

    std::string_view registerNameOf(OperandKind kind, RegisterName const& name)
    {
        auto const number = static_cast<std::size_t>(name.number) % RegistersPerFile;
        switch (kind)
        {
            case OperandKind::General8: return General8Names[number];
            case OperandKind::General16: return General16Names[number];
            case OperandKind::General32: return General32Names[number];
            default: return registerRow(name.physicalRegister()).name;
        }
    }

    std::string_view sizePrefixOf(OperandKind kind)
    {
        switch (kind)
        {
            case OperandKind::Memory8: return "byte ptr ";
            case OperandKind::Memory16: return "word ptr ";
            case OperandKind::Memory32: return "dword ptr ";
            case OperandKind::Memory64: return "qword ptr ";
            case OperandKind::Memory128: return "xmmword ptr ";
            default: return "";
        }
    }

    /// `[base + scale*index ± displacement]`, as llvm-mc prints it: a scale of 1 unwritten, no zero
    /// displacement, `[rip + d]` for a rip-relative operand.
    std::string addressText(MachineOperand const& operand, uint32_t labelDistance)
    {
        if (std::holds_alternative<ConstantOperand>(operand))
            return labelDistance == 0 ? std::string { "[rip]" } : std::format("[rip + {}]", labelDistance);
        auto const& address = std::get<MemoryOperand>(operand);
        auto text = std::string { "[" };
        auto separator = std::string_view {};
        if (address.base.kind == machine::RegisterKind::Physical)
        {
            text += registerNameOf(OperandKind::General64, address.base);
            separator = " + ";
        }
        if (address.index.kind == machine::RegisterKind::Physical)
        {
            text += separator;
            if (address.scale != 1)
                text += std::format("{}*", address.scale);
            text += registerNameOf(OperandKind::General64, address.index);
            separator = " + ";
        }
        if (address.displacement != 0 || separator.empty())
        {
            if (separator.empty())
                text += std::format("{}", address.displacement);
            else if (address.displacement < 0)
                text += std::format(" - {}", -address.displacement);
            else
                text += std::format(" + {}", address.displacement);
        }
        return text + "]";
    }

    /// The text of one explicit operand of @p kind.
    std::string operandText(OperandKind kind, MachineOperand const& operand, uint32_t labelDistance)
    {
        if (isRegisterKind(kind))
            return std::string { registerNameOf(kind, std::get<RegisterOperand>(operand).name) };
        if (isMemoryKind(kind))
            return std::string { sizePrefixOf(kind) } + addressText(operand, labelDistance);
        if (kind == OperandKind::Label)
            return std::format("{}", labelDistance);
        auto const value = std::get<machine::Immediate>(operand).value;
        switch (kind)
        {
            case OperandKind::Condition:
                return std::string { ConditionSuffixes[static_cast<std::size_t>(value)] };
            case OperandKind::Unsigned8:
            case OperandKind::Rounding: return std::format("{}", static_cast<uint64_t>(value) & 0xffU);
            case OperandKind::Unsigned32:
                return std::format("{}", static_cast<uint64_t>(value) & 0xffff'ffffU);
            default: return std::format("{}", value);
        }
    }
} // namespace

std::vector<EncodingSample> samplesFor(Mnemonic mnemonic)
{
    auto const& row = encodingRow(mnemonic);
    auto values = std::vector<std::vector<SlotValue>> {};
    for (auto const& slot: explicitSlots(row))
        values.push_back(valuesOf(slot.kind));

    auto total = std::size_t { 1 };
    auto widest = std::size_t { 1 };
    for (auto const& slotValues: values)
    {
        total *= slotValues.size();
        widest = std::max(widest, slotValues.size());
    }
    // A combination is a number, the last slot's value varying fastest; each is sampled at most once.
    auto picks = std::vector<std::size_t>(values.size());
    auto samples = std::vector<EncodingSample> {};
    auto taken = std::set<std::size_t> {};
    auto const take = [&](std::size_t number) {
        taken.insert(number);
        for (auto const slot: std::views::iota(std::size_t { 0 }, values.size()) | std::views::reverse)
        {
            picks[slot] = number % values[slot].size();
            number /= values[slot].size();
        }
        samples.push_back(sampleOf(mnemonic, values, picks));
    };
    if (total <= MaximumSamplesPerRow)
    {
        for (auto const number: std::views::iota(std::size_t { 0 }, total))
            take(number);
        return samples;
    }
    // Every value of every slot once, side by side; then a stride through the other combinations for the
    // rest, each taken one past the last when the stride lands on one already sampled.
    for (auto const step: std::views::iota(std::size_t { 0 }, widest))
    {
        auto number = std::size_t { 0 };
        for (auto const& slotValues: values)
            number = (number * slotValues.size()) + (step % slotValues.size());
        if (!taken.contains(number))
            take(number);
    }
    auto const rest = MaximumSamplesPerRow - samples.size();
    for (auto const step: std::views::iota(std::size_t { 0 }, rest))
    {
        auto number = step * total / rest;
        while (taken.contains(number))
            number = (number + 1) % total;
        take(number);
    }
    return samples;
}

std::string intendedText(EncodingSample const& sample)
{
    auto const& row = encodingRow(sample.mnemonic);
    auto const slots = explicitSlots(row);
    auto const& text = row.intelTemplate;
    auto rendered = std::string {};
    auto position = std::size_t { 0 };
    while (position < text.size())
    {
        if (text[position] == '{')
        {
            auto const operand = static_cast<std::size_t>(text[position + 1] - '0');
            rendered += operandText(slots[operand].kind, sample.operands[operand], sample.labelDistance);
            position += 3;
            continue;
        }
        rendered += text[position];
        ++position;
    }
    return rendered;
}

std::expected<std::vector<uint8_t>, EncodeError> encodeSample(EncodingSample const& sample)
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    auto buffer = machine::CodeBuffer { memory };
    auto const label = std::to_array({ buffer.createLabel() });
    auto const labels = EncodeLabels { .blocks = label, .constants = label };
    if (auto const encoded = encode(sample.mnemonic, sample.explicitOperands(), labels, buffer); !encoded)
        return std::unexpected { encoded.error() };
    auto const length = buffer.offset();
    auto const padding = std::vector<uint8_t>(sample.labelDistance, uint8_t { 0 });
    buffer.emit(padding);
    buffer.bind(label[0]);
    // A sample's label lies at most SampleBranchDistances' 65'536 bytes past it: within any rel32's reach.
    auto const resolved = buffer.resolve();
    Require(resolved.has_value());
    auto const bytes = buffer.bytes().first(length);
    return std::vector<uint8_t> { bytes.begin(), bytes.end() };
}

std::string hexOf(std::span<uint8_t const> bytes)
{
    auto text = std::string {};
    for (auto const byte: bytes)
    {
        if (!text.empty())
            text += ' ';
        text += std::format("{:02x}", byte);
    }
    return text;
}

std::expected<std::string, EncodeError> sampleLine(EncodingSample const& sample)
{
    auto const bytes = encodeSample(sample);
    if (!bytes)
        return std::unexpected { bytes.error() };
    return std::format("{}\t{}\t{}", hexOf(*bytes), intendedText(sample), encodingRow(sample.mnemonic).name);
}

} // namespace isoline::compiler::x86_64
