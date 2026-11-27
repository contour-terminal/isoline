// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/translation/Translator.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/binary/BodyUnits.hpp>
#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/compiler/FeatureSupport.hpp>
#include <isoline/compiler/intermediate/Operation.hpp>
#include <isoline/compiler/translation/LoopAssignments.hpp>
#include <isoline/compiler/translation/NumericRules.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::compiler::translation
{

ModuleIndex::ModuleIndex(binary::Module const& module, std::pmr::memory_resource& memory):
    _module { &module }, _importedFunctionTypes(&memory), _importedGlobals(&memory)
{
    for (auto const& entry: module.imports)
        switch (entry.kind)
        {
            case binary::ExternKind::Func: _importedFunctionTypes.push_back(entry.typeIndex); break;
            case binary::ExternKind::Global: _importedGlobals.push_back(&entry.global); break;
            case binary::ExternKind::Memory: ++_memoryCount; break;
            case binary::ExternKind::Table:
            case binary::ExternKind::Tag: break;
        }
    _memoryCount += static_cast<uint32_t>(module.memories.size());
}

uint32_t ModuleIndex::functionType(uint32_t function) const noexcept
{
    if (function < _importedFunctionTypes.size())
        return _importedFunctionTypes[function];
    auto const defined = std::size_t { function } - _importedFunctionTypes.size();
    Require(defined < _module->functions.size());
    return _module->functions[defined];
}

binary::GlobalType const& ModuleIndex::globalType(uint32_t global) const noexcept
{
    if (global < _importedGlobals.size())
        return *_importedGlobals[global];
    auto const defined = std::size_t { global } - _importedGlobals.size();
    Require(defined < _module->globals.size());
    return _module->globals[defined].type;
}

namespace
{
    using intermediate::BlockId;
    using intermediate::BranchTo;
    using intermediate::Builder;
    using intermediate::ContextValue;
    using intermediate::Function;
    using intermediate::Hotness;
    using intermediate::Operation;
    using intermediate::Type;
    using intermediate::Unnumbered;
    using intermediate::ValueId;

    /// How many WebAssembly instructions translation visits between two polls of its cancellation.
    constexpr auto CancellationInterval = uint64_t { 65'536 };

    /// The pool entries one WebAssembly instruction may append beyond the demand it reserves: its fixed
    /// part, of which the longest (a trapping truncation's checks) takes some 40.
    constexpr auto FixedEmission = uint64_t { 1'024 };

    /// The immediate that holds the argument count of a forward edge's target: a jump's only target, a
    /// branch's taken one. Its arguments are the instruction's last operands.
    constexpr auto EdgeArgumentCount = std::size_t { 1 };

    /// How many context values a function threads: the step countdown, and memory 0's base and bound
    /// when the module has a memory (they are the first rows of ContextValueTable).
    constexpr auto MemoryContextValues = intermediate::ContextValueTable.size();
    static_assert(intermediate::ContextValueTable[0].value == ContextValue::StepCountdown,
                  "the step countdown is the first context value, threaded by every function");

    /// The kind of an open construct.
    enum class ConstructKind : uint8_t
    {
        Function, ///< The function body: a branch to it returns.
        Block,    ///< A block: a branch goes to its end.
        Loop,     ///< A loop: a branch goes to its header, polling.
        If,       ///< An if: a branch goes to its end.
    };

    /// Whether an if's else arm has begun.
    enum class ElseArm : uint8_t
    {
        Pending, ///< Not yet: the then arm is being translated.
        Begun,   ///< The else instruction was reached.
    };

    /// Whether the code being translated is reachable.
    enum class Reach : uint8_t
    {
        Unreachable, ///< Not: what follows until the construct's else or end emits nothing.
        Reachable,   ///< It is.
    };

    /// Whether a merge position's incoming values differ.
    enum class Merge : uint8_t
    {
        Same,    ///< Every edge passes the same value: no parameter.
        Differs, ///< Some differ: a parameter.
    };

    /// One open construct of the body.
    struct Frame
    {
        ConstructKind kind = ConstructKind::Function; ///< What it is.
        uint32_t bodyIndex = 0;                       ///< Its opening instruction; 0 for the function.
        uint32_t ordinal = Unnumbered;                ///< Its LoopAssignments ordinal; none for the function.
        uint32_t height = 0;         ///< The operand stack's height at entry, params excluded.
        uint32_t typesBegin = 0;     ///< Its params, then its results, in _frameTypes.
        uint32_t paramCount = 0;     ///< How many params it takes.
        uint32_t resultCount = 0;    ///< How many results it leaves.
        uint32_t label = Unnumbered; ///< The block a branch to it goes to: a loop's header, a block's end.
        uint32_t elseBlock = Unnumbered;         ///< An if's else arm.
        uint32_t edges = Unnumbered;             ///< The first forward edge to its label; none yet.
        uint32_t snapshotBegin = 0;              ///< An if's entry state in _snapshots: params, set, context.
        std::optional<ValueId> storedAtEntry {}; ///< An if's _storedCountdown at entry.
        ElseArm elseArm = ElseArm::Pending;      ///< An if's arm.
    };

    /// A forward branch to a block's or an if's end, whose arguments the end's seal may narrow.
    struct Edge
    {
        uint32_t instruction = 0;   ///< The jump or branch.
        uint32_t next = Unnumbered; ///< The next edge to the same end.
    };

    /// The refusal @p code of @p function, with what was requested of which @p limit.
    [[nodiscard]] std::unexpected<CompileError> refusal(CompileErrorCode code,
                                                        uint32_t function,
                                                        uint64_t requested = 0,
                                                        uint64_t limit = 0) noexcept
    {
        return std::unexpected { CompileError {
            .code = code, .function = function, .requested = requested, .limit = limit } };
    }

    /// The IR type of a WebAssembly value type, which the compiler must compile (checkValueType()).
    [[nodiscard]] std::expected<Type, CompileError> irTypeOf(binary::ValueType const& type, uint32_t function)
    {
        if (auto checked = checkValueType(type, function); !checked)
            return std::unexpected { checked.error() };
        switch (type.kind)
        {
            case binary::ValueKind::I32: return Type::I32;
            case binary::ValueKind::I64: return Type::I64;
            case binary::ValueKind::F32: return Type::F32;
            case binary::ValueKind::F64: return Type::F64;
            case binary::ValueKind::Ref: return Type::Ref;
            case binary::ValueKind::V128: break;
        }
        core::unreachable(); // checkValueType() refuses a v128
    }

    /// @p value as an immediate.
    template <typename Enum>
    [[nodiscard]] constexpr uint64_t immediateOf(Enum value) noexcept
    {
        return uint64_t { std::to_underlying(value) };
    }

    /// The translation of one function.
    class Translation
    {
      public:
        Translation(TranslationInput const& input,
                    LoopAssignments const& assignments,
                    std::pmr::memory_resource& memory);

        /// Translates the body.
        [[nodiscard]] std::expected<Function, CompileError> run();

      private:
        using Outcome = std::expected<void, CompileError>;

        // --- the entry and the instructions
        [[nodiscard]] Outcome enter();
        [[nodiscard]] Outcome translateInstruction(uint32_t index);
        [[nodiscard]] Outcome numeric(binary::Instruction const& instruction, Operation operation);
        void checkDivision(NumericCheck check, ValueId lhs, ValueId rhs);
        void checkTruncation(Operation operation, ValueId operand);
        [[nodiscard]] Outcome call(binary::Instruction const& instruction);
        void select();

        // --- control
        [[nodiscard]] Outcome open(ConstructKind kind, uint32_t index);
        [[nodiscard]] Outcome openLoop(Frame& frame);
        void openIf(Frame& frame, uint32_t index, ValueId condition);
        void beginElse();
        [[nodiscard]] Outcome end();
        [[nodiscard]] Outcome seal();
        [[nodiscard]] Outcome branch(uint32_t depth);
        [[nodiscard]] Outcome branchIf(uint32_t index, uint32_t depth);
        [[nodiscard]] Outcome branchTable(binary::Instruction const& instruction);
        void emitReturn();
        void fillEdge(BlockId edge, uint32_t depth);
        void forwardEdge(Frame& frame);
        void forwardBranch(Frame& frame, ValueId condition, BlockId continuation);
        [[nodiscard]] uint32_t skipDeadCode(uint32_t index) noexcept;

        // --- helpers
        [[nodiscard]] Frame& frameAt(uint32_t depth) noexcept { return _frames[_frames.size() - 1 - depth]; }
        [[nodiscard]] std::span<Type const> paramTypes(Frame const& frame) const noexcept;
        [[nodiscard]] std::span<Type const> resultTypes(Frame const& frame) const noexcept;
        [[nodiscard]] std::span<uint32_t const> setOf(Frame const& frame) const noexcept;
        [[nodiscard]] uint64_t widthOf(Frame const& frame, uint32_t values) const noexcept;
        [[nodiscard]] std::span<ValueId const> mergeArguments(Frame const& frame, uint32_t values);
        [[nodiscard]] BlockId labelOf(Frame& frame);
        [[nodiscard]] Outcome reserve(uint64_t operands, uint64_t immediates) const;
        [[nodiscard]] Outcome checkBlockParameters(uint64_t count) const;
        [[nodiscard]] std::expected<void, CompileError> pushTypes(std::span<binary::ValueType const> types);
        [[nodiscard]] std::optional<binary::BranchLikelihood> hintAt(uint32_t index) noexcept;
        [[nodiscard]] ValueId emitValue(Operation operation,
                                        Type type,
                                        std::span<ValueId const> operands,
                                        std::span<uint64_t const> immediates = {});
        void emitEffect(Operation operation,
                        std::span<ValueId const> operands,
                        std::span<uint64_t const> immediates = {});
        [[nodiscard]] ValueId constant(Type type, uint64_t bits);
        [[nodiscard]] ValueId zeroOf(Type type, binary::HeapType const& heap);
        void loadContext();
        void storeCountdown();
        void storeCountdownForTrap();
        void trapIf(ValueId condition, runtime::TrapCode code);
        [[nodiscard]] ValueId pop() noexcept;
        [[nodiscard]] std::span<ValueId const> top(uint32_t count) const noexcept;
        void switchTo(BlockId block) noexcept { _builder.switchTo(block); }
        [[nodiscard]] BlockId createBlock(Hotness hotness = Hotness::Normal);

        TranslationInput const& _input;
        binary::Module const& _module;
        binary::Function const& _code;
        binary::FuncType const& _type;
        std::span<binary::Instruction const> _body;
        LoopAssignments const& _assignments;
        uint32_t _functionIndex;
        uint64_t _frameCost;
        std::pmr::memory_resource& _memory;
        Function _function;
        Builder _builder;
        std::pmr::vector<ValueId> _stack;
        std::pmr::vector<ValueId> _locals;
        std::pmr::vector<Type> _localTypes;
        std::pmr::vector<Frame> _frames;
        std::pmr::vector<Type> _frameTypes;
        std::pmr::vector<ValueId> _snapshots;
        std::pmr::vector<Edge> _edges;
        std::pmr::vector<ValueId> _arguments;
        std::pmr::vector<Merge> _merges;
        std::pmr::vector<uint32_t> _edgeList; ///< The sealed end's edges, by instruction.
        std::pmr::vector<Type> _resultScratch;
        std::pmr::vector<uint32_t> _edgeStamps; ///< Per depth, the br_table that last made its edge.
        std::pmr::vector<uint32_t> _edgeBlocks; ///< Per depth, that edge's block.
        std::pmr::vector<uint32_t> _edgeDepths; ///< The current br_table's distinct depths.
        std::array<ValueId, MemoryContextValues> _context {};
        uint32_t _contextCount;
        std::optional<ValueId> _storedCountdown; ///< The countdown the execution state holds, if known.
        std::array<std::optional<ValueId>, intermediate::TypeTable.size()> _zeros {};
        std::array<std::optional<ValueId>, std::to_underlying(binary::AbstractHeapType::NoExn) + 1> _nulls {};
        std::span<binary::BranchHint const> _hints;
        std::size_t _hintCursor = 0;
        Reach _reach = Reach::Reachable;
        uint32_t _nextOrdinal = 0;
        uint32_t _tableStamp = 0;
    };

    Translation::Translation(TranslationInput const& input,
                             LoopAssignments const& assignments,
                             std::pmr::memory_resource& memory):
        _input { input },
        _module { input.module.module() },
        _code { _module.code[input.codeIndex] },
        _type { _module.types[_module.functions[input.codeIndex]] },
        _body { _code.body },
        _assignments { assignments },
        _functionIndex { input.index.importedFunctionCount() + input.codeIndex },
        _frameCost { input.module.frameCost(input.codeIndex) },
        _memory { memory },
        _function { memory },
        _builder { _function },
        _stack(&memory),
        _locals(&memory),
        _localTypes(&memory),
        _frames(&memory),
        _frameTypes(&memory),
        _snapshots(&memory),
        _edges(&memory),
        _arguments(&memory),
        _merges(&memory),
        _edgeList(&memory),
        _resultScratch(&memory),
        _edgeStamps(&memory),
        _edgeBlocks(&memory),
        _edgeDepths(&memory),
        _contextCount { input.index.memoryCount() > 0 ? static_cast<uint32_t>(MemoryContextValues) : 1U },
        _storedCountdown {}
    {
        // The hints of this function: a run of the module's, which are ordered by function.
        auto const& hints = _module.branchHints;
        auto const first = std::ranges::lower_bound(
            hints, _functionIndex, {}, [](binary::BranchHint const& hint) { return hint.function; });
        auto const last = std::ranges::upper_bound(
            first, hints.end(), _functionIndex, {}, [](binary::BranchHint const& hint) {
                return hint.function;
            });
        _hints = std::span { first, last };
    }

    std::expected<Function, CompileError> Translation::run()
    {
        if (auto entered = enter(); !entered)
            return std::unexpected { entered.error() };
        auto const& limits = _input.limits;
        auto index = uint32_t { 0 };
        auto visited = uint64_t { 0 };
        while (index < _body.size())
        {
            if (visited++ % CancellationInterval == 0
                && _input.cancellation.state() == CancellationState::Cancelled)
                return refusal(CompileErrorCode::Cancelled, _input.codeIndex);
            if (_reach == Reach::Unreachable)
                index = skipDeadCode(index);
            if (auto translated = translateInstruction(index); !translated)
                return std::unexpected { translated.error() };
            if (_function.instructionCount() > limits.maxFunctionInstructions)
                return refusal(CompileErrorCode::TooManyInstructions,
                               _input.codeIndex,
                               _function.instructionCount(),
                               limits.maxFunctionInstructions);
            ++index;
        }
        Require(_frames.empty());
        return std::move(_function);
    }

    uint32_t Translation::skipDeadCode(uint32_t index) noexcept
    {
        // Dead code runs to the innermost construct's else, while its then arm is translated, or to its end.
        auto const& frame = _frames.back();
        auto target = static_cast<uint32_t>(_body.size() - 1);
        if (frame.kind != ConstructKind::Function)
        {
            auto const& opening = _body[frame.bodyIndex];
            auto const hasElse = frame.kind == ConstructKind::If && opening.elseIndex != opening.endIndex;
            target = hasElse && frame.elseArm == ElseArm::Pending ? opening.elseIndex : opening.endIndex;
        }
        Require(target >= index);
        _nextOrdinal = _assignments.firstAtOrAfter(target);
        return target;
    }

    Translation::Outcome Translation::enter()
    {
        auto const entry = createBlock();
        switchTo(entry);
        for (auto const& parameter: _type.params)
        {
            auto const type = irTypeOf(parameter, _input.codeIndex);
            if (!type)
                return std::unexpected { type.error() };
            _locals.push_back(_builder.addParameter(entry, *type));
            _localTypes.push_back(*type);
        }
        if (auto reserved =
                reserve(_type.params.size() + _code.localCount, uint64_t { 2 } * _code.localCount);
            !reserved)
            return reserved;

        // The callee's entry, in the reference's order: the poll, then the frame (depth, stack, native).
        emitEffect(Operation::PollStored, {}, std::array { immediateOf(runtime::PollSite::FunctionEntry) });
        emitEffect(Operation::EnterFrame, {}, std::array { _frameCost });
        loadContext();
        for (auto const& run: _code.locals)
        {
            auto const type = irTypeOf(run.type, _input.codeIndex);
            if (!type)
                return std::unexpected { type.error() };
            auto const zero = zeroOf(*type, run.type.heap);
            _locals.insert(_locals.end(), run.count, zero);
            _localTypes.insert(_localTypes.end(), run.count, *type);
        }

        auto frame = Frame { .kind = ConstructKind::Function,
                             .bodyIndex = 0,
                             .typesBegin = 0,
                             .resultCount = static_cast<uint32_t>(_type.results.size()) };
        if (auto pushed = pushTypes(_type.results); !pushed)
            return pushed;
        _frames.push_back(frame);
        return {};
    }

    Translation::Outcome Translation::translateInstruction(uint32_t index)
    {
        using binary::Opcode;
        auto const& instruction = _body[index];
        if (auto checked = checkOpcode(instruction.opcode, _input.codeIndex); !checked)
            return checked;
        switch (instruction.opcode)
        {
            case Opcode::Unreachable:
                storeCountdownForTrap();
                _builder.trap(runtime::TrapCode::Unreachable);
                _reach = Reach::Unreachable;
                return {};
            case Opcode::Nop: return {};
            case Opcode::Block: return open(ConstructKind::Block, index);
            case Opcode::Loop: return open(ConstructKind::Loop, index);
            case Opcode::If: return open(ConstructKind::If, index);
            case Opcode::Else:
                if (_reach == Reach::Reachable)
                {
                    if (auto reserved = reserve(widthOf(_frames.back(), _frames.back().resultCount), 2);
                        !reserved)
                        return reserved;
                    forwardEdge(_frames.back());
                }
                beginElse();
                return {};
            case Opcode::End: return end();
            case Opcode::Br: return branch(instruction.a);
            case Opcode::BrIf: return branchIf(index, instruction.a);
            case Opcode::BrTable: return branchTable(instruction);
            case Opcode::Return: return branch(static_cast<uint32_t>(_frames.size() - 1));
            case Opcode::Call: return call(instruction);
            case Opcode::Drop: std::ignore = pop(); return {};
            case Opcode::Select:
            case Opcode::SelectTyped: select(); return {};
            case Opcode::LocalGet: _stack.push_back(_locals[instruction.a]); return {};
            case Opcode::LocalSet: _locals[instruction.a] = pop(); return {};
            case Opcode::LocalTee: _locals[instruction.a] = _stack.back(); return {};
            case Opcode::GlobalGet: {
                auto const type = irTypeOf(_input.index.globalType(instruction.a).type, _input.codeIndex);
                if (!type)
                    return std::unexpected { type.error() };
                _stack.push_back(emitValue(Operation::LoadGlobal,
                                           *type,
                                           {},
                                           std::array { uint64_t { instruction.a }, immediateOf(*type) }));
                return {};
            }
            case Opcode::GlobalSet: {
                auto const value = pop();
                emitEffect(
                    Operation::StoreGlobal, std::array { value }, std::array { uint64_t { instruction.a } });
                return {};
            }
            case Opcode::RefNull: {
                if (auto type = irTypeOf(instruction.type, _input.codeIndex); !type)
                    return std::unexpected { type.error() };
                _stack.push_back(emitValue(Operation::RefNull,
                                           Type::Ref,
                                           {},
                                           std::array { immediateOf(instruction.type.heap.kind) }));
                return {};
            }
            case Opcode::RefIsNull: {
                auto const reference = pop();
                _stack.push_back(emitValue(Operation::RefIsNull, Type::I32, std::array { reference }));
                return {};
            }
            case Opcode::RefFunc:
                _stack.push_back(
                    emitValue(Operation::RefFunc, Type::Ref, {}, std::array { uint64_t { instruction.a } }));
                return {};
            default: break;
        }
        if (auto const operation = numericOperationOf(instruction.opcode))
            return numeric(instruction, *operation);
        core::todo("memory, table, bulk and indirect-call instructions: P5 Task 6");
    }

    Translation::Outcome Translation::numeric(binary::Instruction const& instruction, Operation operation)
    {
        auto const& row = intermediate::operationRow(operation);
        auto const resultType = intermediate::fixedTypeOf(row.result);
        Require(resultType.has_value());
        if (row.immediates == intermediate::ImmediateSchema::Value)
        {
            _stack.push_back(emitValue(operation, *resultType, {}, std::array { instruction.c }));
            return {};
        }
        auto const count = intermediate::operandSignatureRow(row.operands).count;
        auto operands = std::array<ValueId, 2> {};
        std::ranges::copy(top(count), operands.begin());
        auto const used = std::span { operands }.first(count);
        auto const rule = numericRuleOf(operation);
        switch (rule.check)
        {
            case NumericCheck::None: break;
            case NumericCheck::ZeroDivisor:
            case NumericCheck::ZeroDivisorOrOverflow: checkDivision(rule.check, used[0], used[1]); break;
            case NumericCheck::Truncation: checkTruncation(operation, used[0]); break;
        }
        auto result = emitValue(operation, *resultType, used);
        if (rule.nan == NanResult::Canonicalized && _input.mode == runtime::ExecutionMode::Deterministic)
            result = emitValue(Operation::CanonicalizeNan, *resultType, std::array { result });
        _stack.resize(_stack.size() - count);
        _stack.push_back(result);
        return {};
    }

    void Translation::checkDivision(NumericCheck check, ValueId lhs, ValueId rhs)
    {
        auto const& integer = checkOperationsOf(_function.typeOf(rhs));
        storeCountdownForTrap();
        trapIf(emitValue(integer.isZero, Type::I32, std::array { rhs }),
               runtime::TrapCode::IntegerDivideByZero);
        if (check != NumericCheck::ZeroDivisorOrOverflow)
            return;
        auto const minimum = constant(integer.type, integer.minimum);
        auto const minusOne = constant(integer.type, integer.minusOne);
        auto const isMinimum = emitValue(integer.equal, Type::I32, std::array { lhs, minimum });
        auto const isMinusOne = emitValue(integer.equal, Type::I32, std::array { rhs, minusOne });
        trapIf(emitValue(Operation::I32And, Type::I32, std::array { isMinimum, isMinusOne }),
               runtime::TrapCode::IntegerOverflow);
    }

    void Translation::checkTruncation(Operation operation, ValueId operand)
    {
        auto const bounds = truncationBoundsOf(operation);
        Require(bounds.has_value());
        auto const& floating = checkOperationsOf(_function.typeOf(operand));
        storeCountdownForTrap();
        trapIf(emitValue(floating.equal, Type::I32, std::array { operand, operand }),
               runtime::TrapCode::InvalidConversionToInteger);
        auto const lower = constant(floating.type, bounds->lower);
        auto const upper = constant(floating.type, bounds->upper);
        auto const aboveLower = emitValue(floating.greater, Type::I32, std::array { operand, lower });
        auto const belowUpper = emitValue(floating.less, Type::I32, std::array { operand, upper });
        auto const inRange = emitValue(Operation::I32And, Type::I32, std::array { aboveLower, belowUpper });
        trapIf(emitValue(Operation::I32Eqz, Type::I32, std::array { inRange }),
               runtime::TrapCode::IntegerOverflow);
    }

    Translation::Outcome Translation::call(binary::Instruction const& instruction)
    {
        auto const callee = instruction.a;
        auto const& type = _module.types[_input.index.functionType(callee)];
        _resultScratch.clear();
        for (auto const& result: type.results)
        {
            auto const resultType = irTypeOf(result, _input.codeIndex);
            if (!resultType)
                return std::unexpected { resultType.error() };
            _resultScratch.push_back(*resultType);
        }
        for (auto const& parameter: type.params)
            if (auto checked = irTypeOf(parameter, _input.codeIndex); !checked)
                return std::unexpected { checked.error() };
        if (auto reserved = reserve(type.params.size() + type.results.size(), 0); !reserved)
            return reserved;
        auto const arguments = static_cast<uint32_t>(type.params.size());
        storeCountdown();
        auto const operation =
            callee < _input.index.importedFunctionCount() ? Operation::CallImport : Operation::Call;
        auto const results =
            _builder.emit(operation, _resultScratch, top(arguments), std::array { uint64_t { callee } });
        _stack.resize(_stack.size() - arguments);
        _stack.insert(_stack.end(), results.begin(), results.end());
        loadContext();
        return {};
    }

    void Translation::select()
    {
        auto const condition = pop();
        auto const second = pop();
        auto const first = pop();
        _stack.push_back(
            emitValue(Operation::Select, _function.typeOf(first), std::array { first, second, condition }));
    }

    Translation::Outcome Translation::open(ConstructKind kind, uint32_t index)
    {
        auto const& instruction = _body[index];
        auto const blockType = instruction.blockType();
        auto params = std::span<binary::ValueType const> {};
        auto results = std::span<binary::ValueType const> {};
        switch (blockType.kind)
        {
            case binary::BlockType::Kind::Empty: break;
            case binary::BlockType::Kind::Value: results = std::span { &instruction.type, 1 }; break;
            case binary::BlockType::Kind::TypeIndex:
                params = _module.types[blockType.typeIndex].params;
                results = _module.types[blockType.typeIndex].results;
                break;
        }
        auto condition = std::optional<ValueId> {};
        if (kind == ConstructKind::If)
            condition = pop();
        auto frame = Frame { .kind = kind,
                             .bodyIndex = index,
                             .ordinal = _nextOrdinal++,
                             .height = static_cast<uint32_t>(_stack.size() - params.size()),
                             .typesBegin = static_cast<uint32_t>(_frameTypes.size()),
                             .paramCount = static_cast<uint32_t>(params.size()),
                             .resultCount = static_cast<uint32_t>(results.size()) };
        if (auto pushed = pushTypes(params); !pushed)
            return pushed;
        if (auto pushed = pushTypes(results); !pushed)
            return pushed;
        switch (kind)
        {
            case ConstructKind::Loop:
                if (auto opened = openLoop(frame); !opened)
                    return opened;
                break;
            case ConstructKind::If:
                if (auto reserved = reserve(widthOf(frame, frame.paramCount), 8); !reserved)
                    return reserved;
                openIf(frame, index, *condition);
                break;
            case ConstructKind::Block:
            case ConstructKind::Function: break;
        }
        _frames.push_back(frame);
        return {};
    }

    Translation::Outcome Translation::openLoop(Frame& frame)
    {
        auto const width = widthOf(frame, frame.paramCount);
        if (auto checked = checkBlockParameters(width); !checked)
            return checked;
        if (auto reserved = reserve(4 * width, 2); !reserved)
            return reserved;
        auto const header = createBlock();
        _builder.jump(header, mergeArguments(frame, frame.paramCount));
        switchTo(header);
        // Its parameters, in the merge order: the loop's params, its set, the context values.
        auto const params = paramTypes(frame);
        for (auto const [position, type]: core::views::enumerate(params))
            _stack[frame.height + position] = _builder.addParameter(header, type);
        for (auto const local: setOf(frame))
            _locals[local] = _builder.addParameter(header, _localTypes[local]);
        for (auto const position: std::views::iota(uint32_t { 0 }, _contextCount))
            _context[position] =
                _builder.addParameter(header, intermediate::ContextValueTable[position].type);
        _storedCountdown.reset();
        frame.label = std::to_underlying(header);
        return {};
    }

    void Translation::openIf(Frame& frame, uint32_t index, ValueId condition)
    {
        auto const hint = hintAt(index);
        auto const thenArm =
            createBlock(hint == binary::BranchLikelihood::Unlikely ? Hotness::Cold : Hotness::Normal);
        auto const elseArm =
            createBlock(hint == binary::BranchLikelihood::Likely ? Hotness::Cold : Hotness::Normal);
        // The state the else arm starts from: the params, the if's set and the context values.
        frame.snapshotBegin = static_cast<uint32_t>(_snapshots.size());
        auto const entry = mergeArguments(frame, frame.paramCount);
        _snapshots.insert(_snapshots.end(), entry.begin(), entry.end());
        frame.storedAtEntry = _storedCountdown;
        frame.elseBlock = std::to_underlying(elseArm);
        _builder.branch(condition, thenArm, {}, elseArm, {});
        switchTo(thenArm);
    }

    void Translation::beginElse()
    {
        auto& frame = _frames.back();
        Require(frame.kind == ConstructKind::If);
        auto const snapshot = std::span { _snapshots }.subspan(frame.snapshotBegin);
        _stack.resize(frame.height);
        _stack.insert(_stack.end(), snapshot.begin(), snapshot.begin() + frame.paramCount);
        auto position = std::size_t { frame.paramCount };
        for (auto const local: setOf(frame))
            _locals[local] = snapshot[position++];
        for (auto const value: std::views::iota(uint32_t { 0 }, _contextCount))
            _context[value] = snapshot[position++];
        _storedCountdown = frame.storedAtEntry;
        frame.elseArm = ElseArm::Begun;
        switchTo(BlockId { frame.elseBlock });
        _reach = Reach::Reachable;
    }

    Translation::Outcome Translation::end()
    {
        auto& frame = _frames.back();
        switch (frame.kind)
        {
            case ConstructKind::Function:
                if (_reach == Reach::Reachable)
                {
                    if (auto reserved = reserve(frame.resultCount, 0); !reserved)
                        return reserved;
                    emitReturn();
                }
                _frames.pop_back();
                _reach = Reach::Unreachable;
                return {};
            case ConstructKind::Loop: {
                auto const done = frame;
                _frames.pop_back();
                _frameTypes.resize(done.typesBegin);
                if (_reach == Reach::Unreachable)
                    _stack.resize(done.height);
                return {};
            }
            case ConstructKind::If:
                if (frame.elseArm == ElseArm::Pending)
                {
                    // No else: the else arm passes the if's entry state to its end.
                    if (_reach == Reach::Reachable)
                    {
                        if (auto reserved = reserve(widthOf(frame, frame.resultCount), 2); !reserved)
                            return reserved;
                        forwardEdge(frame);
                    }
                    beginElse();
                }
                break;
            case ConstructKind::Block: break;
        }
        return seal();
    }

    Translation::Outcome Translation::seal()
    {
        // The fallthrough is one more edge to the end.
        auto const width = widthOf(_frames.back(), _frames.back().resultCount);
        if (auto reserved = reserve((3 * width) + 4, 2); !reserved)
            return reserved;
        if (_reach == Reach::Reachable)
            forwardEdge(_frames.back());
        auto const frame = _frames.back();
        _frames.pop_back();
        auto const resultTypes = std::pmr::vector<Type>(
            _frameTypes.begin() + frame.typesBegin + frame.paramCount,
            _frameTypes.begin() + frame.typesBegin + frame.paramCount + frame.resultCount,
            &_memory);
        _frameTypes.resize(frame.typesBegin);
        _snapshots.resize(frame.kind == ConstructKind::If ? frame.snapshotBegin : _snapshots.size());
        _stack.resize(frame.height);
        if (frame.edges == Unnumbered)
        {
            _reach = Reach::Unreachable; // no path reaches the end
            return {};
        }

        // Which positions differ between the edges; their values are each edge's last operands.
        auto const argumentsOf = [this](uint32_t index) {
            auto const& instruction = _function.instruction(index);
            auto const count = _function.immediates(instruction.immediates)[EdgeArgumentCount];
            return _function.operands(instruction.operands).last(static_cast<std::size_t>(count));
        };
        _edgeList.clear();
        auto edge = frame.edges;
        while (edge != Unnumbered)
        {
            _edgeList.push_back(_edges[edge].instruction);
            edge = _edges[edge].next;
        }
        auto const first = argumentsOf(_edgeList.front());
        Require(first.size() == width);
        _merges.assign(width, Merge::Same);
        for (auto const instruction: std::span { _edgeList }.subspan(1))
            for (auto const [position, value]: core::views::enumerate(argumentsOf(instruction)))
                if (value != first[position])
                    _merges[position] = Merge::Differs;
        auto const parameters = static_cast<uint64_t>(std::ranges::count(_merges, Merge::Differs));
        if (auto checked = checkBlockParameters(parameters); !checked)
            return checked;

        // The merged values: a parameter where they differ, the common value where they do not.
        auto const label = BlockId { frame.label };
        auto const set = setOf(frame);
        _arguments.assign(first.begin(), first.end());
        auto const typeAt = [&](std::size_t position) {
            if (position < frame.resultCount)
                return resultTypes[position];
            if (position < frame.resultCount + set.size())
                return _localTypes[set[position - frame.resultCount]];
            return intermediate::ContextValueTable[position - frame.resultCount - set.size()].type;
        };
        for (auto const [position, merge]: core::views::enumerate(_merges))
            if (merge == Merge::Differs)
                _arguments[position] = _builder.addParameter(label, typeAt(position));

        // Each edge keeps only the arguments of the parameters, in place.
        for (auto const index: _edgeList)
        {
            auto& instruction = _function.instruction(index);
            auto const arguments = _function.operands(instruction.operands).last(width);
            auto kept = std::size_t { 0 };
            for (auto const [position, merge]: core::views::enumerate(_merges))
                if (merge == Merge::Differs)
                    arguments[kept++] = arguments[position];
            _function.immediates(instruction.immediates)[EdgeArgumentCount] = kept;
            instruction.operands.count -= static_cast<uint32_t>(width - kept);
        }

        switchTo(label);
        auto position = std::size_t { 0 };
        for ([[maybe_unused]] auto const result: std::views::iota(uint32_t { 0 }, frame.resultCount))
            _stack.push_back(_arguments[position++]);
        for (auto const local: set)
            _locals[local] = _arguments[position++];
        for (auto const value: std::views::iota(uint32_t { 0 }, _contextCount))
            _context[value] = _arguments[position++];
        _storedCountdown.reset();
        _reach = Reach::Reachable;
        return {};
    }

    Translation::Outcome Translation::branch(uint32_t depth)
    {
        auto& frame = frameAt(depth);
        auto const values = frame.kind == ConstructKind::Loop ? frame.paramCount : frame.resultCount;
        if (auto reserved = reserve(widthOf(frame, values), 8); !reserved)
            return reserved;
        switch (frame.kind)
        {
            case ConstructKind::Function: emitReturn(); break;
            case ConstructKind::Loop: {
                auto const edge = createBlock();
                _builder.jump(edge, {});
                fillEdge(edge, depth);
                break;
            }
            case ConstructKind::Block:
            case ConstructKind::If: forwardEdge(frame); break;
        }
        _reach = Reach::Unreachable;
        return {};
    }

    Translation::Outcome Translation::branchIf(uint32_t index, uint32_t depth)
    {
        auto const condition = pop();
        auto& frame = frameAt(depth);
        auto const values = frame.kind == ConstructKind::Loop ? frame.paramCount : frame.resultCount;
        if (auto reserved = reserve(widthOf(frame, values), 12); !reserved)
            return reserved;
        auto const hint = hintAt(index);
        auto const continuation =
            createBlock(hint == binary::BranchLikelihood::Likely ? Hotness::Cold : Hotness::Normal);
        auto const takenHotness =
            hint == binary::BranchLikelihood::Unlikely ? Hotness::Cold : Hotness::Normal;
        if ((frame.kind == ConstructKind::Block || frame.kind == ConstructKind::If)
            && takenHotness == Hotness::Normal)
            forwardBranch(frame, condition, continuation);
        else
        {
            // A loop's back edge polls, a return leaves the frame, and a cold forward edge is a block of
            // its own: each in a block only the taken branch enters.
            auto const edge = createBlock(takenHotness);
            _builder.branch(condition, edge, {}, continuation, {});
            fillEdge(edge, depth);
        }
        switchTo(continuation);
        return {};
    }

    Translation::Outcome Translation::branchTable(binary::Instruction const& instruction)
    {
        auto const index = pop();
        auto const labels = binary::labelsOf(_code, instruction);
        // One edge block per distinct target, made once however often the table names it.
        if (_edgeStamps.size() < _frames.size())
        {
            _edgeStamps.resize(_frames.size(), Unnumbered);
            _edgeBlocks.resize(_frames.size(), Unnumbered);
        }
        ++_tableStamp;
        _edgeDepths.clear();
        auto demand = uint64_t { labels.size() };
        for (auto const depth: labels)
            if (_edgeStamps[depth] != _tableStamp)
            {
                _edgeStamps[depth] = _tableStamp;
                _edgeDepths.push_back(depth);
                auto const& frame = frameAt(depth);
                demand +=
                    widthOf(frame, frame.kind == ConstructKind::Loop ? frame.paramCount : frame.resultCount);
            }
        if (auto reserved = reserve(demand, (2 * labels.size()) + (8 * _edgeDepths.size())); !reserved)
            return reserved;
        for (auto const depth: _edgeDepths)
            _edgeBlocks[depth] = std::to_underlying(createBlock());
        auto targets = std::pmr::vector<BranchTo>(&_memory);
        targets.reserve(labels.size() - 1);
        for (auto const depth: labels.first(labels.size() - 1))
            targets.push_back(BranchTo { .block = BlockId { _edgeBlocks[depth] } });
        _builder.switchOn(index, targets, BranchTo { .block = BlockId { _edgeBlocks[labels.back()] } });
        for (auto const depth: _edgeDepths)
            fillEdge(BlockId { _edgeBlocks[depth] }, depth);
        _reach = Reach::Unreachable;
        return {};
    }

    void Translation::fillEdge(BlockId edge, uint32_t depth)
    {
        switchTo(edge);
        auto& frame = frameAt(depth);
        switch (frame.kind)
        {
            case ConstructKind::Function: emitReturn(); break;
            case ConstructKind::Loop: {
                auto const countdown = _context[0];
                auto const polled = emitValue(Operation::Poll,
                                              Type::I64,
                                              std::array { countdown },
                                              std::array { immediateOf(runtime::PollSite::LoopBackEdge) });
                _context[0] = polled;
                _builder.jump(BlockId { frame.label }, mergeArguments(frame, frame.paramCount));
                _context[0] = countdown;
                break;
            }
            case ConstructKind::Block:
            case ConstructKind::If: forwardEdge(frame); break;
        }
    }

    void Translation::forwardEdge(Frame& frame)
    {
        auto const label = labelOf(frame);
        _builder.jump(label, mergeArguments(frame, frame.resultCount));
        _edges.push_back(Edge { .instruction = _function.instructionCount() - 1, .next = frame.edges });
        frame.edges = static_cast<uint32_t>(_edges.size() - 1);
    }

    void Translation::forwardBranch(Frame& frame, ValueId condition, BlockId continuation)
    {
        // The taken target's arguments are the branch's last operands: the continuation takes none.
        auto const label = labelOf(frame);
        _builder.branch(condition, label, mergeArguments(frame, frame.resultCount), continuation, {});
        _edges.push_back(Edge { .instruction = _function.instructionCount() - 1, .next = frame.edges });
        frame.edges = static_cast<uint32_t>(_edges.size() - 1);
    }

    void Translation::emitReturn()
    {
        auto const results = _frames.front().resultCount;
        storeCountdown();
        emitEffect(Operation::LeaveFrame, {}, std::array { _frameCost });
        _builder.ret(top(results));
    }

    std::span<Type const> Translation::paramTypes(Frame const& frame) const noexcept
    {
        return std::span { _frameTypes }.subspan(frame.typesBegin, frame.paramCount);
    }

    std::span<Type const> Translation::resultTypes(Frame const& frame) const noexcept
    {
        return std::span { _frameTypes }.subspan(frame.typesBegin + frame.paramCount, frame.resultCount);
    }

    std::span<uint32_t const> Translation::setOf(Frame const& frame) const noexcept
    {
        if (frame.ordinal == Unnumbered)
            return {};
        return _assignments.locals(frame.ordinal);
    }

    uint64_t Translation::widthOf(Frame const& frame, uint32_t values) const noexcept
    {
        return uint64_t { values } + setOf(frame).size() + _contextCount;
    }

    std::span<ValueId const> Translation::mergeArguments(Frame const& frame, uint32_t values)
    {
        _arguments.clear();
        auto const stacked = top(values);
        _arguments.insert(_arguments.end(), stacked.begin(), stacked.end());
        for (auto const local: setOf(frame))
            _arguments.push_back(_locals[local]);
        _arguments.insert(_arguments.end(), _context.begin(), _context.begin() + _contextCount);
        return _arguments;
    }

    BlockId Translation::labelOf(Frame& frame)
    {
        if (frame.label == Unnumbered)
            frame.label = std::to_underlying(createBlock());
        return BlockId { frame.label };
    }

    Translation::Outcome Translation::reserve(uint64_t operands, uint64_t immediates) const
    {
        auto const limit = std::min(_input.limits.maxPoolEntries, intermediate::MaxPoolEntries);
        auto const operandDemand = _function.operandPoolSize() + operands + FixedEmission;
        auto const immediateDemand = _function.immediatePoolSize() + immediates + FixedEmission;
        auto const demand = std::max(operandDemand, immediateDemand);
        if (demand > limit)
            return refusal(CompileErrorCode::PoolTooLarge, _input.codeIndex, demand, limit);
        return {};
    }

    Translation::Outcome Translation::checkBlockParameters(uint64_t count) const
    {
        if (count > _input.limits.maxBlockParameters)
            return refusal(CompileErrorCode::TooManyBlockParameters,
                           _input.codeIndex,
                           count,
                           _input.limits.maxBlockParameters);
        return {};
    }

    std::expected<void, CompileError> Translation::pushTypes(std::span<binary::ValueType const> types)
    {
        for (auto const& type: types)
        {
            auto const irType = irTypeOf(type, _input.codeIndex);
            if (!irType)
                return std::unexpected { irType.error() };
            _frameTypes.push_back(*irType);
        }
        return {};
    }

    std::optional<binary::BranchLikelihood> Translation::hintAt(uint32_t index) noexcept
    {
        while (_hintCursor < _hints.size() && _hints[_hintCursor].instruction < index)
            ++_hintCursor;
        if (_hintCursor < _hints.size() && _hints[_hintCursor].instruction == index)
            return _hints[_hintCursor].likelihood;
        return std::nullopt;
    }

    ValueId Translation::emitValue(Operation operation,
                                   Type type,
                                   std::span<ValueId const> operands,
                                   std::span<uint64_t const> immediates)
    {
        return _builder.emit(operation, std::array { type }, operands, immediates)[0];
    }

    void Translation::emitEffect(Operation operation,
                                 std::span<ValueId const> operands,
                                 std::span<uint64_t const> immediates)
    {
        std::ignore = _builder.emit(operation, {}, operands, immediates);
    }

    ValueId Translation::constant(Type type, uint64_t bits)
    {
        return emitValue(checkOperationsOf(type).constant, type, {}, std::array { bits });
    }

    ValueId Translation::zeroOf(Type type, binary::HeapType const& heap)
    {
        // One zero per type, one null per heap, defined in the entry block, which dominates every use.
        if (type == Type::Ref)
        {
            auto& null = _nulls[std::to_underlying(heap.kind)];
            if (!null)
                null = emitValue(Operation::RefNull, Type::Ref, {}, std::array { immediateOf(heap.kind) });
            return *null;
        }
        auto& zero = _zeros[std::to_underlying(type)];
        if (!zero)
            zero = constant(type, 0);
        return *zero;
    }

    void Translation::loadContext()
    {
        for (auto const position: std::views::iota(uint32_t { 0 }, _contextCount))
        {
            auto const& row = intermediate::ContextValueTable[position];
            _context[position] = emitValue(Operation::LoadContext,
                                           row.type,
                                           {},
                                           std::array { immediateOf(row.value), immediateOf(row.type) });
        }
        _storedCountdown = _context[0];
    }

    void Translation::storeCountdown()
    {
        emitEffect(Operation::StoreContext,
                   std::array { _context[0] },
                   std::array { immediateOf(ContextValue::StepCountdown) });
        _storedCountdown = _context[0];
    }

    void Translation::storeCountdownForTrap()
    {
        if (_storedCountdown != _context[0])
            storeCountdown();
    }

    void Translation::trapIf(ValueId condition, runtime::TrapCode code)
    {
        emitEffect(Operation::TrapIf, std::array { condition }, std::array { immediateOf(code) });
    }

    ValueId Translation::pop() noexcept
    {
        Require(!_stack.empty());
        auto const value = _stack.back();
        _stack.pop_back();
        return value;
    }

    std::span<ValueId const> Translation::top(uint32_t count) const noexcept
    {
        Require(count <= _stack.size());
        return std::span { _stack }.last(count);
    }

    BlockId Translation::createBlock(Hotness hotness)
    {
        auto const block = _builder.createBlock();
        _function.block(block).hotness = hotness;
        return block;
    }
} // namespace

std::expected<intermediate::Function, CompileError> translate(TranslationInput const& input,
                                                              std::pmr::memory_resource& memory)
{
    auto const& module = input.module.module();
    Require(input.codeIndex < module.code.size());
    auto const& code = module.code[input.codeIndex];
    if (auto const units = binary::bodyUnits(code); units > input.limits.maxFunctionBodyUnits)
        return refusal(
            CompileErrorCode::FunctionTooLarge, input.codeIndex, units, input.limits.maxFunctionBodyUnits);
    auto assignments = assignedLocalsPerLoop(code.body, input.limits.maxTotalBlockParameters, memory);
    if (!assignments)
    {
        auto error = assignments.error();
        error.function = input.codeIndex;
        return std::unexpected { error };
    }
    return Translation { input, *assignments, memory }.run();
}

} // namespace isoline::compiler::translation
