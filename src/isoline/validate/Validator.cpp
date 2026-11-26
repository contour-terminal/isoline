// SPDX-License-Identifier: Apache-2.0
#include <isoline/validate/Validator.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
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
#include <isoline/binary/TypeCanon.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/validate/Subtyping.hpp>
#include <isoline/validate/ValidationError.hpp>

namespace isoline::validate
{

namespace
{
    using binary::AddressType;
    using binary::ConstExpr;
    using binary::ExternKind;
    using binary::Function;
    using binary::FuncType;
    using binary::GlobalType;
    using binary::ImmediateKind;
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
    using binary::VectorShape;

    /// What a check found wrong, before its caller adds where: the error, and the index it names.
    struct Fault
    {
        ValidationErrorCode code;                ///< What went wrong.
        std::optional<uint32_t> subject;         ///< The unknown or undeclared index, if it names one.
        std::optional<OperandMismatch> mismatch; ///< The operands that did not match, if recorded.
    };

    /// The outcome of a check whose position its caller knows.
    using Check = std::expected<void, Fault>;

    /// @return The failure of a check with @p code, naming no index.
    [[nodiscard]] std::unexpected<Fault> fail(ValidationErrorCode code) noexcept
    {
        return std::unexpected { Fault { .code = code, .subject = std::nullopt, .mismatch = std::nullopt } };
    }

    /// @return The failure @p fault, passed on.
    [[nodiscard]] std::unexpected<Fault> fail(Fault fault) noexcept
    {
        return std::unexpected { std::move(fault) };
    }

    /// @return The failure of a check with @p code about the index @p subject.
    [[nodiscard]] std::unexpected<Fault> failNaming(ValidationErrorCode code, uint32_t subject) noexcept
    {
        return std::unexpected { Fault { .code = code, .subject = subject, .mismatch = std::nullopt } };
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

    /// The error @p fault is at @p instruction of function @p function.
    [[nodiscard]] ValidationError errorAt(Fault fault, uint32_t function, uint32_t instruction) noexcept
    {
        return ValidationError {
            fault.code, function, instruction, fault.subject, std::move(fault.mismatch)
        };
    }

    /// An error outside every function body.
    [[nodiscard]] std::unexpected<ValidationError> moduleError(Fault fault) noexcept
    {
        return std::unexpected { errorAt(std::move(fault), NoFunction, NoInstruction) };
    }

    /// Lifts a module-level check to a module-level error.
    [[nodiscard]] std::expected<void, ValidationError> atModule(Check const& result)
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
        { .opcode = Opcode::V128Const, .required = Feature::Simd },
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

    // --- Lanes --------------------------------------------------------------------------------

    /// An instruction with a lane index immediate, and how many lanes it may name.
    struct LaneRow
    {
        Opcode opcode;     ///< The instruction.
        uint8_t laneCount; ///< Its lane index must be below this.
    };

    /// Every instruction with a lane index immediate (LaneIndex or MemArgLane).
    constexpr auto LaneTable = std::to_array<LaneRow>({
        { .opcode = Opcode::I8x16ExtractLaneS, .laneCount = 16 },
        { .opcode = Opcode::I8x16ExtractLaneU, .laneCount = 16 },
        { .opcode = Opcode::I8x16ReplaceLane, .laneCount = 16 },
        { .opcode = Opcode::I16x8ExtractLaneS, .laneCount = 8 },
        { .opcode = Opcode::I16x8ExtractLaneU, .laneCount = 8 },
        { .opcode = Opcode::I16x8ReplaceLane, .laneCount = 8 },
        { .opcode = Opcode::I32x4ExtractLane, .laneCount = 4 },
        { .opcode = Opcode::I32x4ReplaceLane, .laneCount = 4 },
        { .opcode = Opcode::I64x2ExtractLane, .laneCount = 2 },
        { .opcode = Opcode::I64x2ReplaceLane, .laneCount = 2 },
        { .opcode = Opcode::F32x4ExtractLane, .laneCount = 4 },
        { .opcode = Opcode::F32x4ReplaceLane, .laneCount = 4 },
        { .opcode = Opcode::F64x2ExtractLane, .laneCount = 2 },
        { .opcode = Opcode::F64x2ReplaceLane, .laneCount = 2 },
        { .opcode = Opcode::V128Load8Lane, .laneCount = 16 },
        { .opcode = Opcode::V128Load16Lane, .laneCount = 8 },
        { .opcode = Opcode::V128Load32Lane, .laneCount = 4 },
        { .opcode = Opcode::V128Load64Lane, .laneCount = 2 },
        { .opcode = Opcode::V128Store8Lane, .laneCount = 16 },
        { .opcode = Opcode::V128Store16Lane, .laneCount = 8 },
        { .opcode = Opcode::V128Store32Lane, .laneCount = 4 },
        { .opcode = Opcode::V128Store64Lane, .laneCount = 2 },
    });

    static_assert(
        [] {
            auto rows = std::size_t { 0 };
            for (auto const& info: binary::OpcodeTable)
            {
                auto const isLaneImmediate =
                    info.immediate == ImmediateKind::LaneIndex || info.immediate == ImmediateKind::MemArgLane;
                auto const row = std::ranges::find(LaneTable, info.opcode, &LaneRow::opcode);
                if (isLaneImmediate != (row != LaneTable.end()))
                    return false;
                // A lane load or store moves one lane: its width times the lane count is 16 bytes.
                if (auto const* access = binary::memoryAccessOf(info.opcode); access && isLaneImmediate)
                    if (access->width * row->laneCount != 16)
                        return false;
                rows += isLaneImmediate ? 1 : 0;
            }
            return rows == LaneTable.size();
        }(),
        "LaneTable holds every lane-index instruction once, and agrees with MemoryAccessTable's widths");

    /// The number of lanes a shuffle's lane indices choose from: both operands' 16 bytes.
    constexpr auto ShuffleLaneCount = 32U;

    /// Checks that @p type, if it names a concrete type, names one below @p typeCount: of the
    /// module's types, or of those before the type it is part of.
    [[nodiscard]] Check checkTypeIndex(ValueType const& type, std::size_t typeCount) noexcept
    {
        if (!binary::isConcreteRef(type))
            return {};
        return checkIndex(type.heap.typeIndex, typeCount, ValidationErrorCode::UnknownType);
    }

    // --- Limits -------------------------------------------------------------------------------

    /// Checks @p limits against @p range, as the interpreter's check_limits does: both bounds first,
    /// then their order. A bound past @p range is @p tooLarge, naming @p boundRow, the row of the
    /// bound its message spells (AddressBoundTable for a table, MemoryBoundTable for a memory).
    [[nodiscard]] Check checkLimits(binary::Limits const& limits,
                                    uint64_t range,
                                    ValidationErrorCode tooLarge,
                                    uint32_t boundRow)
    {
        auto const fits = (limits.min <= range) && (!limits.max || *limits.max <= range);
        if (!fits)
            return failNaming(tooLarge, boundRow);
        return check(!limits.max || limits.min <= *limits.max,
                     ValidationErrorCode::SizeMinimumMustNotBeGreaterThanMaximum);
    }

    [[nodiscard]] Check checkTable(TableType const& table)
    {
        return checkLimits(table.limits,
                           binary::maxTableSize(table),
                           ValidationErrorCode::TableSizeMustBeAtMost,
                           static_cast<uint32_t>(table.limits.addressType));
    }

    /// The page sizes custom-page-sizes admits, as base-2 logarithms: 1 byte and 64 KiB.
    constexpr auto ValidPageSizesLog2 = std::to_array<uint8_t>({ 0, binary::DefaultPageSizeLog2 });

    [[nodiscard]] Check checkMemory(MemoryType const& memory)
    {
        if (std::ranges::find(ValidPageSizesLog2, memory.pageSizeLog2) == ValidPageSizesLog2.end())
            return fail(ValidationErrorCode::InvalidCustomPageSize);
        return checkLimits(memory.limits,
                           binary::maxPages(memory),
                           ValidationErrorCode::MemorySizeMustBeAtMost,
                           memoryBoundRow(memory.limits.addressType, memory.pageSizeLog2));
    }

    /// The largest memarg offset a memory of i32 addresses admits.
    constexpr auto MaxI32Offset = uint64_t { 0xFFFF'FFFF };

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
        std::vector<uint32_t> tagTypes;          ///< The type index of every tag, imports first.
        std::size_t importedGlobalCount = 0;     ///< How many of globals are imported.
        std::vector<uint32_t> declaredFunctions; ///< The functions ref.func may name; sorted, unique.
        std::vector<uint32_t> canonical;         ///< The canonical id of every type (canonicalTypeIds).

        /// Whether @p sub matches @p super, concrete types compared by their canonical ids.
        [[nodiscard]] bool matches(ValueType const& sub, ValueType const& super) const noexcept
        {
            return isSubtype(sub, super, canonical);
        }

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

        [[nodiscard]] std::expected<MemoryType const*, Fault> memory(uint32_t index) const
        {
            if (index >= memories.size())
                return failNaming(ValidationErrorCode::UnknownMemory, index);
            return &memories[index];
        }

        [[nodiscard]] Check function(uint32_t index) const
        {
            return checkIndex(index, functionTypes.size(), ValidationErrorCode::UnknownFunction);
        }

        [[nodiscard]] Check type(uint32_t index) const
        {
            return checkIndex(index, module.types.size(), ValidationErrorCode::UnknownType);
        }

        /// The function type of tag @p index: its params are the payload's types.
        [[nodiscard]] std::expected<FuncType const*, Fault> tag(uint32_t index) const
        {
            if (index >= tagTypes.size())
                return failNaming(ValidationErrorCode::UnknownTag, index);
            return &module.types[tagTypes[index]];
        }

        /// Checks a tag's type: a known function type without results.
        [[nodiscard]] Check tagType(uint32_t typeIndex) const
        {
            return type(typeIndex).and_then([&] {
                return check(module.types[typeIndex].results.empty(),
                             ValidationErrorCode::NonEmptyTagResultType);
            });
        }

        /// Checks that @p type names a type of the module, if it names one at all.
        [[nodiscard]] Check valueType(ValueType const& type) const
        {
            return checkTypeIndex(type, module.types.size());
        }

        /// The type ref.func @p function pushes: a non-null reference to its function type.
        [[nodiscard]] ValueType functionReference(uint32_t function) const
        {
            return binary::concreteRef(functionTypes[function], binary::Nullable::No);
        }
    };

    /// Every function index a constant expression names with ref.func.
    void collectReferences(ConstExpr const& expression, std::vector<uint32_t>& out)
    {
        for (auto const& instruction: expression)
            if (instruction.opcode == Opcode::RefFunc)
                out.push_back(instruction.a);
    }

    /// The functions ref.func may name in a body (C.refs): every function index the module mentions
    /// outside its function bodies and its start section, table initializers included.
    [[nodiscard]] std::vector<uint32_t> declaredFunctionsOf(Module const& module)
    {
        auto declared = std::vector<uint32_t> {};
        for (auto const& global: module.globals)
            collectReferences(global.init, declared);
        for (auto const& table: module.tables)
            collectReferences(table.init, declared);
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
        Opcode opcode;                      ///< Block, Loop, If, Else, TryTable; Block for a body's own.
        std::span<ValueType const> params;  ///< The types its block starts with.
        std::span<ValueType const> results; ///< The types its block ends with.
        std::size_t height;                 ///< The operand stack height at its start.
        std::size_t setLocalsHeight;        ///< How many locals were set at its start.
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

    /// (ref bot): what an Unknown operand is as a reference, a subtype of every reference type.
    constexpr auto BottomReference = ValueType {
        .kind = ValueKind::Ref,
        .nullable = binary::Nullable::No,
        .heap = { .kind = binary::AbstractHeapType::Bot },
    };

    /// @p reference without null: the type ref.as_non_null and br_on_null leave it with.
    [[nodiscard]] constexpr ValueType nonNullOf(ValueType reference) noexcept
    {
        reference.nullable = binary::Nullable::No;
        return reference;
    }

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
            startLocalInitialization();
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
                if (actual && !_context.matches(*actual, expected))
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

        /// Pops @p types at once, as valid.ml's pop does, and on a mismatch records the operands: those
        /// required, and the top ones of the block in their place, as many as there are up to as many
        /// as required, below which an unreachable block's polymorphic stack supplies Unknowns.
        [[nodiscard]] Check popRecording(std::span<ValueType const> types)
        {
            auto const& frame = _frames.back();
            auto const taken = std::min(types.size(), _operands.size() - frame.height);
            auto const supplied = frame.reachability == Reachability::Unreachable ? types.size() - taken : 0;
            auto const top = std::span { _operands }.last(taken);
            auto const matching =
                supplied + taken == types.size()
                && std::ranges::equal(
                    top, types.last(taken), [this](Operand const& actual, ValueType const& required) {
                        return !actual || _context.matches(*actual, required);
                    });
            if (!matching)
            {
                auto mismatch = OperandMismatch { .required = { types.begin(), types.end() },
                                                  .actual = std::vector<Operand>(supplied) };
                mismatch.actual.insert(mismatch.actual.end(), top.begin(), top.end());
                return fail(Fault { .code = ValidationErrorCode::TypeMismatch,
                                    .subject = std::nullopt,
                                    .mismatch = std::move(mismatch) });
            }
            _operands.resize(_operands.size() - taken);
            return {};
        }

        /// The reference on top of the stack, which ref.as_non_null and br_on_null take, without popping
        /// it: the reference interpreter's peek_ref. An Unknown operand, or none at all, is a (ref bot);
        /// only a top that is no reference fails, so that an empty stack fails at the pop, after the
        /// instruction's other checks.
        [[nodiscard]] std::expected<ValueType, Fault> peekReference() const
        {
            if (_operands.size() == _frames.back().height || !_operands.back())
                return BottomReference;
            if (_operands.back()->kind != ValueKind::Ref)
                return fail(ValidationErrorCode::TypeMismatch);
            return *_operands.back();
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
                                      .setLocalsHeight = _setLocals.size(),
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

        // --- Local initialization (typed function references) ---
        //
        // A local of a non-defaultable type starts unset; local.set and local.tee set it until the
        // end of the block they are in, and a block's else or end restores what was set at its
        // start. _setLocals is the stack of the locals set in the open blocks, each block keeping
        // its height (Frame::setLocalsHeight); _setStamps tells in O(1) whether one is set, by
        // holding _generation for it. The stamps are sized by the highest local index set so far
        // and never cleared between bodies: a new generation unsets all at once, so a body's cost
        // is that of its own sets, never of the locals it declares.

        void startLocalInitialization()
        {
            _setLocals.clear();
            ++_generation; // 64 bits: no validator lives through 2^64 bodies, so it never wraps
        }

        /// Whether local @p index, of @p type, is set: a parameter or a local with a default always is.
        [[nodiscard]] bool isSet(Body const& body, uint32_t index, ValueType const& type) const noexcept
        {
            if (index < body.params.size() || binary::isDefaultable(type))
                return true;
            return index < _setStamps.size() && _setStamps[index] == _generation;
        }

        /// Records @p index, a local of @p type, as set, unless it is already.
        void setLocal(Body const& body, uint32_t index, ValueType const& type)
        {
            if (isSet(body, index, type))
                return;
            if (index >= _setStamps.size())
                _setStamps.resize(std::size_t { index } + 1, 0);
            _setStamps[index] = _generation;
            _setLocals.push_back(index);
        }

        /// Unsets the locals set since @p height.
        void restoreLocals(std::size_t height)
        {
            for (auto const index: std::span { _setLocals }.subspan(height))
                _setStamps[index] = 0;
            _setLocals.resize(height);
        }

        /// Checks that local @p index, of @p type, may be read.
        [[nodiscard]] Check readable(Body const& body, uint32_t index, ValueType const& type) const
        {
            return check(isSet(body, index, type), ValidationErrorCode::UninitializedLocal);
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
                    return _context.valueType(instruction.type).transform([&] {
                        return BlockSignature { .params = {}, .results = std::span { &instruction.type, 1 } };
                    });
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
            return _context.memory(instruction.b).and_then([&](MemoryType const* memory) {
                auto const addressType = memory->limits.addressType;
                return check(instruction.a <= row.naturalAlignment(),
                             ValidationErrorCode::AlignmentMustNotBeLarger)
                    .and_then([&] {
                        return check(addressType == AddressType::I64 || instruction.c <= MaxI32Offset,
                                     ValidationErrorCode::OffsetOutOfRange);
                    })
                    .and_then([&] { return checkLanes(instruction); })
                    .and_then([&]() -> Check {
                        // A store pops the value it writes, and a lane access the vector it changes.
                        if (row.access == MemoryAccessKind::Store || row.shape == VectorShape::Lane)
                            if (auto const popped = pop(value); !popped)
                                return fail(popped.error());
                        if (auto const popped = pop(binary::addressValueType(addressType)); !popped)
                            return fail(popped.error());
                        if (row.access == MemoryAccessKind::Load)
                            push(value);
                        return {};
                    });
            });
        }

        /// Pops the operands of a bulk instruction, @p types from the bottom of the stack up.
        [[nodiscard]] Check popOperands(std::initializer_list<ValueType> types)
        {
            return popAll(std::span { types.begin(), types.size() });
        }

        /// The value type of a copy's count between memories or tables @p destination and @p source.
        template <typename Entity>
        [[nodiscard]] static ValueType copyCountOf(Entity const* destination, Entity const* source) noexcept
        {
            return binary::addressValueType(
                binary::copyCountType(destination->limits.addressType, source->limits.addressType));
        }

        /// The value type of the addresses of @p memory or table.
        template <typename Entity>
        [[nodiscard]] static ValueType addressOf(Entity const* entity) noexcept
        {
            return binary::addressValueType(entity->limits.addressType);
        }

        [[nodiscard]] Check pushAfter(std::expected<Operand, Fault> const& popped, ValueType const& type)
        {
            if (!popped)
                return fail(popped.error());
            push(type);
            return {};
        }

        /// Checks the lane immediates of @p instruction: a lane index below its LaneTable count,
        /// or a shuffle's 16 lane indices below ShuffleLaneCount. Any other instruction has none.
        [[nodiscard]] Check checkLanes(Instruction const& instruction) const
        {
            switch (binary::opcodeInfo(instruction.opcode).immediate)
            {
                case ImmediateKind::LaneIndex:
                case ImmediateKind::MemArgLane: {
                    auto const row = std::ranges::find(LaneTable, instruction.opcode, &LaneRow::opcode);
                    return check(instruction.lane < row->laneCount, ValidationErrorCode::InvalidLaneIndex);
                }
                case ImmediateKind::Shuffle:
                    return check(std::ranges::all_of(binary::v128LiteralOf(_context.module, instruction),
                                                     [](std::byte lane) {
                                                         return std::to_integer<unsigned>(lane)
                                                                < ShuffleLaneCount;
                                                     }),
                                 ValidationErrorCode::InvalidLaneIndex);
                default: return {};
            }
        }

        [[nodiscard]] Check step(Body const& body, Instruction const& instruction)
        {
            auto const& info = binary::opcodeInfo(instruction.opcode);
            if (info.signature != OpcodeSignature::Special)
                return checkLanes(instruction).and_then([&] {
                    return applySignature(binary::signatureInfo(info.signature));
                });
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
                case Opcode::TryTable:
                    // As valid.ml: the block type, then each clause against the labels around the
                    // try_table, whose own label its clauses cannot name.
                    return blockSignature(instruction).and_then([&](BlockSignature const& signature) {
                        return catchClauses(body, instruction)
                            .and_then([&] { return popAll(signature.params); })
                            .transform([&] { pushFrame(Opcode::TryTable, signature); });
                    });
                case Opcode::Throw:
                    return _context.tag(instruction.a).and_then([this](FuncType const* type) {
                        return popRecording(type->params).transform([this] { markUnreachable(); });
                    });
                case Opcode::ThrowRef:
                    return pop(binary::ExnRef).transform([this](Operand const&) { markUnreachable(); });
                case Opcode::Else:
                    return popFrame().transform([this](Frame const& frame) {
                        restoreLocals(frame.setLocalsHeight);
                        pushFrame(Opcode::Else,
                                  BlockSignature { .params = frame.params, .results = frame.results });
                    });
                case Opcode::End:
                    return popFrame().and_then([this](Frame const& frame) -> Check {
                        restoreLocals(frame.setLocalsHeight);
                        // An if without else passes its params through the missing else branch.
                        if (frame.opcode == Opcode::If
                            && !std::ranges::equal(
                                frame.params, frame.results, [this](auto const& param, auto const& result) {
                                    return _context.matches(param, result);
                                }))
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
                case Opcode::BrOnNull:
                    // As valid.ml: the reference peeked, the label looked up, then the operands popped.
                    return peekReference().and_then([&](ValueType const& reference) {
                        return label(instruction.a).and_then([&](Frame const* frame) {
                            return pop()
                                .and_then([&](Operand const&) { return popAll(frame->labelTypes()); })
                                .transform([&] {
                                    pushAll(frame->labelTypes());
                                    push(nonNullOf(reference));
                                });
                        });
                    });
                case Opcode::BrOnNonNull:
                    // As valid.ml: the label first, whose last type must be a reference; the operand
                    // is then popped as that type admitting null, and the branch carries it non-null.
                    return label(instruction.a).and_then([&](Frame const* frame) -> Check {
                        auto const types = frame->labelTypes();
                        if (types.empty() || types.back().kind != ValueKind::Ref)
                            return fail(ValidationErrorCode::TypeMismatch);
                        auto const fallingThrough = types.first(types.size() - 1);
                        auto nullable = types.back();
                        nullable.nullable = binary::Nullable::Yes;
                        return pop(nullable)
                            .and_then([&](Operand const&) { return popAll(fallingThrough); })
                            .transform([&] { pushAll(fallingThrough); });
                    });
                case Opcode::BrTable: return brTable(body, instruction);
                case Opcode::Return: return popAll(body.results).transform([this] { markUnreachable(); });

                case Opcode::Call:
                    return _context.function(instruction.a).and_then([&] {
                        return call(_context.functionType(instruction.a));
                    });
                case Opcode::CallIndirect:
                    return indirectTable(instruction).and_then([&](TableType const* table) {
                        return pop(addressOf(table)).and_then([&](Operand const&) {
                            return call(_context.module.types[instruction.a]);
                        });
                    });

                case Opcode::CallRef:
                    return _context.type(instruction.a)
                        .and_then(
                            [&] { return pop(binary::concreteRef(instruction.a, binary::Nullable::Yes)); })
                        .and_then([&](Operand const&) { return call(_context.module.types[instruction.a]); });

                // The tail forms, as valid.ml: what a call checks of its immediates, then the callee's
                // results against the function's, then the call's operands popped, and the stack
                // polymorphic afterwards.
                case Opcode::ReturnCall:
                    return _context.function(instruction.a).and_then([&] {
                        return tailCall(body, _context.functionType(instruction.a), std::nullopt);
                    });
                case Opcode::ReturnCallIndirect:
                    return indirectTable(instruction).and_then([&](TableType const* table) {
                        return tailCall(body, _context.module.types[instruction.a], addressOf(table));
                    });
                case Opcode::ReturnCallRef:
                    return _context.type(instruction.a).and_then([&] {
                        return tailCall(body,
                                        _context.module.types[instruction.a],
                                        binary::concreteRef(instruction.a, binary::Nullable::Yes));
                    });

                case Opcode::Drop: return pop().transform([](Operand const&) {});
                case Opcode::Select: return select();
                case Opcode::SelectTyped: {
                    auto const types = binary::selectTypesOf(body.sideTables, instruction);
                    if (types.size() != 1)
                        return fail(ValidationErrorCode::InvalidResultArity);
                    auto const& type = types.front();
                    return _context.valueType(type)
                        .and_then([this] { return pop(binary::I32); })
                        .and_then([&](Operand const&) { return pop(type); })
                        .and_then([&](Operand const&) { return pushAfter(pop(type), type); });
                }

                case Opcode::LocalGet:
                    return local(body, instruction.a).and_then([&](ValueType const& type) {
                        return readable(body, instruction.a, type).transform([&] { push(type); });
                    });
                case Opcode::LocalSet:
                    return local(body, instruction.a).and_then([&](ValueType const& type) {
                        return pop(type).transform(
                            [&](Operand const&) { setLocal(body, instruction.a, type); });
                    });
                case Opcode::LocalTee:
                    return local(body, instruction.a).and_then([&](ValueType const& type) {
                        return pushAfter(pop(type), type).transform([&] {
                            setLocal(body, instruction.a, type);
                        });
                    });
                case Opcode::GlobalGet:
                    return global(body, instruction.a).transform([this](GlobalType const* type) {
                        push(type->type);
                    });
                case Opcode::GlobalSet:
                    return global(body, instruction.a).and_then([this](GlobalType const* type) -> Check {
                        if (type->mutability != Mutability::Var)
                            return fail(ValidationErrorCode::ImmutableGlobal);
                        return pop(type->type).transform([](Operand const&) {});
                    });

                case Opcode::TableGet:
                    return _context.table(instruction.a).and_then([this](TableType const* table) {
                        return pushAfter(pop(addressOf(table)), table->element);
                    });
                case Opcode::TableSet:
                    return _context.table(instruction.a).and_then([this](TableType const* table) {
                        return popOperands({ addressOf(table), table->element });
                    });
                case Opcode::TableSize:
                    return _context.table(instruction.a).transform([this](TableType const* table) {
                        push(addressOf(table));
                    });
                case Opcode::TableGrow:
                    return _context.table(instruction.a).and_then([this](TableType const* table) {
                        return popOperands({ table->element, addressOf(table) }).transform([&] {
                            push(addressOf(table));
                        });
                    });
                case Opcode::TableFill:
                    return _context.table(instruction.a).and_then([this](TableType const* table) {
                        return popOperands({ addressOf(table), table->element, addressOf(table) });
                    });
                case Opcode::TableCopy:
                    // a is the destination, b the source; the count is i64 only if both are.
                    return _context.table(instruction.a).and_then([&](TableType const* destination) {
                        return _context.table(instruction.b).and_then([&](TableType const* source) {
                            return check(_context.matches(source->element, destination->element),
                                         ValidationErrorCode::TypeMismatch)
                                .and_then([&] {
                                    return popOperands({ addressOf(destination),
                                                         addressOf(source),
                                                         copyCountOf(destination, source) });
                                });
                        });
                    });
                case Opcode::TableInit:
                    // a is the element segment, b the table; the interpreter looks the table up first.
                    return _context.table(instruction.b).and_then([&](TableType const* table) {
                        return elem(instruction.a).and_then([&] {
                            return check(_context.matches(_context.module.elements[instruction.a].type,
                                                          table->element),
                                         ValidationErrorCode::TypeMismatch)
                                .and_then([&] {
                                    return popOperands({ addressOf(table), binary::I32, binary::I32 });
                                });
                        });
                    });
                case Opcode::ElemDrop: return elem(instruction.a);

                case Opcode::MemorySize:
                    return _context.memory(instruction.a).transform([this](MemoryType const* memory) {
                        push(addressOf(memory));
                    });
                case Opcode::MemoryGrow:
                    return _context.memory(instruction.a).and_then([this](MemoryType const* memory) {
                        return pushAfter(pop(addressOf(memory)), addressOf(memory));
                    });
                case Opcode::MemoryFill:
                    return _context.memory(instruction.a).and_then([this](MemoryType const* memory) {
                        return popOperands({ addressOf(memory), binary::I32, addressOf(memory) });
                    });
                case Opcode::MemoryCopy:
                    // a is the destination, b the source; the count is i64 only if both are.
                    return _context.memory(instruction.a).and_then([&](MemoryType const* destination) {
                        return _context.memory(instruction.b).and_then([&](MemoryType const* source) {
                            return popOperands({ addressOf(destination),
                                                 addressOf(source),
                                                 copyCountOf(destination, source) });
                        });
                    });
                case Opcode::MemoryInit:
                    // a is the data segment, b the memory; the interpreter looks the memory up first.
                    return _context.memory(instruction.b).and_then([&](MemoryType const* memory) {
                        return data(instruction.a).and_then([&] {
                            return popOperands({ addressOf(memory), binary::I32, binary::I32 });
                        });
                    });
                case Opcode::DataDrop: return data(instruction.a);

                case Opcode::RefNull:
                    return _context.valueType(instruction.type).transform([&] { push(instruction.type); });
                case Opcode::RefIsNull:
                    return pop().and_then([this](Operand const& operand) -> Check {
                        if (!isReference(operand))
                            return fail(ValidationErrorCode::TypeMismatch);
                        push(binary::I32);
                        return {};
                    });
                case Opcode::RefAsNonNull:
                    return peekReference().and_then([this](ValueType const& reference) {
                        return pop().transform([&](Operand const&) { push(nonNullOf(reference)); });
                    });
                case Opcode::RefFunc:
                    return _context.function(instruction.a)
                        .and_then([&] {
                            if (!std::ranges::binary_search(_context.declaredFunctions, instruction.a))
                                return Check { failNaming(ValidationErrorCode::UndeclaredFunctionReference,
                                                          instruction.a) };
                            return Check {};
                        })
                        .transform([&] { push(_context.functionReference(instruction.a)); });

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

        /// The table call_indirect and return_call_indirect name: known, of functions, and the type
        /// they name known too.
        [[nodiscard]] std::expected<TableType const*, Fault> indirectTable(
            Instruction const& instruction) const
        {
            return _context.table(instruction.b)
                .and_then([&](TableType const* table) -> std::expected<TableType const*, Fault> {
                    return _context.type(instruction.a)
                        .and_then([this, table] {
                            return check(_context.matches(table->element, binary::FuncRef),
                                         ValidationErrorCode::TypeMismatch);
                        })
                        .transform([table] { return table; });
                });
        }

        /// Types a tail call of @p type from @p body: its results must match the function's, then
        /// @p callee, the operand naming the callee if any, is popped, and the parameters beneath it.
        [[nodiscard]] Check tailCall(Body const& body, FuncType const& type, std::optional<ValueType> callee)
        {
            auto const returnable = std::ranges::equal(
                type.results, body.results, [this](auto const& result, auto const& expected) {
                    return _context.matches(result, expected);
                });
            return check(returnable, ValidationErrorCode::TypeMismatch)
                .and_then([&]() -> Check {
                    if (!callee)
                        return {};
                    return pop(*callee).transform([](Operand const&) {});
                })
                .and_then([&] { return popAll(type.params); })
                .transform([this] { markUnreachable(); });
        }

        /// Checks the catch clauses of try_table @p instruction: each one's tag is known, and what it
        /// hands its label matches the label's types: the tag's params for catch, then the exnref,
        /// (ref exn), for catch_ref; nothing for catch_all, and the exnref for catch_all_ref.
        [[nodiscard]] Check catchClauses(Body const& body, Instruction const& instruction)
        {
            constexpr auto CaughtReference = binary::ValueType {
                .kind = ValueKind::Ref,
                .nullable = binary::Nullable::No,
                .heap = { .kind = binary::AbstractHeapType::Exn },
            };
            for (auto const& clause: binary::catchClausesOf(body.sideTables, instruction))
            {
                _handed.clear();
                if (binary::catchesByTag(clause.kind))
                {
                    auto const type = _context.tag(clause.tag);
                    if (!type)
                        return fail(type.error());
                    _handed.assign((*type)->params.begin(), (*type)->params.end());
                }
                if (binary::handsReference(clause.kind))
                    _handed.push_back(CaughtReference);
                auto const target = label(clause.label);
                if (!target)
                    return fail(target.error());
                auto const types = (*target)->labelTypes();
                if (!std::ranges::equal(_handed, types, [this](auto const& handed, auto const& required) {
                        return _context.matches(handed, required);
                    }))
                    return fail(ValidationErrorCode::TypeMismatch);
            }
            return {};
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
        std::vector<ValueType> _handed;      ///< Scratch for catchClauses: what a clause hands its label.
        std::vector<uint32_t> _setLocals;    ///< The non-defaultable locals set in the open blocks.
        std::vector<uint64_t> _setStamps;    ///< Per local index, _generation while it is set.
        uint64_t _generation = 0;            ///< The stamp of the body being validated; never 0.
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
                       .tagTypes = {},
                       .importedGlobalCount = 0,
                       .declaredFunctions = declaredFunctionsOf(module),
                       .canonical = {} },
            _bodies { _context }
        {
        }

        [[nodiscard]] std::expected<void, ValidationError> validate()
        {
            return checkTypeSection()
                .and_then([this] { return checkImports(); })
                .and_then([this] { return checkTags(); })
                .and_then([this] { return checkFunctionTypes(); })
                .and_then([this] { return checkTypes(); })
                .and_then([this] { return checkTablesAndMemories(); })
                .and_then([this] { return checkGlobals(); })
                .and_then([this] { return checkElements(); })
                .and_then([this] { return checkData(); })
                .and_then([this] { return checkBodies(); })
                .and_then([this] { return checkStart(); })
                .and_then([this] { return checkExports(); })
                .and_then([this] { return checkCounts(); });
        }

      private:
        [[nodiscard]] Module const& module() const noexcept { return _context.module; }

        /// Checks that each type names only the types before it, as a type section without rec
        /// groups must, then finds every type's canonical id.
        [[nodiscard]] std::expected<void, ValidationError> checkTypeSection()
        {
            for (auto const [index, type]: core::views::enumerate(module().types))
            {
                auto const before = [index](std::span<ValueType const> types) -> Check {
                    for (auto const& valueType: types)
                        if (auto const result = checkTypeIndex(valueType, index); !result)
                            return result;
                    return {};
                };
                if (auto const result = before(type.params).and_then([&] { return before(type.results); });
                    !result)
                    return moduleError(result.error());
            }
            _context.canonical = binary::canonicalTypeIds(module().types);
            return {};
        }

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
                        result = _context.valueType(import.table.element).and_then([&] {
                            return checkTable(import.table);
                        });
                        _context.tables.push_back(import.table);
                        break;
                    case ExternKind::Memory:
                        result = checkMemory(import.memory);
                        _context.memories.push_back(import.memory);
                        break;
                    case ExternKind::Global:
                        result = _context.valueType(import.global.type);
                        _context.globals.push_back(import.global);
                        ++_context.importedGlobalCount;
                        break;
                    case ExternKind::Tag:
                        result = _context.tagType(import.typeIndex);
                        _context.tagTypes.push_back(import.typeIndex);
                        break;
                }
                if (!result)
                    return moduleError(result.error());
            }
            return {};
        }

        /// Checks the defined tags, after the imported ones as valid.ml does, before the functions.
        [[nodiscard]] std::expected<void, ValidationError> checkTags()
        {
            for (auto const& tag: module().tags)
            {
                if (auto const result = _context.tagType(tag.typeIndex); !result)
                    return moduleError(result.error());
                _context.tagTypes.push_back(tag.typeIndex);
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
            for (auto const& table: module().tables)
                _context.tables.push_back(table.type);
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
                if (auto const result = _context.valueType(global.type.type).and_then([&] {
                        return checkConstant(global.init, global.type.type);
                    });
                    !result)
                    return moduleError(result.error());
            return {};
        }

        /// Checks a defined table: its type, and that its slots start with a value of its element
        /// type: its initializer's, or the null, which a non-null element type has none of.
        [[nodiscard]] Check checkDefinedTable(binary::Table const& table)
        {
            auto const& element = table.type.element;
            return _context.valueType(element).and_then([&] { return checkTable(table.type); }).and_then([&] {
                if (table.init.empty())
                    return check(binary::isDefaultable(element), ValidationErrorCode::TypeMismatch);
                return checkConstant(table.init, element);
            });
        }

        [[nodiscard]] std::expected<void, ValidationError> checkTablesAndMemories()
        {
            for (auto const& table: module().tables)
                if (auto const result = checkDefinedTable(table); !result)
                    return moduleError(result.error());
            for (auto const& memory: module().memories)
                if (auto const result = checkMemory(memory); !result)
                    return moduleError(result.error());
            return {};
        }

        [[nodiscard]] Check checkElement(binary::ElementSegment const& segment)
        {
            if (auto const result = _context.valueType(segment.type); !result)
                return result;
            if (auto const* indices = std::get_if<std::vector<uint32_t>>(&segment.items))
            {
                // Each index stands for (ref.func index): a reference to the function's type.
                for (auto const index: *indices)
                    if (auto const result = _context.function(index).and_then([&] {
                            return check(_context.matches(_context.functionReference(index), segment.type),
                                         ValidationErrorCode::TypeMismatch);
                        });
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
                return checkConstant(segment.offset, binary::addressValueType(table->limits.addressType))
                    .and_then([&] {
                        return check(_context.matches(segment.type, table->element),
                                     ValidationErrorCode::TypeMismatch);
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
                auto const result = _context.memory(segment.memory).and_then([&](MemoryType const* memory) {
                    return checkConstant(segment.offset,
                                         binary::addressValueType(memory->limits.addressType));
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
                for (auto const& run: function.locals)
                    if (auto const result = _context.valueType(run.type); !result)
                        return std::unexpected { errorAt(result.error(), functionIndex, NoInstruction) };
                auto const body = Body { .instructions = function.body,
                                         .sideTables = function,
                                         .params = type.params,
                                         .localRuns = function.locals,
                                         .results = type.results,
                                         .globals = _context.globals };
                if (auto const result = _bodies.validate(body); !result)
                    return std::unexpected { errorAt(
                        result.error().fault, functionIndex, result.error().instruction) };
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
                case ExternKind::Memory:
                    return _context.memory(entry.index).transform([](MemoryType const*) {});
                case ExternKind::Global:
                    return checkIndex(
                        entry.index, _context.globals.size(), ValidationErrorCode::UnknownGlobal);
                case ExternKind::Tag: break; // checked below the switch
            }
            return checkIndex(entry.index, _context.tagTypes.size(), ValidationErrorCode::UnknownTag);
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
    // A set decodeModule accepted is closed; this keeps a validator called with another set honest.
    Require(features.checkPrerequisites().has_value());
    return ModuleValidator { module, features }.validate();
}

} // namespace isoline::validate
