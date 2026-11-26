// SPDX-License-Identifier: Apache-2.0
#include <isoline/interp/ReferenceInterpreter.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <variant>
#include <vector>

#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/interp/Numerics.hpp>
#include <isoline/interp/Simd.hpp>
#include <isoline/runtime/Segment.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::interp
{

namespace
{
    using binary::AddressType;
    using binary::Extension;
    using binary::Instruction;
    using binary::MemoryAccess;
    using binary::MemoryAccessKind;
    using binary::Opcode;
    using binary::ValueKind;
    using binary::VectorShape;
    using runtime::F32;
    using runtime::F64;
    using runtime::fits;
    using runtime::FuncAddr;
    using runtime::FunctionInstance;
    using runtime::I32;
    using runtime::I64;
    using runtime::ModuleInstance;
    using runtime::Ref;
    using runtime::RefKind;
    using runtime::Store;
    using runtime::Trap;
    using runtime::trap;
    using runtime::TrapCode;
    using runtime::V128;
    using runtime::Value;

    /// The outcome of executing one instruction: nothing, or the trap execution ends with.
    using Step = std::expected<void, Trap>;

    /// How many entries (operands, locals and labels, over all frames) the stacks may hold before
    /// a call or a block entry traps with CallStackExhausted, as the spec lets an implementation do
    /// when a resource runs out. Store::maxCallDepth() alone would let deep calls of functions with
    /// many locals (up to the decoder's 50000 each), or with a call inside many nested blocks (one
    /// label each per frame), claim gigabytes; this bounds the stacks to about 100 MiB.
    ///
    /// The budget is checked before each call and each block entry, so the stacks may overshoot it
    /// by what one step adds: one function's parameters and locals, or the operands one frame
    /// pushes between two block entries. Both are bounded by the decoder's limits on one function.
    constexpr auto MaxStackValues = std::size_t { 1 } << 22;

    /// A trap that names an index: call_indirect's table element.
    ///
    /// Trap::subject is 32 bits wide because every table P1 decodes takes i32 addresses, so the index
    /// a call_indirect pops always fits; a table64 (memory64) widens the subject rather than
    /// truncating the index here.
    [[nodiscard]] std::unexpected<Trap> trapNaming(TrapCode code, uint64_t subject) noexcept
    {
        Require(subject <= std::numeric_limits<uint32_t>::max());
        return std::unexpected { Trap { code, static_cast<uint32_t>(subject) } };
    }

    // --- Values -------------------------------------------------------------------------------

    /// The value of a constant instruction (i32/i64/f32/f64/v128.const) of @p module, or nullopt
    /// for any other.
    [[nodiscard]] std::optional<Value> constantOf(binary::Module const& module,
                                                  Instruction const& instruction) noexcept
    {
        switch (instruction.opcode)
        {
            case Opcode::I32Const: return I32 { static_cast<uint32_t>(instruction.c) };
            case Opcode::I64Const: return I64 { instruction.c };
            case Opcode::F32Const: return F32 { static_cast<uint32_t>(instruction.c) };
            case Opcode::F64Const: return F64 { instruction.c };
            case Opcode::V128Const: return runtime::v128FromBytes(binary::v128LiteralOf(module, instruction));
            default: return std::nullopt;
        }
    }

    /// The null reference ref.null @p instruction pushes.
    [[nodiscard]] Value nullOf(Instruction const& instruction) noexcept
    {
        return Ref { .kind = RefKind::Null, .heap = instruction.type.heap, .address = 0 };
    }

    /// The reference ref.func pushes for function @p index of @p instance.
    [[nodiscard]] Value functionReference(ModuleInstance const& instance, uint32_t index)
    {
        return Ref { .kind = RefKind::Func,
                     .heap = { .kind = binary::AbstractHeapType::Func },
                     .address = instance.functions.at(index) };
    }

    /// The bits of a number value, zero-extended. Precondition (checked): @p value is a number.
    [[nodiscard]] uint64_t rawBits(Value const& value)
    {
        return std::visit(
            [](auto const& number) -> uint64_t {
                if constexpr (requires { number.bits; })
                    return number.bits;
                else
                    core::todo("rawBits of a value that is no number"); // uncovered(raw-bits)
            },
            value);
    }

    /// The number value of @p kind whose bits are the low bits of @p raw.
    [[nodiscard]] Value numberOf(ValueKind kind, uint64_t raw)
    {
        switch (kind)
        {
            case ValueKind::I32: return I32 { static_cast<uint32_t>(raw) };
            case ValueKind::I64: return I64 { raw };
            case ValueKind::F32: return F32 { static_cast<uint32_t>(raw) };
            case ValueKind::F64: return F64 { raw };
            case ValueKind::V128:       // uncovered(number-of)
            case ValueKind::Ref: break; // uncovered(number-of)
        }
        // uncovered-begin(number-of)
        core::todo("numberOf a kind that is no number");
    }
    // uncovered-end(number-of)

    /// Whether @p values are one value of each kind @p types names, in order.
    [[nodiscard]] bool haveKinds(std::span<Value const> values, std::span<binary::ValueType const> types)
    {
        return std::ranges::equal(values, types, {}, runtime::kindOf, &binary::ValueType::kind);
    }

    /// The type of a copy's length between memories or tables of address types @p destination and
    /// @p source: i64 only when both are, as memory64 and table64 define it.
    [[nodiscard]] constexpr AddressType copyCountType(AddressType destination, AddressType source) noexcept
    {
        return destination == AddressType::I64 && source == AddressType::I64 ? AddressType::I64
                                                                             : AddressType::I32;
    }

    // --- Memory accesses ----------------------------------------------------------------------

    /// The little-endian bytes @p raw's low @p width bytes are, read back; sign-extended if asked.
    [[nodiscard]] constexpr uint64_t widen(uint64_t raw, uint32_t width, Extension extension) noexcept
    {
        if (extension == Extension::Zero || width == sizeof(uint64_t))
            return raw;
        auto const unused = 64U - (8U * width);
        return static_cast<uint64_t>(static_cast<int64_t>(raw << unused) >> unused);
    }

    // --- The machine --------------------------------------------------------------------------

    /// What a branch to a label does with it: leaves a block, or repeats a loop.
    enum class LabelKind : uint8_t
    {
        Block, ///< A block or if: a branch continues after its end, and the label is gone.
        Loop,  ///< A loop: a branch continues at its start, and the label stays.
    };

    /// A label on the label stack: a structured control instruction being executed.
    struct Label
    {
        LabelKind kind = LabelKind::Block; ///< What a branch to it does.
        uint32_t arity = 0;                ///< How many values a branch to it carries.
        std::size_t height = 0;            ///< The operand stack height beneath its operands.
        uint32_t continuation = 0;         ///< The instruction a branch to it continues at.
    };

    /// An activation of a Wasm function.
    struct Frame
    {
        ModuleInstance* instance = nullptr;     ///< The instance the function belongs to.
        binary::Function const* code = nullptr; ///< Its code.
        std::size_t localsBase = 0;             ///< Its first local's index in the locals stack.
        std::size_t labelsBase = 0;             ///< The label stack's height when it began.
        std::size_t valuesBase = 0;             ///< The operand stack's height when it began.
        uint32_t arity = 0;                     ///< How many results it returns.
        uint32_t pc = 0;                        ///< The index of its next instruction.
    };

    /// The parameter and result counts of a block type.
    struct BlockArity
    {
        uint32_t params = 0;
        uint32_t results = 0;
    };

    /// One invocation's execution: the operand, locals, label and frame stacks, all on the heap.
    class Machine
    {
      public:
        /// @param store The store to execute on.
        /// @param depth The frames active over every invocation in progress; kept up to date.
        Machine(Store& store, uint32_t& depth): _store { store }, _depth { depth } {}

        Machine(Machine const&) = delete;
        Machine(Machine&&) = delete;
        Machine& operator=(Machine const&) = delete;
        Machine& operator=(Machine&&) = delete;

        /// Gives back the depth of the frames a trap left behind.
        ~Machine() { _depth -= static_cast<uint32_t>(_frames.size()); }

        /// Invokes @p function with @p args and writes its results to @p results.
        [[nodiscard]] Step run(FuncAddr function, std::span<Value const> args, std::span<Value> results)
        {
            auto const& type = _store.function(function).type;
            Require(haveKinds(args, type.params));
            Require(results.size() == type.results.size());
            _values.assign(args.begin(), args.end());
            if (auto const entered = call(function); !entered)
                return entered;
            while (!_frames.empty())
            {
                auto& frame = _frames.back();
                auto const& instruction = frame.code->body[frame.pc];
                ++frame.pc;
                if (auto const step = execute(instruction); !step)
                    return step;
            }
            Require(_values.size() == results.size());
            std::ranges::copy(_values, results.begin());
            return {};
        }

      private:
        [[nodiscard]] Frame& frame() noexcept { return _frames.back(); }

        [[nodiscard]] ModuleInstance& instance() noexcept { return *frame().instance; }

        // --- Operand stack ---

        void push(Value value) { _values.push_back(value); }

        [[nodiscard]] Value pop()
        {
            Require(!_values.empty());
            auto const value = _values.back();
            _values.pop_back();
            return value;
        }

        [[nodiscard]] uint32_t popI32() { return std::get<I32>(pop()).bits; }

        [[nodiscard]] Ref popRef() { return std::get<Ref>(pop()); }

        /// Pops an address, an offset or a size of a memory or table of @p type.
        [[nodiscard]] uint64_t popAddress(AddressType type)
        {
            return type == AddressType::I64 ? std::get<I64>(pop()).bits : popI32();
        }

        /// Pushes @p value as a value of @p type; -1 wraps to all ones, as memory.grow's failure.
        void pushAddress(AddressType type, uint64_t value)
        {
            push(type == AddressType::I64 ? Value { I64 { value } }
                                          : Value { I32 { static_cast<uint32_t>(value) } });
        }

        /// Keeps the top @p arity values, dropping every value between them and @p height.
        void keepTop(uint32_t arity, std::size_t height)
        {
            Require(_values.size() >= height + arity);
            auto const top = _values.end() - static_cast<std::ptrdiff_t>(arity);
            std::move(top, _values.end(), _values.begin() + static_cast<std::ptrdiff_t>(height));
            _values.resize(height + arity);
        }

        /// Whether the operand, locals and label stacks together hold MaxStackValues entries.
        [[nodiscard]] bool stacksExhausted() const noexcept
        {
            return _values.size() + _locals.size() + _labels.size() >= MaxStackValues;
        }

        // --- Calls ---

        /// Calls @p address: pushes a Wasm frame, its arguments moved off the operand stack into
        /// its locals, or runs a host function to completion.
        [[nodiscard]] Step call(FuncAddr address)
        {
            auto const& function = _store.function(address);
            if (function.isHost())
                return callHost(function);
            if (_depth >= _store.maxCallDepth() || stacksExhausted())
                return trap(TrapCode::CallStackExhausted);

            auto& owner = _store.moduleInstance(function.moduleInstance);
            auto const& code = owner.module->code.at(function.codeIndex);
            auto const paramCount = static_cast<std::ptrdiff_t>(function.type.params.size());
            auto const localsBase = _locals.size();
            _locals.insert(_locals.end(), _values.end() - paramCount, _values.end());
            _values.resize(_values.size() - static_cast<std::size_t>(paramCount));
            for (auto const& run: code.locals)
                _locals.insert(_locals.end(), run.count, runtime::defaultValue(run.type));
            _frames.push_back(Frame { .instance = &owner,
                                      .code = &code,
                                      .localsBase = localsBase,
                                      .labelsBase = _labels.size(),
                                      .valuesBase = _values.size(),
                                      .arity = static_cast<uint32_t>(function.type.results.size()),
                                      .pc = 0 });
            ++_depth;
            return {};
        }

        /// Runs a host function synchronously: it may grow memories and tables, and invoke Wasm.
        [[nodiscard]] Step callHost(FunctionInstance const& function)
        {
            auto const paramCount = static_cast<std::ptrdiff_t>(function.type.params.size());
            auto const args = std::vector<Value>(_values.end() - paramCount, _values.end());
            _values.resize(_values.size() - static_cast<std::size_t>(paramCount));
            auto results = std::vector<Value>(function.type.results.size());
            std::ranges::transform(function.type.results, results.begin(), runtime::defaultValue);
            if (auto const done = function.host(args, results); !done)
                return done;
            Require(haveKinds(results, function.type.results));
            _values.insert(_values.end(), results.begin(), results.end());
            return {};
        }

        /// Returns from the current frame, its results left on the operand stack.
        void doReturn()
        {
            auto const ending = frame();
            keepTop(ending.arity, ending.valuesBase);
            _labels.resize(ending.labelsBase);
            _locals.resize(ending.localsBase);
            _frames.pop_back();
            --_depth;
        }

        [[nodiscard]] Step callIndirect(Instruction const& instruction)
        {
            auto const& table = _store.table(instance().tables.at(instruction.b));
            auto const index = popAddress(table.type.limits.addressType);
            if (index >= table.elements.size())
                return trapNaming(TrapCode::UndefinedElement, index);
            auto const reference = table.elements[index];
            if (reference.kind == RefKind::Null)
                return trapNaming(TrapCode::UninitializedElement, index);
            if (_store.function(reference.address).type != instance().module->types.at(instruction.a))
                return trap(TrapCode::IndirectCallTypeMismatch);
            return call(reference.address);
        }

        // --- Control ---

        [[nodiscard]] BlockArity blockArity(Instruction const& instruction)
        {
            switch (instruction.blockKind)
            {
                case binary::BlockType::Kind::Empty: return {};
                case binary::BlockType::Kind::Value: return { .params = 0, .results = 1 };
                case binary::BlockType::Kind::TypeIndex: break;
            }
            auto const& type = instance().module->types.at(instruction.a);
            return { .params = static_cast<uint32_t>(type.params.size()),
                     .results = static_cast<uint32_t>(type.results.size()) };
        }

        /// Enters a block, loop or if whose operands are on the stack; traps with
        /// CallStackExhausted when the stacks hold MaxStackValues entries already.
        [[nodiscard]] Step enter(Instruction const& instruction, LabelKind kind)
        {
            if (stacksExhausted())
                return trap(TrapCode::CallStackExhausted);
            auto const arity = blockArity(instruction);
            _labels.push_back(Label {
                .kind = kind,
                .arity = kind == LabelKind::Loop ? arity.params : arity.results,
                .height = _values.size() - arity.params,
                // A loop's body starts after it, at the frame's next instruction already.
                .continuation = kind == LabelKind::Loop ? frame().pc : instruction.endIndex + 1,
            });
            return {};
        }

        /// Branches to label @p depth; the label one beyond the innermost is the function body's.
        void branch(uint32_t depth)
        {
            auto const labelCount = _labels.size() - frame().labelsBase;
            Require(depth <= labelCount);
            if (depth == labelCount)
            {
                doReturn();
                return;
            }
            auto const index = _labels.size() - 1 - depth;
            auto const label = _labels[index];
            keepTop(label.arity, label.height);
            _labels.resize(label.kind == LabelKind::Loop ? index + 1 : index);
            frame().pc = label.continuation;
        }

        void branchTable(Instruction const& instruction)
        {
            auto const labels = binary::labelsOf(*frame().code, instruction);
            auto const selector = popI32();
            branch(selector < labels.size() - 1 ? labels[selector] : labels.back());
        }

        [[nodiscard]] Step ifThen(Instruction const& instruction)
        {
            auto const condition = popI32();
            if (auto const entered = enter(instruction, LabelKind::Block); !entered || condition != 0)
                return entered;
            // Into the else branch, or to the end, whose execution leaves the label.
            frame().pc = instruction.elseIndex == instruction.endIndex ? instruction.endIndex
                                                                       : instruction.elseIndex + 1;
            return {};
        }

        void end()
        {
            if (_labels.size() > frame().labelsBase)
                _labels.pop_back();
            else
                doReturn();
        }

        // --- Memory ---

        [[nodiscard]] runtime::MemoryInstance& memory(uint32_t index)
        {
            return _store.memory(instance().memories.at(index));
        }

        /// The address of a load or store of @p width bytes at @p address + @p offset in a memory of
        /// @p size bytes, or nullopt when it is out of bounds.
        [[nodiscard]] static std::optional<uint64_t> effectiveAddress(uint64_t address,
                                                                      uint64_t offset,
                                                                      uint64_t width,
                                                                      uint64_t size) noexcept
        {
            if (offset > size || address > size - offset || !fits(address + offset, width, size))
                return std::nullopt;
            return address + offset;
        }

        /// Executes a load or store. Bytes are read and written one at a time, little-endian
        /// whatever the host's byte order, and the memory's bytes are fetched afresh.
        [[nodiscard]] Step accessMemory(MemoryAccess const& row, Instruction const& instruction)
        {
            if (row.value == ValueKind::V128)
                return accessVector(row, instruction);
            auto const width = uint32_t { row.width };
            auto const raw = row.access == MemoryAccessKind::Store ? rawBits(pop()) : uint64_t { 0 };
            auto& target = memory(instruction.b);
            auto const address = popAddress(target.type.limits.addressType);
            auto const effective = effectiveAddress(address, instruction.c, width, target.bytes.size());
            if (!effective)
                return trap(TrapCode::OutOfBoundsMemoryAccess);
            auto const bytes = std::span { target.bytes }.subspan(*effective, width);
            if (row.access == MemoryAccessKind::Store)
            {
                for (auto const index: std::views::iota(0U, width))
                    bytes[index] = static_cast<std::byte>(raw >> (8U * index));
                return {};
            }
            auto loaded = uint64_t { 0 };
            for (auto const index: std::views::iota(0U, width))
                loaded |= uint64_t { std::to_integer<uint8_t>(bytes[index]) } << (8U * index);
            push(numberOf(row.value, widen(loaded, width, row.extension)));
            return {};
        }

        /// Executes a v128 load or store, as accessMemory does a scalar one: the vector is
        /// composed from, or decomposed into, its little-endian bytes (v128FromBytes, v128ToBytes),
        /// and the row's shape says which of them the memory's bytes are.
        [[nodiscard]] Step accessVector(MemoryAccess const& row, Instruction const& instruction)
        {
            auto const width = std::size_t { row.width };
            // A store's value, or the vector a lane access changes; the operand above the address.
            auto vector = row.access == MemoryAccessKind::Store || row.shape == VectorShape::Lane
                              ? runtime::v128ToBytes(std::get<V128>(pop()))
                              : std::array<std::byte, 16> {};
            auto& target = memory(instruction.b);
            auto const address = popAddress(target.type.limits.addressType);
            auto const effective = effectiveAddress(address, instruction.c, width, target.bytes.size());
            if (!effective)
                return trap(TrapCode::OutOfBoundsMemoryAccess);
            auto const bytes = std::span { target.bytes }.subspan(*effective, width);
            // Where in the vector the accessed bytes sit: lane `lane` of a lane access, else the start.
            auto const offset = row.shape == VectorShape::Lane ? width * instruction.lane : std::size_t { 0 };
            auto const lanes = std::span { vector }.subspan(offset, width);
            if (row.access == MemoryAccessKind::Store)
            {
                std::ranges::copy(lanes, bytes.begin());
                return {};
            }
            std::ranges::copy(bytes, lanes.begin());
            switch (row.shape)
            {
                case VectorShape::Extend8x8:
                case VectorShape::Extend16x4:
                case VectorShape::Extend32x2: vector = widenLanes(row, bytes); break;
                case VectorShape::Splat:
                    for (auto const lane: std::views::iota(std::size_t { 1 }, vector.size() / width))
                        std::ranges::copy(bytes, std::span { vector }.subspan(lane * width, width).begin());
                    break;
                case VectorShape::Whole:
                case VectorShape::Zero:
                case VectorShape::Lane: break;
            }
            push(runtime::v128FromBytes(vector));
            return {};
        }

        /// The bytes of the vector a widening load of @p row yields from the 8 bytes @p bytes: each
        /// lane of memoryLaneWidth() bytes widened, per the row's extension, to twice its width.
        [[nodiscard]] static std::array<std::byte, 16> widenLanes(MemoryAccess const& row,
                                                                  std::span<std::byte const> bytes) noexcept
        {
            auto const laneWidth = row.memoryLaneWidth();
            auto result = std::array<std::byte, 16> {};
            for (auto const lane: std::views::iota(0U, row.width / laneWidth))
            {
                auto raw = uint64_t { 0 };
                for (auto const index: std::views::iota(0U, laneWidth))
                    raw |= uint64_t { std::to_integer<uint8_t>(bytes[(lane * laneWidth) + index]) }
                           << (8U * index);
                auto const widened = widen(raw, laneWidth, row.extension);
                for (auto const index: std::views::iota(0U, 2 * laneWidth))
                    result[(lane * 2 * laneWidth) + index] = static_cast<std::byte>(widened >> (8U * index));
            }
            return result;
        }

        void memoryGrow(Instruction const& instruction)
        {
            auto const address = instance().memories.at(instruction.a);
            auto const type = _store.memory(address).type.limits.addressType;
            auto const delta = popAddress(type);
            pushAddress(type, static_cast<uint64_t>(_store.growMemory(address, delta)));
        }

        void memorySize(Instruction const& instruction)
        {
            auto const& target = memory(instruction.a);
            pushAddress(target.type.limits.addressType, target.bytes.size() / runtime::PageSize);
        }

        [[nodiscard]] Step memoryFill(Instruction const& instruction)
        {
            auto& target = memory(instruction.a);
            auto const type = target.type.limits.addressType;
            auto const count = popAddress(type);
            auto const value = static_cast<std::byte>(popI32());
            auto const destination = popAddress(type);
            if (!fits(destination, count, target.bytes.size()))
                return trap(TrapCode::OutOfBoundsMemoryAccess);
            std::ranges::fill(std::span { target.bytes }.subspan(destination, count), value);
            return {};
        }

        [[nodiscard]] Step memoryCopy(Instruction const& instruction)
        {
            auto& destinationMemory = memory(instruction.a);
            auto& sourceMemory = memory(instruction.b);
            auto const destinationType = destinationMemory.type.limits.addressType;
            auto const sourceType = sourceMemory.type.limits.addressType;
            auto const countType = copyCountType(destinationType, sourceType);
            auto const count = popAddress(countType);
            auto const source = popAddress(sourceType);
            auto const destination = popAddress(destinationType);
            if (!fits(source, count, sourceMemory.bytes.size())
                || !fits(destination, count, destinationMemory.bytes.size()))
                return trap(TrapCode::OutOfBoundsMemoryAccess);
            if (count != 0)
                std::memmove(
                    destinationMemory.bytes.data() + destination, sourceMemory.bytes.data() + source, count);
            return {};
        }

        [[nodiscard]] Step memoryInit(Instruction const& instruction)
        {
            auto& target = memory(instruction.b);
            auto const segment = instance().data.at(instruction.a).value_or(std::span<std::byte const> {});
            auto const count = popI32();
            auto const source = popI32();
            auto const destination = popAddress(target.type.limits.addressType);
            return runtime::copySegment(segment,
                                        source,
                                        count,
                                        std::span { target.bytes },
                                        destination,
                                        TrapCode::OutOfBoundsMemoryAccess);
        }

        // --- Tables ---

        [[nodiscard]] runtime::TableInstance& table(uint32_t index)
        {
            return _store.table(instance().tables.at(index));
        }

        [[nodiscard]] Step tableGet(Instruction const& instruction)
        {
            auto const& target = table(instruction.a);
            auto const index = popAddress(target.type.limits.addressType);
            if (index >= target.elements.size())
                return trap(TrapCode::OutOfBoundsTableAccess);
            push(target.elements[index]);
            return {};
        }

        [[nodiscard]] Step tableSet(Instruction const& instruction)
        {
            auto& target = table(instruction.a);
            auto const reference = popRef();
            auto const index = popAddress(target.type.limits.addressType);
            if (index >= target.elements.size())
                return trap(TrapCode::OutOfBoundsTableAccess);
            target.elements[index] = reference;
            return {};
        }

        void tableSize(Instruction const& instruction)
        {
            auto const& target = table(instruction.a);
            pushAddress(target.type.limits.addressType, target.elements.size());
        }

        void tableGrow(Instruction const& instruction)
        {
            auto const address = instance().tables.at(instruction.a);
            auto const type = _store.table(address).type.limits.addressType;
            auto const delta = popAddress(type);
            auto const init = popRef();
            pushAddress(type, static_cast<uint64_t>(_store.growTable(address, delta, init)));
        }

        [[nodiscard]] Step tableFill(Instruction const& instruction)
        {
            auto& target = table(instruction.a);
            auto const type = target.type.limits.addressType;
            auto const count = popAddress(type);
            auto const value = popRef();
            auto const destination = popAddress(type);
            if (!fits(destination, count, target.elements.size()))
                return trap(TrapCode::OutOfBoundsTableAccess);
            std::ranges::fill(std::span { target.elements }.subspan(destination, count), value);
            return {};
        }

        [[nodiscard]] Step tableCopy(Instruction const& instruction)
        {
            auto& destinationTable = table(instruction.a);
            auto const& sourceTable = table(instruction.b);
            auto const destinationType = destinationTable.type.limits.addressType;
            auto const sourceType = sourceTable.type.limits.addressType;
            auto const countType = copyCountType(destinationType, sourceType);
            auto const count = popAddress(countType);
            auto const source = popAddress(sourceType);
            auto const destination = popAddress(destinationType);
            if (!fits(source, count, sourceTable.elements.size())
                || !fits(destination, count, destinationTable.elements.size()))
                return trap(TrapCode::OutOfBoundsTableAccess);
            // Through a copy, as the two ranges may overlap within one table.
            auto const slice =
                std::vector<Ref>(sourceTable.elements.begin() + static_cast<std::ptrdiff_t>(source),
                                 sourceTable.elements.begin() + static_cast<std::ptrdiff_t>(source + count));
            std::ranges::copy(slice,
                              destinationTable.elements.begin() + static_cast<std::ptrdiff_t>(destination));
            return {};
        }

        [[nodiscard]] Step tableInit(Instruction const& instruction)
        {
            auto& target = table(instruction.b);
            auto const& segment = instance().elements.at(instruction.a);
            auto const items = segment ? std::span<Ref const> { *segment } : std::span<Ref const> {};
            auto const count = popI32();
            auto const source = popI32();
            auto const destination = popAddress(target.type.limits.addressType);
            return runtime::copySegment(items,
                                        source,
                                        count,
                                        std::span { target.elements },
                                        destination,
                                        TrapCode::OutOfBoundsTableAccess);
        }

        // --- Dispatch ---

        /// Applies a binary numeric instruction to the two operands it pops, the right-hand one first.
        [[nodiscard]] std::expected<Value, Trap> binaryNumeric(Opcode opcode)
        {
            auto const rhs = pop();
            auto const lhs = pop();
            return applyBinary(opcode, lhs, rhs);
        }

        /// Applies a comparison to the two operands it pops, the right-hand one first.
        [[nodiscard]] I32 compareNumeric(Opcode opcode)
        {
            auto const rhs = pop();
            auto const lhs = pop();
            return applyCompare(opcode, lhs, rhs);
        }

        /// Applies a wide-arithmetic instruction to the two or four i64 it pops and pushes the
        /// result's low half, then its high half.
        void wideNumeric(Opcode opcode)
        {
            auto const count = binary::signatureInfo(binary::opcodeInfo(opcode).signature).paramCount;
            auto operands = std::array<uint64_t, 4> {};
            for (auto& operand: std::span { operands }.first(count) | std::views::reverse)
                operand = std::get<I64>(pop()).bits;
            auto const [low, high] = applyWide(opcode, std::span { operands }.first(count));
            push(I64 { low });
            push(I64 { high });
        }

        [[nodiscard]] V128 popV128() { return std::get<V128>(pop()); }

        /// Evaluates a SIMD operator by its row in Simd's table on the operands its signature pops.
        /// @return Its result, for the caller to push.
        [[nodiscard]] Value simd(Instruction const& instruction, SimdShape shape)
        {
            auto const opcode = instruction.opcode;
            auto const count = binary::signatureInfo(binary::opcodeInfo(opcode).signature).paramCount;
            auto operands = std::array<Value, 3> {};
            for (auto& operand: std::span { operands }.first(count) | std::views::reverse)
                operand = pop();
            auto const vector = [&](std::size_t index) {
                return std::get<V128>(operands[index]);
            };
            switch (shape)
            {
                case SimdShape::Binary: return applySimdBinary(opcode, vector(0), vector(1));
                case SimdShape::Ternary: return applySimdTernary(opcode, vector(0), vector(1), vector(2));
                case SimdShape::Test: return applySimdTest(opcode, vector(0));
                case SimdShape::Shift:
                    return applySimdShift(opcode, vector(0), std::get<I32>(operands[1]).bits);
                case SimdShape::Splat: return splat(opcode, operands[0]);
                case SimdShape::ExtractLane: return extractLane(opcode, vector(0), instruction.lane);
                case SimdShape::ReplaceLane:
                    return replaceLane(opcode, vector(0), instruction.lane, operands[1]);
                case SimdShape::Unary:
                case SimdShape::None: break;
            }
            // Unary. None never comes here, as execute() passes table rows only; were it to,
            // applySimdUnary's precondition check would stop it.
            return applySimdUnary(opcode, vector(0));
        }

        /// Executes i8x16.shuffle on the two vectors it pops.
        void shuffleBytes(Instruction const& instruction)
        {
            auto const b = popV128();
            auto const a = popV128();
            push(interp::shuffle(a, b, binary::v128LiteralOf(*instance().module, instruction)));
        }

        /// Executes a numeric instruction by its row in Numerics' table.
        [[nodiscard]] Step numeric(Opcode opcode, NumericShape shape)
        {
            auto const pushResult = [this](Value value) {
                push(value);
            };
            switch (shape)
            {
                case NumericShape::Unary: return applyUnary(opcode, pop()).transform(pushResult);
                case NumericShape::Test: push(applyTest(opcode, pop())); return {};
                case NumericShape::Binary: return binaryNumeric(opcode).transform(pushResult);
                case NumericShape::Compare: push(compareNumeric(opcode)); return {};
                case NumericShape::Wide: wideNumeric(opcode); return {};
                case NumericShape::None: break; // uncovered(numeric-shape-none)
            }
            // uncovered-begin(numeric-shape-none)
            core::todo(binary::opcodeInfo(opcode).text);
        }
        // uncovered-end(numeric-shape-none)

        /// Executes one instruction of the current frame, whose pc points past it already.
        [[nodiscard]] Step execute(Instruction const& instruction)
        {
            auto const opcode = instruction.opcode;
            if (auto const shape = numericShape(opcode); shape != NumericShape::None)
                return numeric(opcode, shape);
            if (auto const shape = simdShape(opcode); shape != SimdShape::None)
            {
                push(simd(instruction, shape));
                return {};
            }
            if (auto const* row = binary::memoryAccessOf(opcode))
                return accessMemory(*row, instruction);
            if (auto const constant = constantOf(*instance().module, instruction))
            {
                push(*constant);
                return {};
            }

            switch (opcode)
            {
                // Control
                case Opcode::Unreachable: return trap(TrapCode::Unreachable);
                case Opcode::Nop: return {};
                case Opcode::Block: return enter(instruction, LabelKind::Block);
                case Opcode::Loop: return enter(instruction, LabelKind::Loop);
                case Opcode::If: return ifThen(instruction);
                case Opcode::Else: frame().pc = instruction.endIndex; return {};
                case Opcode::End: end(); return {};
                case Opcode::Br: branch(instruction.a); return {};
                case Opcode::BrIf:
                    if (popI32() != 0)
                        branch(instruction.a);
                    return {};
                case Opcode::BrTable: branchTable(instruction); return {};
                case Opcode::Return: doReturn(); return {};
                case Opcode::Call: return call(instance().functions.at(instruction.a));
                case Opcode::CallIndirect: return callIndirect(instruction);

                // Parametric
                case Opcode::Drop: static_cast<void>(pop()); return {};
                case Opcode::Select:
                case Opcode::SelectTyped: {
                    auto const condition = popI32();
                    auto const second = pop();
                    auto const first = pop();
                    push(condition != 0 ? first : second);
                    return {};
                }

                // Variables
                case Opcode::LocalGet: push(_locals.at(frame().localsBase + instruction.a)); return {};
                case Opcode::LocalSet: {
                    auto const value = pop();
                    _locals.at(frame().localsBase + instruction.a) = value;
                    return {};
                }
                case Opcode::LocalTee:
                    _locals.at(frame().localsBase + instruction.a) = _values.back();
                    return {};
                case Opcode::GlobalGet:
                    push(_store.global(instance().globals.at(instruction.a)).value);
                    return {};
                case Opcode::GlobalSet: {
                    auto const value = pop();
                    _store.global(instance().globals.at(instruction.a)).value = value;
                    return {};
                }

                // Tables
                case Opcode::TableGet: return tableGet(instruction);
                case Opcode::TableSet: return tableSet(instruction);
                case Opcode::TableSize: tableSize(instruction); return {};
                case Opcode::TableGrow: tableGrow(instruction); return {};
                case Opcode::TableFill: return tableFill(instruction);
                case Opcode::TableCopy: return tableCopy(instruction);
                case Opcode::TableInit: return tableInit(instruction);
                case Opcode::ElemDrop: instance().elements.at(instruction.a).reset(); return {};

                // Memory
                case Opcode::MemorySize: memorySize(instruction); return {};
                case Opcode::MemoryGrow: memoryGrow(instruction); return {};
                case Opcode::MemoryFill: return memoryFill(instruction);
                case Opcode::MemoryCopy: return memoryCopy(instruction);
                case Opcode::MemoryInit: return memoryInit(instruction);
                case Opcode::DataDrop: instance().data.at(instruction.a).reset(); return {};

                // References
                case Opcode::RefNull: push(nullOf(instruction)); return {};
                case Opcode::RefIsNull: push(I32 { popRef().kind == RefKind::Null ? 1U : 0U }); return {};
                case Opcode::RefFunc: push(functionReference(instance(), instruction.a)); return {};

                // Vectors
                case Opcode::I8x16Shuffle: shuffleBytes(instruction); return {};

                default: break; // uncovered(execute-default)
            }
            // uncovered-begin(execute-default)
            // Every instruction validation admits has a case above or a table row, which
            // Numerics.everyFixedSignatureOpcodeHasARow and Simd.everyFixedSignatureSimdOpcodeHasARow
            // hold the Numerics and Simd tables to.
            core::todo(binary::opcodeInfo(opcode).text);
        }
        // uncovered-end(execute-default)

        Store& _store;
        uint32_t& _depth;
        std::vector<Value> _values;
        std::vector<Value> _locals;
        std::vector<Label> _labels;
        std::vector<Frame> _frames;
    };
} // namespace

std::expected<void, Trap> ReferenceInterpreter::invoke(Store& store,
                                                       FuncAddr function,
                                                       std::span<Value const> args,
                                                       std::span<Value> results)
{
    // Float results are the spec's only in the default floating-point environment.
    assert(std::fegetround() == FE_TONEAREST);
    if (_invocations >= MaxNestedInvocations)
        return trap(TrapCode::CallStackExhausted);
    // Unwinds on every exit, an exception thrown by a host callback included.
    struct InvocationGuard
    {
        uint32_t& count;
        explicit InvocationGuard(uint32_t& counter): count { counter } { ++count; }
        InvocationGuard(InvocationGuard const&) = delete;
        InvocationGuard& operator=(InvocationGuard const&) = delete;
        InvocationGuard(InvocationGuard&&) = delete;
        InvocationGuard& operator=(InvocationGuard&&) = delete;
        ~InvocationGuard() { --count; }
    };
    auto const guard = InvocationGuard { _invocations };
    return Machine { store, _depth }.run(function, args, results);
}

std::expected<Value, Trap> ReferenceInterpreter::evaluateConst(Store& store,
                                                               uint32_t moduleInstance,
                                                               binary::ConstExpr const& expression)
{
    auto const& instance = store.moduleInstance(moduleInstance);
    auto stack = std::vector<Value> {};
    for (auto const& instruction: expression)
    {
        if (auto const constant = constantOf(*instance.module, instruction))
        {
            stack.push_back(*constant);
            continue;
        }
        switch (instruction.opcode)
        {
            case Opcode::End: Require(stack.size() == 1); return stack.back();
            case Opcode::GlobalGet:
                stack.push_back(store.global(instance.globals.at(instruction.a)).value);
                break;
            case Opcode::RefNull: stack.push_back(nullOf(instruction)); break;
            case Opcode::RefFunc: stack.push_back(functionReference(instance, instruction.a)); break;
            default: {
                // Extended-const's i32 and i64 add, sub and mul.
                Require(numericShape(instruction.opcode) == NumericShape::Binary && stack.size() >= 2);
                auto const rhs = stack.back();
                stack.pop_back();
                auto const lhs = stack.back();
                stack.pop_back();
                auto const result = applyBinary(instruction.opcode, lhs, rhs);
                if (!result)
                    return std::unexpected { result.error() }; // uncovered(trapping-constant)
                stack.push_back(*result);
            }
        }
    }
    // uncovered-begin(constant-without-end)
    core::todo("a constant expression without end");
}
// uncovered-end(constant-without-end)

} // namespace isoline::interp
