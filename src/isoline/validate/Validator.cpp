// SPDX-License-Identifier: Apache-2.0
#include <isoline/validate/Validator.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/MemoryAccess.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/validate/ValidationError.hpp>

namespace isoline::validate
{

namespace
{
    using binary::ConstExpr;
    using binary::ExternKind;
    using binary::Function;
    using binary::FuncType;
    using binary::GlobalType;
    using binary::Instruction;
    using binary::LocalRun;
    using binary::MemoryAccess;
    using binary::MemoryAccessKind;
    using binary::MemoryType;
    using binary::Module;
    using binary::Mutability;
    using binary::Opcode;
    using binary::OpcodeSignature;
    using binary::SegmentMode;
    using binary::TableType;
    using binary::ValueKind;
    using binary::ValueType;

    /// What a check found wrong, before its caller adds where: the error, and the index it names.
    struct Fault
    {
        ValidationErrorCode code;        ///< What went wrong.
        std::optional<uint32_t> subject; ///< The unknown or undeclared index, if it names one.
    };

    /// The outcome of a check whose position its caller knows.
    using Check = std::expected<void, Fault>;

    /// @return The failure of a check with @p code, naming no index.
    [[nodiscard]] std::unexpected<Fault> fail(ValidationErrorCode code) noexcept
    {
        return std::unexpected { Fault { .code = code, .subject = std::nullopt } };
    }

    /// @return The failure @p fault, passed on.
    [[nodiscard]] std::unexpected<Fault> fail(Fault fault) noexcept
    {
        return std::unexpected { fault };
    }

    /// @return The failure of a check with @p code about the index @p subject.
    [[nodiscard]] std::unexpected<Fault> failNaming(ValidationErrorCode code, uint32_t subject) noexcept
    {
        return std::unexpected { Fault { .code = code, .subject = subject } };
    }

    /// @return Success when @p holds, else the failure @p code.
    [[nodiscard]] Check check(bool holds, ValidationErrorCode code) noexcept
    {
        if (!holds)
            return fail(code);
        return {};
    }

    /// @return Success when @p index is below @p count, else the failure @p code naming it.
    [[nodiscard]] Check checkIndex(uint64_t index, std::size_t count, ValidationErrorCode code) noexcept
    {
        if (index >= count)
            return failNaming(code, static_cast<uint32_t>(index));
        return {};
    }

    /// An error outside every function body.
    [[nodiscard]] std::unexpected<ValidationError> moduleError(Fault fault) noexcept
    {
        return std::unexpected { ValidationError { fault.code, NoFunction, NoInstruction, fault.subject } };
    }

    /// Lifts a module-level check to a module-level error.
    [[nodiscard]] std::expected<void, ValidationError> atModule(Check const& result) noexcept
    {
        if (!result)
            return moduleError(result.error());
        return {};
    }

    // --- Constant expressions -----------------------------------------------------------------

    /// An instruction a constant expression may hold, and the proposal that allows it there.
    struct ConstantRow
    {
        Opcode opcode;                   ///< The instruction.
        std::optional<Feature> required; ///< The proposal that makes it constant; nullopt: always.
    };

    /// Every instruction a constant expression may hold (WebAssembly 2.0, plus extended-const).
    /// global.get is further restricted to immutable imported globals.
    constexpr auto ConstantTable = std::to_array<ConstantRow>({
        { .opcode = Opcode::I32Const, .required = std::nullopt },
        { .opcode = Opcode::I64Const, .required = std::nullopt },
        { .opcode = Opcode::F32Const, .required = std::nullopt },
        { .opcode = Opcode::F64Const, .required = std::nullopt },
        { .opcode = Opcode::RefNull, .required = std::nullopt },
        { .opcode = Opcode::RefFunc, .required = std::nullopt },
        { .opcode = Opcode::GlobalGet, .required = std::nullopt },
        { .opcode = Opcode::End, .required = std::nullopt },
        { .opcode = Opcode::I32Add, .required = Feature::ExtendedConst },
        { .opcode = Opcode::I32Sub, .required = Feature::ExtendedConst },
        { .opcode = Opcode::I32Mul, .required = Feature::ExtendedConst },
        { .opcode = Opcode::I64Add, .required = Feature::ExtendedConst },
        { .opcode = Opcode::I64Sub, .required = Feature::ExtendedConst },
        { .opcode = Opcode::I64Mul, .required = Feature::ExtendedConst },
    });

    // --- Limits -------------------------------------------------------------------------------

    /// The largest size a memory with 32-bit addresses may declare: 65536 pages of 64 KiB.
    constexpr auto MaxMemoryPages = uint64_t { 65536 };

    /// The largest size a table with 32-bit indices may declare.
    constexpr auto MaxTableSize = uint64_t { 0xFFFF'FFFF };

    /// Checks @p limits against @p range, as the interpreter's check_limits does: both bounds first,
    /// then their order.
    [[nodiscard]] Check checkLimits(binary::Limits const& limits,
                                    uint64_t range,
                                    ValidationErrorCode tooLarge)
    {
        return check(limits.min <= range, tooLarge)
            .and_then([&] { return check(!limits.max || *limits.max <= range, tooLarge); })
            .and_then([&] {
                return check(!limits.max || limits.min <= *limits.max,
                             ValidationErrorCode::SizeMinimumMustNotBeGreaterThanMaximum);
            });
    }

    [[nodiscard]] Check checkTable(TableType const& table)
    {
        // A u32 table size cannot exceed the range, so the interpreter's "table size must be at most
        // 2^32-1" is unreachable for the tables P1 decodes; only the order of the bounds can fail.
        return checkLimits(
            table.limits, MaxTableSize, ValidationErrorCode::SizeMinimumMustNotBeGreaterThanMaximum);
    }

    [[nodiscard]] Check checkMemory(MemoryType const& memory)
    {
        return checkLimits(memory.limits, MaxMemoryPages, ValidationErrorCode::MemorySizeMustBeAtMost4GiB);
    }

    // --- The context ----------------------------------------------------------------------------

    /// The module's index spaces, as instructions and segments refer to them.
    struct Context
    {
        Module const& module;
        FeatureSet features;
        std::vector<uint32_t> functionTypes;     ///< The type index of every function, imports first.
        std::vector<TableType> tables;           ///< Every table, imports first.
        std::vector<MemoryType> memories;        ///< Every memory, imports first.
        std::vector<GlobalType> globals;         ///< Every global, imports first.
        std::size_t importedGlobalCount = 0;     ///< How many of globals are imported.
        std::vector<uint32_t> declaredFunctions; ///< The functions ref.func may name; sorted, unique.

        [[nodiscard]] FuncType const& functionType(uint32_t function) const
        {
            return module.types[functionTypes[function]];
        }

        [[nodiscard]] std::expected<TableType const*, Fault> table(uint32_t index) const
        {
            if (index >= tables.size())
                return failNaming(ValidationErrorCode::UnknownTable, index);
            return &tables[index];
        }

        [[nodiscard]] Check memory(uint32_t index) const
        {
            return checkIndex(index, memories.size(), ValidationErrorCode::UnknownMemory);
        }

        [[nodiscard]] Check function(uint32_t index) const
        {
            return checkIndex(index, functionTypes.size(), ValidationErrorCode::UnknownFunction);
        }

        [[nodiscard]] Check type(uint32_t index) const
        {
            return checkIndex(index, module.types.size(), ValidationErrorCode::UnknownType);
        }
    };

    /// Every function index a constant expression names with ref.func.
    void collectReferences(ConstExpr const& expression, std::vector<uint32_t>& out)
    {
        for (auto const& instruction: expression)
            if (instruction.opcode == Opcode::RefFunc)
                out.push_back(instruction.a);
    }

    /// The functions ref.func may name in a body (spec 2.0, C.refs): every function index the module
    /// mentions outside its function bodies and its start section.
    [[nodiscard]] std::vector<uint32_t> declaredFunctionsOf(Module const& module)
    {
        auto declared = std::vector<uint32_t> {};
        for (auto const& global: module.globals)
            collectReferences(global.init, declared);
        for (auto const& entry: module.exports)
            if (entry.kind == ExternKind::Func)
                declared.push_back(entry.index);
        for (auto const& segment: module.elements)
        {
            collectReferences(segment.offset, declared);
            if (auto const* indices = std::get_if<std::vector<uint32_t>>(&segment.items))
                declared.insert(declared.end(), indices->begin(), indices->end());
            else
                for (auto const& item: std::get<std::vector<ConstExpr>>(segment.items))
                    collectReferences(item, declared);
        }
        for (auto const& segment: module.data)
            collectReferences(segment.offset, declared);
        std::ranges::sort(declared);
        auto const duplicates = std::ranges::unique(declared);
        declared.erase(duplicates.begin(), duplicates.end());
        return declared;
    }

    // --- Bodies -------------------------------------------------------------------------------

    /// What a body is validated against: its code, its locals, its results and the globals it sees.
    struct Body
    {
        std::span<Instruction const> instructions; ///< Its instructions, ending with the final end.
        Function const& sideTables;                ///< The function holding its br_table/select runs.
        std::span<ValueType const> params;         ///< Its parameters: its first locals.
        std::span<LocalRun const> localRuns;       ///< Its declared locals.
        std::span<ValueType const> results;        ///< Its result types.
        std::span<GlobalType const> globals;       ///< The globals it may refer to.
    };

    /// Where a body failed: what went wrong, at which of its instructions.
    struct BodyError
    {
        Fault fault;
        uint32_t instruction;
    };

    /// An operand stack entry: a value type, or nullopt for the spec's Unknown, the type of an
    /// operand popped from the polymorphic stack of unreachable code.
    using Operand = std::optional<ValueType>;

    /// Whether the rest of a control frame can be reached.
    enum class Reachability : uint8_t
    {
        Reachable,
        Unreachable,
    };

    /// A control frame of the validation algorithm.
    struct Frame
    {
        Opcode opcode;                      ///< Block, Loop, If, Else; Block for a body's own frame.
        std::span<ValueType const> params;  ///< The types its block starts with.
        std::span<ValueType const> results; ///< The types its block ends with.
        std::size_t height;                 ///< The operand stack height at its start.
        Reachability reachability;          ///< Whether its remainder is reachable.

        /// The types a branch to this frame carries: a loop's are its params, any other's its results.
        [[nodiscard]] std::span<ValueType const> labelTypes() const noexcept
        {
            return opcode == Opcode::Loop ? params : results;
        }
    };

    /// A block's parameter and result types.
    struct BlockSignature
    {
        std::span<ValueType const> params;
        std::span<ValueType const> results;
    };

    [[nodiscard]] constexpr bool isNumber(Operand const& operand) noexcept
    {
        return !operand || (operand->kind != ValueKind::V128 && operand->kind != ValueKind::Ref);
    }

    [[nodiscard]] constexpr bool isVector(Operand const& operand) noexcept
    {
        return !operand || operand->kind == ValueKind::V128;
    }

    [[nodiscard]] constexpr bool isReference(Operand const& operand) noexcept
    {
        return !operand || operand->kind == ValueKind::Ref;
    }

    /// The spec appendix's validation algorithm over one body or constant expression.
    ///
    /// Its stacks are kept between bodies so that validating a module allocates them once.
    class BodyValidator
    {
      public:
        explicit BodyValidator(Context const& context) noexcept: _context { context } {}

        /// Types @p body.
        /// @param body The body and what it is typed against.
        /// @return Nothing when it is valid, else the first error and the instruction it is at.
        [[nodiscard]] std::expected<void, BodyError> validate(Body const& body)
        {
            _operands.clear();
            _frames.clear();
            _localRunEnds.clear();
            auto end = uint64_t { 0 };
            for (auto const& run: body.localRuns)
            {
                end += run.count;
                _localRunEnds.push_back(end);
            }

            pushFrame(Opcode::Block, BlockSignature { .params = {}, .results = body.results });
            for (auto const [index, instruction]: core::views::enumerate(body.instructions))
            {
                // The decoder resolves block structure: the final end closes the body's frame.
                Require(!_frames.empty());
                if (auto const result = step(body, instruction); !result)
                    return std::unexpected { BodyError { .fault = result.error(),
                                                         .instruction = static_cast<uint32_t>(index) } };
            }
            Require(_frames.empty());
            return {};
        }

      private:
        // --- The operand and control stacks, as the spec appendix names them ---

        void push(Operand const& operand) { _operands.push_back(operand); }

        void pushAll(std::span<ValueType const> types)
        {
            for (auto const& type: types)
                push(type);
        }

        [[nodiscard]] std::expected<Operand, Fault> pop()
        {
            auto const& frame = _frames.back();
            if (_operands.size() == frame.height)
            {
                if (frame.reachability == Reachability::Unreachable)
                    return Operand {};
                return fail(ValidationErrorCode::TypeMismatch);
            }
            auto const operand = _operands.back();
            _operands.pop_back();
            return operand;
        }

        [[nodiscard]] std::expected<Operand, Fault> pop(ValueType const& expected)
        {
            return pop().and_then([&](Operand const& actual) -> std::expected<Operand, Fault> {
                if (actual && *actual != expected)
                    return fail(ValidationErrorCode::TypeMismatch);
                return actual;
            });
        }

        [[nodiscard]] Check popAll(std::span<ValueType const> types)
        {
            for (auto const& type: types | std::views::reverse)
                if (auto const popped = pop(type); !popped)
                    return fail(popped.error());
            return {};
        }

        /// Pops @p types and pushes back what was popped, keeping the more precise operand types.
        [[nodiscard]] Check popAndRestore(std::span<ValueType const> types)
        {
            _restored.clear();
            for (auto const& type: types | std::views::reverse)
            {
                auto const popped = pop(type);
                if (!popped)
                    return fail(popped.error());
                _restored.push_back(*popped);
            }
            for (auto const& operand: _restored | std::views::reverse)
                push(operand);
            return {};
        }

        void pushFrame(Opcode opcode, BlockSignature const& signature)
        {
            _frames.push_back(Frame { .opcode = opcode,
                                      .params = signature.params,
                                      .results = signature.results,
                                      .height = _operands.size(),
                                      .reachability = Reachability::Reachable });
            pushAll(signature.params);
        }

        [[nodiscard]] std::expected<Frame, Fault> popFrame()
        {
            return popAll(_frames.back().results).and_then([this]() -> std::expected<Frame, Fault> {
                auto const frame = _frames.back();
                if (_operands.size() != frame.height)
                    return fail(ValidationErrorCode::TypeMismatch);
                _frames.pop_back();
                return frame;
            });
        }

        void markUnreachable()
        {
            auto& frame = _frames.back();
            _operands.resize(frame.height);
            frame.reachability = Reachability::Unreachable;
        }

        // --- Lookups ---

        [[nodiscard]] std::expected<Frame const*, Fault> label(uint32_t depth) const
        {
            if (depth >= _frames.size())
                return failNaming(ValidationErrorCode::UnknownLabel, depth);
            return &_frames[_frames.size() - 1 - depth];
        }

        [[nodiscard]] std::expected<ValueType, Fault> local(Body const& body, uint32_t index) const
        {
            if (index < body.params.size())
                return body.params[index];
            auto const declared = uint64_t { index } - body.params.size();
            auto const run = std::ranges::upper_bound(_localRunEnds, declared);
            if (run == _localRunEnds.end())
                return failNaming(ValidationErrorCode::UnknownLocal, index);
            return body.localRuns[static_cast<std::size_t>(run - _localRunEnds.begin())].type;
        }

        [[nodiscard]] std::expected<GlobalType const*, Fault> global(Body const& body, uint32_t index) const
        {
            if (index >= body.globals.size())
                return failNaming(ValidationErrorCode::UnknownGlobal, index);
            return &body.globals[index];
        }

        [[nodiscard]] Check elem(uint32_t index) const
        {
            return checkIndex(
                index, _context.module.elements.size(), ValidationErrorCode::UnknownElemSegment);
        }

        [[nodiscard]] Check data(uint32_t index) const
        {
            return checkIndex(index, _context.module.data.size(), ValidationErrorCode::UnknownDataSegment);
        }

        [[nodiscard]] std::expected<BlockSignature, Fault> blockSignature(
            Instruction const& instruction) const
        {
            switch (instruction.blockKind)
            {
                case binary::BlockType::Kind::Empty: return BlockSignature {};
                case binary::BlockType::Kind::Value:
                    return BlockSignature { .params = {}, .results = std::span { &instruction.type, 1 } };
                case binary::BlockType::Kind::TypeIndex: break; // checked below the switch
            }
            return _context.type(instruction.a).transform([&] {
                auto const& type = _context.module.types[instruction.a];
                return BlockSignature { .params = type.params, .results = type.results };
            });
        }

        // --- Instructions ---

        /// Pops the operands of @p row and pushes its result.
        [[nodiscard]] Check applySignature(binary::SignatureRow const& row)
        {
            for (auto const kind: row.paramKinds() | std::views::reverse)
                if (auto const popped = pop(ValueType { .kind = kind }); !popped)
                    return fail(popped.error());
            for (auto const kind: row.resultKinds())
                push(ValueType { .kind = kind });
            return {};
        }

        [[nodiscard]] Check memoryAccess(MemoryAccess const& row, Instruction const& instruction)
        {
            // MemArg::memory is in b, its alignment exponent in a.
            auto const value = ValueType { .kind = row.value };
            return _context.memory(instruction.b)
                .and_then([&] {
                    return check(instruction.a <= row.naturalAlignment(),
                                 ValidationErrorCode::AlignmentMustNotBeLarger);
                })
                .and_then([&]() -> Check {
                    if (row.access == MemoryAccessKind::Store)
                        if (auto const popped = pop(value); !popped)
                            return fail(popped.error());
                    if (auto const popped = pop(binary::I32); !popped)
                        return fail(popped.error());
                    if (row.access == MemoryAccessKind::Load)
                        push(value);
                    return {};
                });
        }

        /// Pops @p count i32 operands, at most three.
        [[nodiscard]] Check popI32s(std::size_t count)
        {
            static constexpr auto I32s = std::to_array<ValueType>({ binary::I32, binary::I32, binary::I32 });
            return popAll(std::span { I32s }.first(count));
        }

        [[nodiscard]] Check pushAfter(std::expected<Operand, Fault> const& popped, ValueType const& type)
        {
            if (!popped)
                return fail(popped.error());
            push(type);
            return {};
        }

        [[nodiscard]] Check step(Body const& body, Instruction const& instruction)
        {
            auto const& info = binary::opcodeInfo(instruction.opcode);
            if (info.signature != OpcodeSignature::Special)
                return applySignature(binary::signatureInfo(info.signature));
            if (auto const* access = binary::memoryAccessOf(instruction.opcode))
                return memoryAccess(*access, instruction);
            return special(body, instruction);
        }

        /// Types the instructions SignatureTable and MemoryAccessTable leave to code.
        [[nodiscard]] Check special(Body const& body, Instruction const& instruction)
        {
            switch (instruction.opcode)
            {
                case Opcode::Unreachable: markUnreachable(); return {};

                case Opcode::Block:
                case Opcode::Loop:
                    return blockSignature(instruction).and_then([&](BlockSignature const& signature) {
                        return popAll(signature.params).transform([&] {
                            pushFrame(instruction.opcode, signature);
                        });
                    });
                case Opcode::If:
                    return blockSignature(instruction).and_then([&](BlockSignature const& signature) {
                        return pop(binary::I32)
                            .and_then([&](Operand const&) { return popAll(signature.params); })
                            .transform([&] { pushFrame(Opcode::If, signature); });
                    });
                case Opcode::Else:
                    return popFrame().transform([this](Frame const& frame) {
                        pushFrame(Opcode::Else,
                                  BlockSignature { .params = frame.params, .results = frame.results });
                    });
                case Opcode::End:
                    return popFrame().and_then([this](Frame const& frame) -> Check {
                        // An if without else passes its params through the missing else branch.
                        if (frame.opcode == Opcode::If && !std::ranges::equal(frame.params, frame.results))
                            return fail(ValidationErrorCode::TypeMismatch);
                        pushAll(frame.results);
                        return {};
                    });

                case Opcode::Br:
                    return label(instruction.a).and_then([this](Frame const* frame) {
                        return popAll(frame->labelTypes()).transform([this] { markUnreachable(); });
                    });
                case Opcode::BrIf:
                    // The label first: the reference interpreter reports an unknown label before
                    // a missing condition.
                    return label(instruction.a).and_then([this](Frame const* frame) {
                        return pop(binary::I32).and_then([this, frame](Operand const&) {
                            return popAll(frame->labelTypes()).transform([this, frame] {
                                pushAll(frame->labelTypes());
                            });
                        });
                    });
                case Opcode::BrTable: return brTable(body, instruction);
                case Opcode::Return: return popAll(body.results).transform([this] { markUnreachable(); });

                case Opcode::Call:
                    return _context.function(instruction.a).and_then([&] {
                        return call(_context.functionType(instruction.a));
                    });
                case Opcode::CallIndirect:
                    return _context.table(instruction.b)
                        .and_then([&](TableType const* table) {
                            return _context.type(instruction.a).and_then([table] {
                                return check(table->element == binary::FuncRef,
                                             ValidationErrorCode::TypeMismatch);
                            });
                        })
                        .and_then([this] { return pop(binary::I32); })
                        .and_then([&](Operand const&) { return call(_context.module.types[instruction.a]); });

                case Opcode::Drop: return pop().transform([](Operand const&) {});
                case Opcode::Select: return select();
                case Opcode::SelectTyped: {
                    auto const types = binary::selectTypesOf(body.sideTables, instruction);
                    if (types.size() != 1)
                        return fail(ValidationErrorCode::InvalidResultArity);
                    auto const& type = types.front();
                    return pop(binary::I32)
                        .and_then([&](Operand const&) { return pop(type); })
                        .and_then([&](Operand const&) { return pushAfter(pop(type), type); });
                }

                case Opcode::LocalGet:
                    return local(body, instruction.a).transform([this](ValueType const& type) {
                        push(type);
                    });
                case Opcode::LocalSet:
                    return local(body, instruction.a).and_then([this](ValueType const& type) {
                        return pop(type).transform([](Operand const&) {});
                    });
                case Opcode::LocalTee:
                    return local(body, instruction.a).and_then([this](ValueType const& type) {
                        return pushAfter(pop(type), type);
                    });
                case Opcode::GlobalGet:
                    return global(body, instruction.a).transform([this](GlobalType const* type) {
                        push(type->type);
                    });
                case Opcode::GlobalSet:
                    return global(body, instruction.a).and_then([this](GlobalType const* type) -> Check {
                        if (type->mutability != Mutability::Var)
                            return fail(ValidationErrorCode::GlobalIsImmutable);
                        return pop(type->type).transform([](Operand const&) {});
                    });

                case Opcode::TableGet:
                    return _context.table(instruction.a).and_then([this](TableType const* table) {
                        return pushAfter(pop(binary::I32), table->element);
                    });
                case Opcode::TableSet:
                    return _context.table(instruction.a).and_then([this](TableType const* table) {
                        return pop(table->element).and_then([this](Operand const&) { return popI32s(1); });
                    });
                case Opcode::TableSize:
                    return _context.table(instruction.a).transform([this](TableType const*) {
                        push(binary::I32);
                    });
                case Opcode::TableGrow:
                    return _context.table(instruction.a).and_then([this](TableType const* table) {
                        return pop(binary::I32)
                            .and_then([&](Operand const&) { return pop(table->element); })
                            .transform([this](Operand const&) { push(binary::I32); });
                    });
                case Opcode::TableFill:
                    return _context.table(instruction.a).and_then([this](TableType const* table) {
                        return pop(binary::I32)
                            .and_then([&](Operand const&) { return pop(table->element); })
                            .and_then([this](Operand const&) { return popI32s(1); });
                    });
                case Opcode::TableCopy:
                    // a is the destination, b the source.
                    return _context.table(instruction.a).and_then([&](TableType const* destination) {
                        return _context.table(instruction.b).and_then([&](TableType const* source) {
                            return check(destination->element == source->element,
                                         ValidationErrorCode::TypeMismatch)
                                .and_then([this] { return popI32s(3); });
                        });
                    });
                case Opcode::TableInit:
                    // a is the element segment, b the table; the interpreter looks the table up first.
                    return _context.table(instruction.b).and_then([&](TableType const* table) {
                        return elem(instruction.a).and_then([&] {
                            return check(table->element == _context.module.elements[instruction.a].type,
                                         ValidationErrorCode::TypeMismatch)
                                .and_then([this] { return popI32s(3); });
                        });
                    });
                case Opcode::ElemDrop: return elem(instruction.a);

                case Opcode::MemorySize:
                    return _context.memory(instruction.a).transform([this] { push(binary::I32); });
                case Opcode::MemoryGrow:
                    return _context.memory(instruction.a).and_then([this] {
                        return pushAfter(pop(binary::I32), binary::I32);
                    });
                case Opcode::MemoryFill:
                    return _context.memory(instruction.a).and_then([this] { return popI32s(3); });
                case Opcode::MemoryCopy:
                    return _context.memory(instruction.a)
                        .and_then([&] { return _context.memory(instruction.b); })
                        .and_then([this] { return popI32s(3); });
                case Opcode::MemoryInit:
                    // a is the data segment, b the memory; the interpreter looks the memory up first.
                    return _context.memory(instruction.b)
                        .and_then([&] { return data(instruction.a); })
                        .and_then([this] { return popI32s(3); });
                case Opcode::DataDrop: return data(instruction.a);

                case Opcode::RefNull: push(instruction.type); return {};
                case Opcode::RefIsNull:
                    return pop().and_then([this](Operand const& operand) -> Check {
                        if (!isReference(operand))
                            return fail(ValidationErrorCode::TypeMismatch);
                        push(binary::I32);
                        return {};
                    });
                case Opcode::RefFunc:
                    return _context.function(instruction.a)
                        .and_then([&] {
                            if (!std::ranges::binary_search(_context.declaredFunctions, instruction.a))
                                return Check { failNaming(ValidationErrorCode::UndeclaredFunctionReference,
                                                          instruction.a) };
                            return Check {};
                        })
                        .transform([this] { push(binary::FuncRef); });

                default: break; // uncovered(special-default)
            }
            // uncovered-begin(special-default)
            // Every Special opcode has a case above or a row in MemoryAccessTable.
            core::todo(binary::opcodeInfo(instruction.opcode).text);
        }
        // uncovered-end(special-default)

        [[nodiscard]] Check call(FuncType const& type)
        {
            return popAll(type.params).transform([&] { pushAll(type.results); });
        }

        [[nodiscard]] Check select()
        {
            return pop(binary::I32).and_then([this](Operand const&) -> Check {
                auto const first = pop();
                if (!first)
                    return fail(first.error());
                auto const second = pop();
                if (!second)
                    return fail(second.error());
                // Without a type annotation, select takes numbers or vectors only.
                if (!(isNumber(*first) && isNumber(*second)) && !(isVector(*first) && isVector(*second)))
                    return fail(ValidationErrorCode::TypeMismatch);
                if (*first && *second && **first != **second)
                    return fail(ValidationErrorCode::TypeMismatch);
                // The appendix pushes t1, or t2 when t1 is Unknown. An Unknown t1 was either popped at
                // an unreachable frame's floor or is an earlier select's Unknown result, which that
                // select pushed right at the floor; either way nothing typed lies beneath it, so t2 is
                // Unknown too and t1 is always the answer.
                push(*first);
                return {};
            });
        }

        [[nodiscard]] Check brTable(Body const& body, Instruction const& instruction)
        {
            auto const labels = binary::labelsOf(body.sideTables, instruction);
            auto const defaultLabel = label(labels.back());
            if (!defaultLabel)
                return fail(defaultLabel.error());
            auto const arity = (*defaultLabel)->labelTypes().size();
            return pop(binary::I32)
                .and_then([&](Operand const&) -> Check {
                    for (auto const depth: labels.first(labels.size() - 1))
                    {
                        auto const target = label(depth);
                        if (!target)
                            return fail(target.error());
                        if ((*target)->labelTypes().size() != arity)
                            return fail(ValidationErrorCode::TypeMismatch);
                        if (auto const result = popAndRestore((*target)->labelTypes()); !result)
                            return result;
                    }
                    return popAll((*defaultLabel)->labelTypes());
                })
                .transform([this] { markUnreachable(); });
        }

        Context const& _context;
        std::vector<Operand> _operands;
        std::vector<Frame> _frames;
        std::vector<uint64_t> _localRunEnds; ///< Per local run, the declared-local index past its end.
        std::vector<Operand> _restored;      ///< Scratch for popAndRestore.
    };

    // --- The module ---------------------------------------------------------------------------

    /// Validates one module: builds its context, then checks it in the interpreter's order.
    class ModuleValidator
    {
      public:
        ModuleValidator(Module const& module, FeatureSet features):
            _context { .module = module,
                       .features = features,
                       .functionTypes = {},
                       .tables = {},
                       .memories = {},
                       .globals = {},
                       .importedGlobalCount = 0,
                       .declaredFunctions = declaredFunctionsOf(module) },
            _bodies { _context }
        {
        }

        [[nodiscard]] std::expected<void, ValidationError> validate()
        {
            return checkImports()
                .and_then([this] { return checkFunctionTypes(); })
                .and_then([this] { return checkTypes(); })
                .and_then([this] { return checkGlobals(); })
                .and_then([this] { return checkTablesAndMemories(); })
                .and_then([this] { return checkElements(); })
                .and_then([this] { return checkData(); })
                .and_then([this] { return checkBodies(); })
                .and_then([this] { return checkStart(); })
                .and_then([this] { return checkExports(); })
                .and_then([this] { return checkCounts(); });
        }

      private:
        [[nodiscard]] Module const& module() const noexcept { return _context.module; }

        [[nodiscard]] std::expected<void, ValidationError> checkImports()
        {
            for (auto const& import: module().imports)
            {
                auto result = Check {};
                switch (import.kind)
                {
                    case ExternKind::Func:
                        result = _context.type(import.typeIndex);
                        _context.functionTypes.push_back(import.typeIndex);
                        break;
                    case ExternKind::Table:
                        result = checkTable(import.table);
                        _context.tables.push_back(import.table);
                        break;
                    case ExternKind::Memory:
                        result = checkMemory(import.memory);
                        _context.memories.push_back(import.memory);
                        break;
                    case ExternKind::Global:
                        _context.globals.push_back(import.global);
                        ++_context.importedGlobalCount;
                        break;
                    case ExternKind::Tag: result = fail(ValidationErrorCode::UnknownTag); break;
                }
                if (!result)
                    return moduleError(result.error());
            }
            return {};
        }

        [[nodiscard]] std::expected<void, ValidationError> checkFunctionTypes()
        {
            for (auto const typeIndex: module().functions)
            {
                if (auto const result = _context.type(typeIndex); !result)
                    return moduleError(result.error());
                _context.functionTypes.push_back(typeIndex);
            }
            for (auto const& global: module().globals)
                _context.globals.push_back(global.type);
            _context.tables.insert(_context.tables.end(), module().tables.begin(), module().tables.end());
            _context.memories.insert(
                _context.memories.end(), module().memories.begin(), module().memories.end());
            return {};
        }

        [[nodiscard]] std::expected<void, ValidationError> checkTypes() const
        {
            // Before multi-value, a function returns at most one value.
            if (_context.features.has(Feature::MultiValue))
                return {};
            return atModule(
                check(std::ranges::all_of(module().types,
                                          [](FuncType const& type) { return type.results.size() <= 1; }),
                      ValidationErrorCode::InvalidResultArity));
        }

        /// Types a constant expression of type @p type: constness first, as the interpreter checks
        /// it, then the stack algorithm over the globals constant expressions see, the imported ones.
        [[nodiscard]] Check checkConstant(ConstExpr const& expression, ValueType const& type)
        {
            for (auto const& instruction: expression)
            {
                auto const row = std::ranges::find(ConstantTable, instruction.opcode, &ConstantRow::opcode);
                if (row == ConstantTable.end() || (row->required && !_context.features.has(*row->required)))
                    return fail(ValidationErrorCode::ConstantExpressionRequired);
                if (instruction.opcode != Opcode::GlobalGet)
                    continue;
                if (instruction.a >= _context.importedGlobalCount)
                    return failNaming(ValidationErrorCode::UnknownGlobal, instruction.a);
                if (_context.globals[instruction.a].mutability != Mutability::Const)
                    return fail(ValidationErrorCode::ConstantExpressionRequired);
            }
            auto const body = Body {
                .instructions = expression,
                .sideTables = _constantSideTables,
                .params = {},
                .localRuns = {},
                .results = std::span { &type, 1 },
                .globals = std::span { _context.globals }.first(_context.importedGlobalCount),
            };
            return _bodies.validate(body).transform_error([](BodyError const& error) { return error.fault; });
        }

        [[nodiscard]] std::expected<void, ValidationError> checkGlobals()
        {
            for (auto const& global: module().globals)
                if (auto const result = checkConstant(global.init, global.type.type); !result)
                    return moduleError(result.error());
            return {};
        }

        [[nodiscard]] std::expected<void, ValidationError> checkTablesAndMemories() const
        {
            for (auto const& table: module().tables)
                if (auto const result = checkTable(table); !result)
                    return moduleError(result.error());
            for (auto const& memory: module().memories)
                if (auto const result = checkMemory(memory); !result)
                    return moduleError(result.error());
            return {};
        }

        [[nodiscard]] Check checkElement(binary::ElementSegment const& segment)
        {
            if (auto const* indices = std::get_if<std::vector<uint32_t>>(&segment.items))
            {
                // Each index stands for (ref.func index): a funcref.
                for (auto const index: *indices)
                    if (auto const result = _context.function(index); !result)
                        return result;
                if (auto const result =
                        check(segment.type == binary::FuncRef, ValidationErrorCode::TypeMismatch);
                    !result)
                    return result;
            }
            else
            {
                for (auto const& item: std::get<std::vector<ConstExpr>>(segment.items))
                    if (auto const result = checkConstant(item, segment.type); !result)
                        return result;
            }
            if (segment.mode != SegmentMode::Active)
                return {};
            return _context.table(segment.table).and_then([&](TableType const* table) {
                return checkConstant(segment.offset, binary::I32).and_then([&] {
                    return check(table->element == segment.type, ValidationErrorCode::TypeMismatch);
                });
            });
        }

        [[nodiscard]] std::expected<void, ValidationError> checkElements()
        {
            for (auto const& segment: module().elements)
                if (auto const result = checkElement(segment); !result)
                    return moduleError(result.error());
            return {};
        }

        [[nodiscard]] std::expected<void, ValidationError> checkData()
        {
            for (auto const& segment: module().data)
            {
                if (segment.mode != SegmentMode::Active)
                    continue;
                auto const result = _context.memory(segment.memory).and_then([&] {
                    return checkConstant(segment.offset, binary::I32);
                });
                if (!result)
                    return moduleError(result.error());
            }
            return {};
        }

        [[nodiscard]] std::expected<void, ValidationError> checkBodies()
        {
            auto const importedFunctions = _context.functionTypes.size() - module().functions.size();
            for (auto const [index, function]: core::views::enumerate(module().code))
            {
                auto const functionIndex = static_cast<uint32_t>(importedFunctions + index);
                auto const& type = _context.functionType(functionIndex);
                auto const body = Body { .instructions = function.body,
                                         .sideTables = function,
                                         .params = type.params,
                                         .localRuns = function.locals,
                                         .results = type.results,
                                         .globals = _context.globals };
                if (auto const result = _bodies.validate(body); !result)
                    return std::unexpected { ValidationError { result.error().fault.code,
                                                               functionIndex,
                                                               result.error().instruction,
                                                               result.error().fault.subject } };
            }
            return {};
        }

        [[nodiscard]] std::expected<void, ValidationError> checkStart() const
        {
            if (!module().start)
                return {};
            auto const index = *module().start;
            return atModule(_context.function(index).and_then([&] {
                auto const& type = _context.functionType(index);
                return check(type.params.empty() && type.results.empty(), ValidationErrorCode::StartFunction);
            }));
        }

        [[nodiscard]] Check checkExportIndex(binary::Export const& entry) const
        {
            switch (entry.kind)
            {
                case ExternKind::Func: return _context.function(entry.index);
                case ExternKind::Table: return _context.table(entry.index).transform([](TableType const*) {});
                case ExternKind::Memory: return _context.memory(entry.index);
                case ExternKind::Global:
                    return checkIndex(
                        entry.index, _context.globals.size(), ValidationErrorCode::UnknownGlobal);
                case ExternKind::Tag: break; // failed below the switch
            }
            return fail(ValidationErrorCode::UnknownTag);
        }

        /// The index of the first export, in declaration order, whose name an earlier one has.
        ///
        /// Found by sorting rather than hashing: export names are the module's to choose, so a hash
        /// set's worst case, every name in one bucket, would be theirs to pick too.
        /// @return Its index, or nullopt when every name is unique.
        [[nodiscard]] std::optional<std::size_t> firstDuplicateExport() const
        {
            auto const& exports = module().exports;
            if (exports.size() < 2)
                return std::nullopt;
            auto order = std::vector<std::size_t> {};
            order.reserve(exports.size());
            for (auto const index: std::views::iota(std::size_t { 0 }, exports.size()))
                order.push_back(index);
            // Stable, so that of equal names the earlier declaration sorts first.
            std::ranges::stable_sort(
                order, {}, [&](std::size_t index) -> std::string_view { return exports[index].name; });
            auto first = std::optional<std::size_t> {};
            for (auto const position: std::views::iota(std::size_t { 1 }, order.size()))
                if (exports[order[position]].name == exports[order[position - 1]].name)
                    first = std::min(first.value_or(order[position]), order[position]);
            return first;
        }

        [[nodiscard]] std::expected<void, ValidationError> checkExports() const
        {
            auto const duplicate = firstDuplicateExport();
            auto const& exports = module().exports;
            for (auto const index: std::views::iota(std::size_t { 0 }, exports.size()))
            {
                auto const result = checkExportIndex(exports[index]).and_then([&] {
                    return check(index != duplicate, ValidationErrorCode::DuplicateExportName);
                });
                if (!result)
                    return moduleError(result.error());
            }
            return {};
        }

        [[nodiscard]] std::expected<void, ValidationError> checkCounts() const
        {
            // WebAssembly 2.0 still allows one memory; reference types lifted the MVP's one table.
            return atModule(
                check(_context.memories.size() <= 1 || _context.features.has(Feature::MultiMemory),
                      ValidationErrorCode::MultipleMemories)
                    .and_then([this] {
                        return check(_context.tables.size() <= 1
                                         || _context.features.has(Feature::ReferenceTypes),
                                     ValidationErrorCode::MultipleTables);
                    }));
        }

        Context _context;
        BodyValidator _bodies;
        Function _constantSideTables; ///< Constant expressions hold no br_table or typed select.
    };

} // namespace

std::expected<void, ValidationError> validateModule(binary::Module const& module, FeatureSet const& features)
{
    return ModuleValidator { module, features }.validate();
}

} // namespace isoline::validate
