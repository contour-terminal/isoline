// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/machine/MachineFunction.hpp>

#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <utility>
#include <variant>

namespace isoline::compiler::machine
{

namespace
{
    /// @p value as an index of one of a function's pools, which stays below 2^32 (Require).
    [[nodiscard]] uint32_t narrow(std::size_t value) noexcept
    {
        Require(value < intermediate::Unnumbered);
        return static_cast<uint32_t>(value);
    }
} // namespace

MachineFunction::MachineFunction(std::pmr::memory_resource& memory):
    _memory { &memory },
    _registerClasses { &memory },
    _blocks { &memory },
    _instructions { &memory },
    _operands { &memory },
    _registers { &memory },
    _edges { &memory },
    _constants { &memory }
{
}

VirtualRegister MachineFunction::createVirtualRegister(RegisterClass registerClass)
{
    auto const reg = VirtualRegister { narrow(_registerClasses.size()) };
    _registerClasses.push_back(registerClass);
    return reg;
}

RegisterClass MachineFunction::registerClass(VirtualRegister reg) const noexcept
{
    auto const index = std::to_underlying(reg);
    Require(index < _registerClasses.size());
    return _registerClasses[index];
}

BlockId MachineFunction::createBlock(Hotness hotness)
{
    auto const block = BlockId { narrow(_blocks.size()) };
    auto& created = _blocks.emplace_back();
    created.hotness = hotness;
    return block;
}

MachineBlock const& MachineFunction::block(BlockId block) const noexcept
{
    auto const index = std::to_underlying(block);
    Require(index < _blocks.size());
    return _blocks[index];
}

PoolRange MachineFunction::appendRegisters(std::span<VirtualRegister const> registers)
{
    auto const begin = narrow(_registers.size());
    for (auto const reg: registers)
        Require(std::to_underlying(reg) < _registerClasses.size());
    _registers.insert(_registers.end(), registers.begin(), registers.end());
    return PoolRange { .begin = begin, .count = narrow(registers.size()) };
}

void MachineFunction::setParameters(BlockId block, std::span<VirtualRegister const> parameters)
{
    Require(this->block(block).parameters.count == 0);
    _blocks[std::to_underlying(block)].parameters = appendRegisters(parameters);
}

std::span<VirtualRegister const> MachineFunction::parameters(BlockId block) const noexcept
{
    auto const range = this->block(block).parameters;
    return std::span { _registers }.subspan(range.begin, range.count);
}

void MachineFunction::requireRegister(RegisterName const& name) const noexcept
{
    if (name.kind == RegisterKind::Virtual)
        Require(registerClass(name.virtualRegister()) == name.registerClass);
    else if (name.kind == RegisterKind::Physical)
        Require(name.number < MaximumRegistersPerClass);
}

uint32_t MachineFunction::append(BlockId block, uint16_t opcode, std::span<MachineOperand const> operands)
{
    Require(std::to_underlying(block) < _blocks.size());
    for (auto const& operand: operands)
    {
        if (auto const* reg = std::get_if<RegisterOperand>(&operand))
        {
            Require(reg->name.kind != RegisterKind::None);
            requireRegister(reg->name);
        }
        else if (auto const* address = std::get_if<MemoryOperand>(&operand))
        {
            for (auto const& part: { address->base, address->index })
            {
                Require(part.kind == RegisterKind::None || part.registerClass == RegisterClass::General);
                requireRegister(part);
            }
        }
    }
    auto const index = narrow(_instructions.size());
    auto const begin = narrow(_operands.size());
    _operands.insert(_operands.end(), operands.begin(), operands.end());
    _instructions.push_back(MachineInstruction {
        .opcode = opcode, .operands = PoolRange { .begin = begin, .count = narrow(operands.size()) } });
    _blocks[std::to_underlying(block)].instructions.push_back(index);
    return index;
}

std::span<uint32_t const> MachineFunction::instructions(BlockId block) const noexcept
{
    return this->block(block).instructions;
}

MachineInstruction const& MachineFunction::instruction(uint32_t index) const noexcept
{
    Require(index < _instructions.size());
    return _instructions[index];
}

std::span<MachineOperand const> MachineFunction::operands(uint32_t index) const noexcept
{
    auto const range = instruction(index).operands;
    return std::span { _operands }.subspan(range.begin, range.count);
}

std::span<MachineOperand> MachineFunction::operands(uint32_t index) noexcept
{
    auto const range = instruction(index).operands;
    return std::span { _operands }.subspan(range.begin, range.count);
}

void MachineFunction::setEdges(BlockId block, std::span<EdgeTo const> edges)
{
    Require(this->block(block).edges.count == 0);
    auto const begin = narrow(_edges.size());
    for (auto const& edge: edges)
    {
        Require(std::to_underlying(edge.target) < _blocks.size());
        _edges.push_back(MachineEdge { .target = edge.target, .arguments = appendRegisters(edge.arguments) });
    }
    _blocks[std::to_underlying(block)].edges = PoolRange { .begin = begin, .count = narrow(edges.size()) };
}

std::span<MachineEdge const> MachineFunction::edges(BlockId block) const noexcept
{
    auto const range = this->block(block).edges;
    return std::span { _edges }.subspan(range.begin, range.count);
}

std::span<VirtualRegister const> MachineFunction::arguments(MachineEdge const& edge) const noexcept
{
    return std::span { _registers }.subspan(edge.arguments.begin, edge.arguments.count);
}

ConstantOperand MachineFunction::addConstant(uint64_t bits)
{
    auto const slot = narrow(_constants.size());
    _constants.push_back(bits);
    return ConstantOperand { .slot = slot };
}

intermediate::ControlFlowGraph controlFlowGraphOf(MachineFunction const& function,
                                                  std::pmr::memory_resource& memory)
{
    auto begins = std::pmr::vector<uint32_t> { &memory };
    auto successors = std::pmr::vector<BlockId> { &memory };
    begins.reserve(std::size_t { function.blockCount() } + 1);
    for (auto const block: std::views::iota(uint32_t { 0 }, function.blockCount()))
    {
        begins.push_back(narrow(successors.size()));
        for (auto const& edge: function.edges(BlockId { block }))
            successors.push_back(edge.target);
    }
    begins.push_back(narrow(successors.size()));
    return intermediate::ControlFlowGraph { std::move(begins), std::move(successors), memory };
}

} // namespace isoline::compiler::machine
