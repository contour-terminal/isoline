// SPDX-License-Identifier: Apache-2.0
#pragma once

// A function in machine terms, target-independent: blocks of machine instructions, each an opcode of the
// target's selection table over operands in one per-function pool. Values are virtual registers until the
// register allocator (P5 Task 10) rewrites them to physical ones in place. Control flow keeps the SSA shape
// of the intermediate code: a block has parameters, and each edge out of a block passes arguments to its
// target's parameters, so that the allocator places the edge moves. A value that crosses a back edge does
// so as a block parameter; within a block a virtual register may be redefined (a tied UseDef operand), but
// no value defined in one block is redefined in another. The selector (Task 13) splits critical edges while
// it builds the function, so no block is added after selection (R6).

#include <cstdint>
#include <memory_resource>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/machine/Registers.hpp>

namespace isoline::compiler::machine
{

/// A block of a machine function, numbered densely from 0 in the order they were created; block 0 is
/// the entry. The intermediate code's type, so that one LoopNesting and one DominatorTree serve both.
using intermediate::BlockId;

/// Whether a block is expected to run (linearize() lays a cold one out at the end of its innermost loop).
using intermediate::Hotness;

/// A value of a machine function before register allocation, numbered densely from 0.
enum class VirtualRegister : uint32_t
{
};

/// What kind of register a RegisterName names.
enum class RegisterKind : uint8_t
{
    None,     ///< No register: a memory operand's absent base or index.
    Virtual,  ///< A virtual register, before allocation.
    Physical, ///< A physical register: fixed by the selector, or assigned by the allocator.
};

/// A register an operand names: virtual or physical, with its class.
struct RegisterName
{
    RegisterKind kind = RegisterKind::None;               ///< Which kind it is.
    RegisterClass registerClass = RegisterClass::General; ///< Its class.
    uint32_t number = 0; ///< The VirtualRegister's number, or the PhysicalRegister's number in its class.

    /// The name of @p reg, of @p registerClass.
    [[nodiscard]] static constexpr RegisterName of(VirtualRegister reg, RegisterClass registerClass) noexcept
    {
        return RegisterName { .kind = RegisterKind::Virtual,
                              .registerClass = registerClass,
                              .number = std::to_underlying(reg) };
    }

    /// The name of @p reg.
    [[nodiscard]] static constexpr RegisterName of(PhysicalRegister reg) noexcept
    {
        return RegisterName { .kind = RegisterKind::Physical,
                              .registerClass = reg.registerClass,
                              .number = reg.number };
    }

    /// The virtual register it names (kind Virtual).
    [[nodiscard]] constexpr VirtualRegister virtualRegister() const noexcept
    {
        return VirtualRegister { number };
    }

    /// The physical register it names (kind Physical).
    [[nodiscard]] constexpr PhysicalRegister physicalRegister() const noexcept
    {
        return PhysicalRegister { .registerClass = registerClass, .number = static_cast<uint8_t>(number) };
    }

    /// Whether both name the same register.
    [[nodiscard]] constexpr bool operator==(RegisterName const&) const noexcept = default;
};

/// A register operand, and what the instruction does with it (the role of its target constraint).
struct RegisterOperand
{
    RegisterName name {};                ///< The register.
    OperandRole role = OperandRole::Use; ///< Read, written, both, or written early.

    /// Whether both are the same operand.
    [[nodiscard]] constexpr bool operator==(RegisterOperand const&) const noexcept = default;
};

/// An immediate operand.
struct Immediate
{
    int64_t value = 0; ///< Its value, sign-extended; the encoder checks its width.

    /// Whether both are the same operand.
    [[nodiscard]] constexpr bool operator==(Immediate const&) const noexcept = default;
};

/// How a memory operand's index is widened before it is scaled.
enum class IndexExtension : uint8_t
{
    None,         ///< The register's full width.
    ZeroExtend32, ///< Its low 32 bits, zero-extended (arm64's uxtw; a memory32 index).
    SignExtend32, ///< Its low 32 bits, sign-extended (arm64's sxtw).
};

/// A memory operand: base + extend(index) × scale + displacement. Its registers are read (uses) by the
/// instruction.
struct MemoryOperand
{
    RegisterName base {};     ///< The base register; kind None for none.
    RegisterName index {};    ///< The index register; kind None for none.
    uint8_t scale = 1;        ///< The index's scale: 1, 2, 4 or 8.
    int32_t displacement = 0; ///< The displacement; a larger offset is added by an instruction of its own.
    IndexExtension extension = IndexExtension::None; ///< How the index is widened.

    /// Whether both are the same operand.
    [[nodiscard]] constexpr bool operator==(MemoryOperand const&) const noexcept = default;
};

/// A branch target: a block of the function.
struct LabelOperand
{
    BlockId block {}; ///< The target.

    /// Whether both are the same operand.
    [[nodiscard]] constexpr bool operator==(LabelOperand const&) const noexcept = default;
};

/// A runtime stub a call reaches, by its row of the runtime stub table.
struct StubOperand
{
    uint16_t stub = 0; ///< The stub's row.

    /// Whether both are the same operand.
    [[nodiscard]] constexpr bool operator==(StubOperand const&) const noexcept = default;
};

/// A slot of the function's constant pool (MachineFunction::addConstant()).
struct ConstantOperand
{
    uint32_t slot = 0; ///< The slot's index.

    /// Whether both are the same operand.
    [[nodiscard]] constexpr bool operator==(ConstantOperand const&) const noexcept = default;
};

/// One operand of a machine instruction.
using MachineOperand =
    std::variant<RegisterOperand, Immediate, MemoryOperand, LabelOperand, StubOperand, ConstantOperand>;

/// A run of one of a MachineFunction's pools.
struct PoolRange
{
    uint32_t begin = 0; ///< The index of its first entry.
    uint32_t count = 0; ///< How many entries.

    /// Whether both name the same run.
    [[nodiscard]] constexpr bool operator==(PoolRange const&) const noexcept = default;
};

/// One machine instruction: an opcode of the target's selection table, and its operands in the pool.
struct MachineInstruction
{
    uint16_t opcode = 0;   ///< The target's opcode; ITarget::constraints() describes its operands.
    PoolRange operands {}; ///< Its operands, in the operand pool.
};

/// An edge out of a block: its target, and the arguments it passes to the target's parameters.
struct MachineEdge
{
    BlockId target {};      ///< The block it enters.
    PoolRange arguments {}; ///< Its arguments, in the register pool; one per parameter of the target.
};

/// What MachineFunction::setEdges() takes for an edge: the target and the values it passes.
struct EdgeTo
{
    BlockId target {};                             ///< The block it enters.
    std::span<VirtualRegister const> arguments {}; ///< Its arguments.
};

/// One block: its parameters, its instructions in order (the last its terminator), and its edges out.
struct MachineBlock
{
    PoolRange parameters {};                 ///< The values it defines on entry, in the register pool.
    std::pmr::vector<uint32_t> instructions; ///< Its instructions, as indices into the function's.
    PoolRange edges {};                      ///< Its edges out, in the edge pool; the CFG's successors.
    Hotness hotness = Hotness::Normal;       ///< Whether it is expected to run.
};

/// What kind of place a Location names.
enum class LocationKind : uint8_t
{
    None,     ///< No place: a value the register allocator has not placed.
    Register, ///< A physical register.
    Slot,     ///< An 8-byte stack slot of the frame.
};

/// Where a value lives at a point of an allocated function: a physical register, or a stack slot named by its
/// byte offset from the frame's bottom (the stack pointer every call leaves from, FrameLayout). A slot holds
/// one 64-bit word whatever the class: an integer, a pointer, a Ref or a float's bits.
struct Location
{
    LocationKind kind = LocationKind::None;               ///< Which kind of place.
    RegisterClass registerClass = RegisterClass::General; ///< The class of the value it holds.
    uint32_t index = 0; ///< The register's number in its class, or the slot's byte offset.

    /// The location of @p reg.
    [[nodiscard]] static constexpr Location of(PhysicalRegister reg) noexcept
    {
        return Location { .kind = LocationKind::Register,
                          .registerClass = reg.registerClass,
                          .index = reg.number };
    }

    /// The slot at @p offset bytes from the frame's bottom, holding a value of @p registerClass.
    [[nodiscard]] static constexpr Location slot(RegisterClass registerClass, uint32_t offset) noexcept
    {
        return Location { .kind = LocationKind::Slot, .registerClass = registerClass, .index = offset };
    }

    /// The physical register it names (kind Register).
    [[nodiscard]] constexpr PhysicalRegister physicalRegister() const noexcept
    {
        return PhysicalRegister { .registerClass = registerClass, .number = static_cast<uint8_t>(index) };
    }

    /// Whether both name the same place. The class is what the place holds, not where it is: a slot is one
    /// place whatever class it holds.
    [[nodiscard]] constexpr bool samePlace(Location const& other) const noexcept
    {
        if (kind != other.kind || index != other.index)
            return false;
        return kind == LocationKind::Slot || registerClass == other.registerClass;
    }

    /// Whether both are the same location, class included.
    [[nodiscard]] constexpr bool operator==(Location const&) const noexcept = default;
};

/// One move the register allocator inserts: the value at @p from copied to @p to. Either may be a slot,
/// both included: the encoder lowers a slot-to-slot move without a register of the function's (x86-64:
/// push and pop of the memory operand).
struct Move
{
    Location from {}; ///< Where the value is.
    Location to {};   ///< Where it goes.

    /// Whether both are the same move.
    [[nodiscard]] constexpr bool operator==(Move const&) const noexcept = default;
};

/// A call site of a function, and the outgoing area it needs: its stack arguments and result area, shadow
/// space included (machine::assignCall()'s arguments.memoryBytes plus its results.memoryBytes), as the
/// selector records it (MachineFunction::addCallSite()). The register allocator sizes the frame's outgoing
/// area to the largest.
struct CallSite
{
    uint32_t instruction = 0;   ///< The call instruction, by index.
    uint32_t outgoingBytes = 0; ///< The bytes of outgoing area it needs at the frame's bottom.

    /// Whether both are the same call site.
    [[nodiscard]] constexpr bool operator==(CallSite const&) const noexcept = default;
};

/// What a function's frame came to, as the register allocator decides it and the encoder lays it out. From
/// the bottom (the stack pointer every call leaves from) up: the outgoing area (every call's stack arguments,
/// and the host convention's shadow space, which a callee may write), the spill slots, the callee-saved
/// registers, and at the top what the call into the function pushed (x86-64's return address).
struct FrameLayout
{
    /// The frame's size, from the caller's stack pointer at its call down to the frame's bottom: what the
    /// call pushed included. Within the target's FrameRule, and a multiple of its stack alignment
    /// (checkFrame()).
    uint64_t totalBytes = 0;
    RegisterSet calleeSaved {}; ///< The callee-saved registers the function uses, which its prologue saves.
    uint64_t outgoingBytes = 0; ///< The outgoing area at the frame's bottom, shadow space included.
    uint64_t spillOffset = 0;   ///< Where the spill slots begin, from the frame's bottom.
    uint64_t spillBytes = 0;    ///< How many bytes the spill slots take.

    /// Whether both lay a frame out alike.
    [[nodiscard]] bool operator==(FrameLayout const&) const noexcept = default;
};

/// A function in machine terms. Move-constructible only: every container allocates from its memory, which an
/// assignment could not keep.
class MachineFunction
{
  public:
    /// An empty function, every container of which allocates from @p memory.
    explicit MachineFunction(std::pmr::memory_resource& memory);

    MachineFunction(MachineFunction&&) noexcept = default;
    MachineFunction(MachineFunction const&) = delete;
    MachineFunction& operator=(MachineFunction&&) = delete; // its containers may not share an allocator
    MachineFunction& operator=(MachineFunction const&) = delete;
    ~MachineFunction() = default;

    /// A new virtual register of @p registerClass.
    [[nodiscard]] VirtualRegister createVirtualRegister(RegisterClass registerClass);

    /// How many virtual registers there are.
    [[nodiscard]] uint32_t virtualRegisterCount() const noexcept
    {
        return static_cast<uint32_t>(_registerClasses.size());
    }

    /// The class of @p reg (Require: one of the function's).
    [[nodiscard]] RegisterClass registerClass(VirtualRegister reg) const noexcept;

    /// The name of @p reg, with its class.
    [[nodiscard]] RegisterName name(VirtualRegister reg) const noexcept
    {
        return RegisterName::of(reg, registerClass(reg));
    }

    /// A new, empty block.
    /// @param hotness Whether it is expected to run.
    [[nodiscard]] BlockId createBlock(Hotness hotness);

    /// How many blocks there are.
    [[nodiscard]] uint32_t blockCount() const noexcept { return static_cast<uint32_t>(_blocks.size()); }

    /// @p block's record (Require: one of the function's).
    [[nodiscard]] MachineBlock const& block(BlockId block) const noexcept;

    /// Gives @p block its parameters, once (Require: it has none yet).
    void setParameters(BlockId block, std::span<VirtualRegister const> parameters);

    /// The parameters of @p block.
    [[nodiscard]] std::span<VirtualRegister const> parameters(BlockId block) const noexcept;

    /// Appends an instruction to @p block.
    /// @param block The block.
    /// @param opcode The target's opcode.
    /// @param operands Its operands; each register one of the function's, of its class (Require).
    /// @return Its index.
    uint32_t append(BlockId block, uint16_t opcode, std::span<MachineOperand const> operands);

    /// The instructions of @p block, in order, as indices.
    [[nodiscard]] std::span<uint32_t const> instructions(BlockId block) const noexcept;

    /// How many instructions there are, in every block.
    [[nodiscard]] uint32_t instructionCount() const noexcept
    {
        return static_cast<uint32_t>(_instructions.size());
    }

    /// The instruction at @p index (Require: below instructionCount()).
    [[nodiscard]] MachineInstruction const& instruction(uint32_t index) const noexcept;

    /// The operands of the instruction at @p index.
    [[nodiscard]] std::span<MachineOperand const> operands(uint32_t index) const noexcept;

    /// The operands of the instruction at @p index, to rewrite in place (the register allocator).
    [[nodiscard]] std::span<MachineOperand> operands(uint32_t index) noexcept;

    /// Gives @p block its edges out, once (Require: it has none yet; every target one of the function's).
    void setEdges(BlockId block, std::span<EdgeTo const> edges);

    /// The edges out of @p block: its successors in the control-flow graph, in order.
    [[nodiscard]] std::span<MachineEdge const> edges(BlockId block) const noexcept;

    /// The arguments @p edge passes.
    [[nodiscard]] std::span<VirtualRegister const> arguments(MachineEdge const& edge) const noexcept;

    /// Adds @p bits to the constant pool.
    /// @return The operand that names its slot.
    [[nodiscard]] ConstantOperand addConstant(uint64_t bits);

    /// The constant pool, by slot.
    [[nodiscard]] std::span<uint64_t const> constants() const noexcept { return _constants; }

    /// Records that the instruction at @p instruction is a call needing @p outgoingBytes of outgoing area
    /// (Require: one of the function's instructions).
    void addCallSite(uint32_t instruction, uint32_t outgoingBytes);

    /// The call sites, in the order they were recorded.
    [[nodiscard]] std::span<CallSite const> callSites() const noexcept { return _callSites; }

    /// Gives the instruction at @p instruction the moves that run before it, in order, once (Require: one of
    /// the function's instructions, without moves yet). The register allocator's: its spills, reloads, the
    /// copies into and out of fixed registers, and the edge moves.
    void setMovesBefore(uint32_t instruction, std::span<Move const> moves);

    /// The moves that run before the instruction at @p instruction, in order; none before allocation.
    [[nodiscard]] std::span<Move const> movesBefore(uint32_t instruction) const noexcept;

    /// How many moves the allocator inserted, before every instruction.
    [[nodiscard]] uint32_t moveCount() const noexcept { return static_cast<uint32_t>(_moves.size()); }

    /// What the function allocates from.
    [[nodiscard]] std::pmr::memory_resource& memory() const noexcept { return *_memory; }

  private:
    [[nodiscard]] PoolRange appendRegisters(std::span<VirtualRegister const> registers);
    void requireRegister(RegisterName const& name) const noexcept;

    std::pmr::memory_resource* _memory;
    std::pmr::vector<RegisterClass> _registerClasses;
    std::pmr::vector<MachineBlock> _blocks;
    std::pmr::vector<MachineInstruction> _instructions;
    std::pmr::vector<MachineOperand> _operands;
    std::pmr::vector<VirtualRegister> _registers; ///< Block parameters and edge arguments.
    std::pmr::vector<MachineEdge> _edges;
    std::pmr::vector<uint64_t> _constants;
    std::pmr::vector<CallSite> _callSites;
    std::pmr::vector<PoolRange> _movesBefore; ///< Per instruction, its moves; empty until the allocator ran.
    std::pmr::vector<Move> _moves;
};

/// The control-flow graph of @p function: each block's successors are its edges' targets, in order.
/// @param function The function.
/// @param memory What the graph allocates from.
[[nodiscard]] intermediate::ControlFlowGraph controlFlowGraphOf(MachineFunction const& function,
                                                                std::pmr::memory_resource& memory);

} // namespace isoline::compiler::machine
