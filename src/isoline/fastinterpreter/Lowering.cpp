// SPDX-License-Identifier: Apache-2.0
#include <isoline/fastinterpreter/Lowering.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <ranges>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <isoline/fastinterpreter/LoweringTable.hpp>
#include <isoline/fastinterpreter/SlotCodec.hpp>

namespace isoline::fastinterpreter
{

namespace
{
    using binary::Opcode;
    using binary::ValueType;

    /// What an entry of the abstract operand stack holds.
    enum class EntryKind : uint8_t
    {
        Slot,     ///< A value in its home slot.
        Local,    ///< A forwarded local.get: the local's current value, which no slot copies yet.
        Constant, ///< A constant, which no slot holds yet.
    };

    /// The end of a forwarding chain: no older entry forwards the same local.
    inline constexpr auto NoEntry = std::numeric_limits<uint32_t>::max();

    /// One entry of the abstract operand stack.
    struct Entry
    {
        EntryKind kind = EntryKind::Slot; ///< What it holds.
        ValueType type {};                ///< Its value's type.
        uint32_t slot = 0;  ///< Its home slot: the locals' slots plus the widths of the entries below it.
        uint32_t local = 0; ///< Local: the local it forwards.
        uint32_t olderForwarder = NoEntry; ///< Local: the next entry below forwarding the same local.
        uint64_t constant = 0;             ///< Constant: its bits, an i32 or f32 zero-extended.
    };

    /// A run of a function's locals of one type: a parameter, or a declared run.
    struct LocalRun
    {
        uint32_t firstLocal = 0; ///< The index of its first local, the parameters counted.
        uint32_t firstSlot = 0;  ///< The slot of its first local.
        ValueType type {};       ///< The type of its locals.
    };

    /// Whether lowering goes on after an instruction, or the rest of the body is dead.
    enum class Flow : uint8_t
    {
        Continues, ///< The next instruction is reachable.
        Ends,      ///< The instruction left the function; what follows up to the body's end is dead.
    };

    /// The move of a value of each SlotClass from slot to slot, indexed by the class.
    inline constexpr auto MoveOperations =
        std::to_array<OperationId>({ OperationId::Move64, OperationId::Move128, OperationId::Move128 });

    /// The select of two values of each SlotClass, indexed by the class.
    inline constexpr auto SelectOperations =
        std::to_array<OperationId>({ OperationId::Select64, OperationId::Select128, OperationId::Select128 });

    /// The operation of @p table for values of @p type.
    [[nodiscard]] OperationId operationFor(std::span<OperationId const, 3> table,
                                           ValueType const& type) noexcept
    {
        return table[std::to_underlying(slotClassOf(type))];
    }

    /// The slots a value of @p type takes.
    [[nodiscard]] uint32_t widthOf(ValueType const& type) noexcept
    {
        return slotWidth(slotClassOf(type));
    }

    /// The type of the one result of an instruction whose signature is fixed.
    [[nodiscard]] ValueType resultTypeOf(Opcode opcode) noexcept
    {
        auto const& signature = binary::signatureInfo(binary::opcodeInfo(opcode).signature);
        return ValueType { .kind = signature.resultKinds().front() };
    }

    /// What every function's lowering reads of its module.
    struct ModuleContext
    {
        binary::Module const& module;               ///< The module.
        std::span<ValueType const> importedGlobals; ///< The value types of its imported globals, in order.

        /// The value type of a global.
        /// @param index The global's index, the imported ones first.
        [[nodiscard]] ValueType globalType(uint32_t index) const
        {
            if (index < importedGlobals.size())
                return importedGlobals[index];
            return module.globals.at(index - importedGlobals.size()).type.type;
        }
    };

    /// The value types of @p module's imported globals, in order, allocated from @p resource.
    [[nodiscard]] std::pmr::vector<ValueType> importedGlobalTypes(binary::Module const& module,
                                                                  std::pmr::memory_resource* resource)
    {
        auto types = std::pmr::vector<ValueType>(resource);
        types.reserve(module.importedCount(binary::ExternKind::Global));
        for (auto const& import: module.imports)
            if (import.kind == binary::ExternKind::Global)
                types.push_back(import.global.type);
        return types;
    }

    /// The lowering of one function's body: one forward pass over its instructions with an abstract
    /// operand stack whose entries are slots, forwarded locals or constants.
    ///
    /// Each entry's home slot is fixed by its height. A local.get pushes an entry that forwards the local,
    /// linked into the local's chain, which a local.set or local.tee of it materializes, and only its own:
    /// each entry is materialized at most once, so the work is linear in the body. Every container
    /// allocates from the lowering arena; the code's capacity is the function's bound, reserved beforehand.
    class FunctionLowering
    {
      public:
        /// Prepares the lowering of one function into @p function, whose code and local-initialization
        /// runs have their reserved capacity already.
        /// @param context The module.
        /// @param validated The validated module, for the function's frame shape and cost.
        /// @param codeIndex The function's index in the code section.
        /// @param function Where its code goes.
        /// @param resource The lowering arena.
        FunctionLowering(ModuleContext const& context,
                         validate::ValidatedModule const& validated,
                         uint32_t codeIndex,
                         LoweredFunction& function,
                         std::pmr::memory_resource* resource):
            _source { context.module.code.at(codeIndex) },
            _type { context.module.types.at(_source.typeIndex) },
            _context { context },
            _function { function },
            _code { function.code },
            _localRuns(resource),
            _stack(resource),
            _chains(resource),
            _top { layOutLocals() },
            _maxTop { _top }
        {
            _function.codeIndex = codeIndex;
            _function.frameCost = validated.frameCost(codeIndex);
            _function.resultSlots = 0;
            for (auto const& result: _type.results)
                _function.resultSlots += widthOf(result);
            _stack.reserve(validated.frameShape(codeIndex).maxOperands);
        }

        /// Lowers the body, then fits the code's capacity to its size.
        /// @return The abstract operand stack entries it touched (LoweringStats::work).
        [[nodiscard]] uint64_t run()
        {
            for (auto const& instruction: _source.body)
                if (lowerInstruction(instruction) == Flow::Ends)
                    break;
            _function.frameSlots = _maxTop;
            // Reserved at the bound; the copy holds what the function needs, and frees the rest.
            _code = std::pmr::vector<Word>(_code.begin(), _code.end(), _code.get_allocator());
            return _work;
        }

      private:
        /// Assigns the parameters and declared locals their slots, and derives the local-initialization runs.
        /// @return The slots of the parameters and declared locals: where the operand stack begins.
        [[nodiscard]] uint32_t layOutLocals()
        {
            _localRuns.reserve(_type.params.size() + _source.locals.size());
            // Counted in 64 bits: frameSlotsOf() refused every frame past MaxFrameSlots before this.
            auto slot = uint64_t { 0 };
            auto local = uint32_t { 0 };
            for (auto const& param: _type.params)
            {
                _localRuns.push_back(LocalRun {
                    .firstLocal = local, .firstSlot = static_cast<uint32_t>(slot), .type = param });
                ++local;
                slot += widthOf(param);
            }
            _function.paramSlots = static_cast<uint32_t>(slot);
            for (auto const& run: _source.locals)
            {
                if (run.count == 0)
                    continue;
                _localRuns.push_back(LocalRun {
                    .firstLocal = local, .firstSlot = static_cast<uint32_t>(slot), .type = run.type });
                auto const slots = uint64_t { run.count } * widthOf(run.type);
                appendLocalInit(static_cast<uint32_t>(slot), static_cast<uint32_t>(slots), run.type);
                local += run.count;
                slot += slots;
            }
            Require(slot <= MaxFrameSlots);
            _function.localSlots = static_cast<uint32_t>(slot);
            return _function.localSlots;
        }

        /// Appends the initialization of @p slotCount slots from @p slotBase, of declared locals of @p type,
        /// merging it into the previous run where both start alike.
        void appendLocalInit(uint32_t slotBase, uint32_t slotCount, ValueType const& type)
        {
            auto const kind = slotClassOf(type) == SlotClass::Reference ? LocalInit::Null : LocalInit::Zero;
            auto const heap = kind == LocalInit::Null ? type.heap : binary::HeapType {};
            auto& runs = _function.localInit;
            if (!runs.empty() && runs.back().kind == kind && runs.back().heap == heap)
            {
                runs.back().slotCount += slotCount;
                return;
            }
            runs.push_back(
                LocalInitRun { .slotBase = slotBase, .slotCount = slotCount, .kind = kind, .heap = heap });
        }

        /// The run holding @p local.
        [[nodiscard]] LocalRun const& runOf(uint32_t local) const
        {
            auto const after = std::ranges::upper_bound(_localRuns, local, {}, &LocalRun::firstLocal);
            Require(after != _localRuns.begin());
            return *std::prev(after);
        }

        /// The slot of @p local.
        [[nodiscard]] uint32_t localSlot(uint32_t local) const
        {
            auto const& run = runOf(local);
            return run.firstSlot + ((local - run.firstLocal) * widthOf(run.type));
        }

        /// Lowers one reachable instruction.
        [[nodiscard]] Flow lowerInstruction(binary::Instruction const& instruction)
        {
            auto const& row = loweringRow(instruction.opcode);
            switch (row.form)
            {
                case OperationForm::Unary: lowerUnary(row); return Flow::Continues;
                case OperationForm::Binary:
                case OperationForm::BinaryImmediate: lowerBinary(row); return Flow::Continues;
                default: return lowerSpecial(instruction);
            }
        }

        /// Lowers an instruction of a Special row.
        [[nodiscard]] Flow lowerSpecial(binary::Instruction const& instruction)
        {
            switch (instruction.opcode)
            {
                case Opcode::Nop: return Flow::Continues;
                case Opcode::Unreachable: emit(OperationId::Unreachable, {}, {}); return Flow::Ends;
                case Opcode::Return:
                case Opcode::End: lowerReturn(); return Flow::Ends;
                case Opcode::Drop: std::ignore = pop(); return Flow::Continues;
                case Opcode::Select:
                case Opcode::SelectTyped: lowerSelect(); return Flow::Continues;
                case Opcode::LocalGet: pushLocal(instruction.a); return Flow::Continues;
                case Opcode::LocalSet: std::ignore = setLocal(instruction.a); return Flow::Continues;
                case Opcode::LocalTee: push(setLocal(instruction.a)); return Flow::Continues;
                case Opcode::GlobalGet: lowerGlobalGet(instruction.a); return Flow::Continues;
                case Opcode::GlobalSet: lowerGlobalSet(instruction.a); return Flow::Continues;
                case Opcode::I32Const:
                case Opcode::I64Const:
                case Opcode::F32Const:
                case Opcode::F64Const:
                    push(Entry { .kind = EntryKind::Constant,
                                 .type = resultTypeOf(instruction.opcode),
                                 .constant = instruction.c });
                    return Flow::Continues;
                // uncovered-begin(lowering-later-task)
                default:
                    core::todo(std::format(
                        "lowering {}: control flow, calls, memory, references, SIMD and exceptions "
                        "are lowered by later parts of the fast interpreter",
                        binary::opcodeInfo(instruction.opcode).text));
                    // uncovered-end(lowering-later-task)
            }
        }

        /// Lowers a Unary row: its operand's slot to a result in the operand's home slot.
        void lowerUnary(LoweringRow const& row)
        {
            auto operand = pop();
            auto const source = readable(operand);
            emit(row.operation, { operand.slot, source }, {});
            push(Entry { .type = resultTypeOf(row.opcode) });
        }

        /// Lowers a Binary or BinaryImmediate row: a constant right operand folds into the immediate twin
        /// where the row has one, any other constant is materialized.
        void lowerBinary(LoweringRow const& row)
        {
            auto right = pop();
            auto left = pop();
            auto const leftSource = readable(left);
            if (row.form == OperationForm::BinaryImmediate && right.kind == EntryKind::Constant)
                emit(row.immediate, { left.slot, leftSource }, { right.constant });
            else
                emit(row.operation, { left.slot, leftSource, readable(right) }, {});
            push(Entry { .type = resultTypeOf(row.opcode) });
        }

        /// Lowers select and typed select: the result goes to the first value's home slot.
        void lowerSelect()
        {
            auto condition = pop();
            auto second = pop();
            auto first = pop();
            auto const firstSource = readable(first);
            auto const secondSource = readable(second);
            emit(operationFor(SelectOperations, first.type),
                 { first.slot, firstSource, secondSource, readable(condition) },
                 {});
            push(Entry { .type = first.type });
        }

        /// Lowers global.get of @p global into a fresh slot.
        void lowerGlobalGet(uint32_t global)
        {
            auto const type = _context.globalType(global);
            auto const slot = _top;
            push(Entry { .type = type });
            emit(OperationId::GlobalGet, { slot }, { global });
        }

        /// Lowers global.set of @p global from the operand on top.
        void lowerGlobalSet(uint32_t global)
        {
            auto value = pop();
            emit(OperationId::GlobalSet, { readable(value) }, { global });
        }

        /// Lowers return, and the end of the body: the results are materialized into their home slots,
        /// which are consecutive, and the first of them returned.
        void lowerReturn()
        {
            for ([[maybe_unused]] auto const& result: _type.results)
                materialize(pop());
            emit(OperationId::Return, { _top }, {});
        }

        /// Pops the operand on top into @p local, after materializing every entry forwarding the local's
        /// old value, and only those.
        /// @return The popped entry, which local.tee pushes again: its value is the local's now.
        [[nodiscard]] Entry setLocal(uint32_t local)
        {
            auto value = pop();
            // The local's own value written back changes nothing: its forwarders stay forwarders.
            if (value.kind == EntryKind::Local && value.local == local)
                return value;
            materializeChain(local);
            auto const target = localSlot(local);
            switch (value.kind)
            {
                case EntryKind::Slot:
                    emit(operationFor(MoveOperations, value.type), { target, value.slot }, {});
                    break;
                case EntryKind::Local:
                    emit(operationFor(MoveOperations, value.type), { target, localSlot(value.local) }, {});
                    break;
                case EntryKind::Constant: emit(OperationId::Const64, { target }, { value.constant }); break;
            }
            return value;
        }

        /// Pushes an entry forwarding @p local.
        void pushLocal(uint32_t local)
        {
            push(Entry { .kind = EntryKind::Local, .type = runOf(local).type, .local = local });
        }

        /// Pushes @p entry at the top: its home slot is the top's, and a forwarded local joins its chain.
        void push(Entry entry)
        {
            // The validator's operand high-water mark sized the stack, so it never reallocates.
            Require(_stack.size() < _stack.capacity());
            entry.slot = _top;
            _top += widthOf(entry.type);
            _maxTop = std::max(_maxTop, _top);
            if (entry.kind == EntryKind::Local)
            {
                auto const chain = _chains.try_emplace(entry.local, NoEntry).first;
                entry.olderForwarder = chain->second;
                chain->second = static_cast<uint32_t>(_stack.size());
            }
            else
                entry.olderForwarder = NoEntry;
            _stack.push_back(entry);
            ++_work;
        }

        /// Pops the entry on top; a forwarded local leaves its chain, of which it is the newest.
        [[nodiscard]] Entry pop()
        {
            Require(!_stack.empty());
            auto const entry = _stack.back();
            _stack.pop_back();
            if (entry.kind == EntryKind::Local)
            {
                auto const chain = _chains.find(entry.local);
                Require(chain != _chains.end() && chain->second == _stack.size());
                if (entry.olderForwarder == NoEntry)
                    _chains.erase(chain);
                else
                    chain->second = entry.olderForwarder;
            }
            _top = entry.slot;
            ++_work;
            return entry;
        }

        /// The slot an operation reads @p entry from: its own, its local's, or, for a constant, its home
        /// slot once the constant is materialized there.
        [[nodiscard]] uint32_t readable(Entry const& entry)
        {
            switch (entry.kind)
            {
                case EntryKind::Slot: break;
                case EntryKind::Local: return localSlot(entry.local);
                case EntryKind::Constant: materialize(entry); break;
            }
            return entry.slot;
        }

        /// Copies what @p entry holds into its home slot, unless it is there already.
        void materialize(Entry const& entry)
        {
            switch (entry.kind)
            {
                case EntryKind::Slot: return;
                case EntryKind::Local:
                    emit(
                        operationFor(MoveOperations, entry.type), { entry.slot, localSlot(entry.local) }, {});
                    break;
                case EntryKind::Constant:
                    emit(OperationId::Const64, { entry.slot }, { entry.constant });
                    break;
            }
            ++_work;
        }

        /// Materializes every entry forwarding @p local, which then holds its value in its own slot.
        void materializeChain(uint32_t local)
        {
            auto const chain = _chains.find(local);
            if (chain == _chains.end())
                return;
            auto position = chain->second;
            while (position != NoEntry)
            {
                auto& entry = _stack[position];
                materialize(entry);
                position = entry.olderForwarder;
                entry.kind = EntryKind::Slot;
                entry.olderForwarder = NoEntry;
            }
            _chains.erase(chain);
        }

        /// Appends @p operation with its slot operands and immediates, as OperationTable lays it out.
        void emit(OperationId operation,
                  std::initializer_list<uint32_t> slots,
                  std::initializer_list<uint64_t> immediates)
        {
            auto const& row = operationRow(operation);
            Require(slots.size() == row.slots && immediates.size() == row.immediates);
            // The capacity is the function's bound (codeBoundWords()); outgrowing it breaks the published
            // bound.
            Require(_code.size() + row.words <= _code.capacity());
            _code.push_back(Word { std::to_underlying(operation) });
            auto const operands = std::span { slots.begin(), slots.size() };
            for (auto const pair: std::views::iota(std::size_t { 0 }, (operands.size() + 1) / 2))
            {
                auto word = Word { operands[2 * pair] };
                if ((2 * pair) + 1 < operands.size())
                    word |= Word { operands[(2 * pair) + 1] } << 32U;
                _code.push_back(word);
            }
            _code.insert(_code.end(), immediates.begin(), immediates.end());
        }

        binary::Function const& _source;
        binary::FuncType const& _type;
        ModuleContext const& _context;
        LoweredFunction& _function;
        std::pmr::vector<Word>& _code;
        std::pmr::vector<LocalRun> _localRuns;
        std::pmr::vector<Entry> _stack;
        std::pmr::unordered_map<uint32_t, uint32_t> _chains; ///< Each forwarded local's newest forwarder.
        uint32_t _top = 0;                                   ///< The home slot of the next entry pushed.
        uint32_t _maxTop = 0;                                ///< The highest _top reached: the frame's slots.
        uint64_t _work = 0;
    };

    /// The most slots a frame of @p function may take, in 64 bits: its params' and locals' slots, and two
    /// per operand of its deepest operand stack (validate::FrameShape::maxOperands).
    [[nodiscard]] uint64_t frameSlotsOf(validate::ValidatedModule const& validated, uint32_t codeIndex)
    {
        auto const& module = validated.module();
        auto const& function = module.code[codeIndex];
        auto slots = uint64_t { 2 } * validated.frameShape(codeIndex).maxOperands;
        for (auto const& param: module.types.at(function.typeIndex).params)
            slots += widthOf(param);
        for (auto const& run: function.locals)
            slots += uint64_t { run.count } * widthOf(run.type);
        return slots;
    }

    /// The LoweringFailure of each RefusalReason of the arena, indexed by the reason.
    inline constexpr auto RefusalFailures =
        std::to_array<LoweringFailure>({ LoweringFailure::Budget, LoweringFailure::HostMemory });

    /// Lowers every function of @p validated into @p functions, reserving each one's bound first.
    /// @param codeIndex Set to the function being lowered, which a refusal names; NoFunction before the
    /// first.
    [[nodiscard]] std::expected<LoweringStats, LoweringError> lowerFunctions(
        validate::ValidatedModule const& validated,
        ILoweringArena& arena,
        std::pmr::vector<LoweredFunction>& functions,
        uint32_t& codeIndex)
    {
        auto const& module = validated.module();
        auto const importedGlobals = importedGlobalTypes(module, &arena);
        auto const context = ModuleContext { .module = module, .importedGlobals = importedGlobals };
        auto stats = LoweringStats {};
        for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(module.code.size())))
        {
            codeIndex = index;
            if (auto const slots = frameSlotsOf(validated, index); slots > MaxFrameSlots)
                return std::unexpected(LoweringError { .codeIndex = index,
                                                       .requested = slots,
                                                       .limit = MaxFrameSlots,
                                                       .reason = LoweringFailure::FrameSlots });
            auto const reservation = loweringReservation(module, index);
            if (auto const reserved = arena.reserve(reservation); !reserved)
                return std::unexpected(
                    LoweringError { .codeIndex = index, .requested = reservation, .limit = arena.budget() });
            // What the reservation covers, allocated first so that it draws on it: the records of every
            // function with the first, then this function's local-initialization runs and code bound.
            if (functions.empty())
                functions.reserve(module.code.size());
            auto& function = functions.emplace_back(&arena);
            auto const& source = module.code[index];
            function.localInit.reserve(source.locals.size());
            function.code.reserve(codeBoundWords(source));
            stats.bodyUnits += bodyUnits(source);
            stats.work += FunctionLowering(context, validated, index, function, &arena).run();
        }
        return stats;
    }
} // namespace

uint64_t loweringReservation(binary::Module const& module, uint32_t codeIndex)
{
    Require(codeIndex < module.code.size());
    auto const& function = module.code[codeIndex];
    auto bytes = (codeBoundWords(function) * sizeof(Word)) + (function.locals.size() * sizeof(LocalInitRun));
    if (codeIndex == 0)
        bytes += module.code.size() * sizeof(LoweredFunction);
    return bytes;
}

std::expected<std::shared_ptr<LoweredModule const>, LoweringError> lower(
    validate::ValidatedModule const& module,
    LoweringConfig const& config,
    std::shared_ptr<ILoweringArena> arena)
{
    Require(arena != nullptr);
    Require(std::ranges::find(availableDispatches(), config.dispatch) != availableDispatches().end());
    auto const chargedBefore = arena->charged();
    auto const budget = arena->budget();
    auto functions = std::pmr::vector<LoweredFunction>(arena.get());
    auto codeIndex = NoFunction;
    auto stats = std::expected<LoweringStats, LoweringError> {};
    try
    {
        stats = lowerFunctions(module, *arena, functions, codeIndex);
    }
    catch (std::bad_alloc const&)
    {
        // The arena refused an allocation, for its budget or for the host's memory. What was allocated
        // unwinds with the throw; what is left of the reservation is returned here.
        arena->release(arena->reserved());
        auto const refusal = arena->lastRefusal();
        Require(refusal.has_value());
        return std::unexpected(
            LoweringError { .codeIndex = codeIndex,
                            .requested = refusal->requested,
                            .limit = budget,
                            .reason = RefusalFailures[std::to_underlying(refusal->reason)] });
    }
    if (!stats)
        return std::unexpected(stats.error());
    auto const loweredBytes = arena->charged() - chargedBefore;
    // The module's own record comes from the host's memory, not the arena: it owns the arena.
    auto const host = std::pmr::polymorphic_allocator<> { &arena->hostResource() };
    auto resource = std::shared_ptr<std::pmr::memory_resource> { std::move(arena) };
    try
    {
        return std::allocate_shared<LoweredModule const>(host,
                                                         std::move(resource),
                                                         std::move(functions),
                                                         config.dispatch,
                                                         config.mode,
                                                         loweredBytes,
                                                         *stats);
    }
    catch (std::bad_alloc const&)
    {
        return std::unexpected(LoweringError { .codeIndex = NoFunction,
                                               .requested = sizeof(LoweredModule),
                                               .limit = budget,
                                               .reason = LoweringFailure::HostMemory });
    }
}

} // namespace isoline::fastinterpreter
