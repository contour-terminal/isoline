// SPDX-License-Identifier: Apache-2.0
#pragma once

// The vocabulary of the compiler's SSA intermediate representation, as data: its value types, the
// effects an operation may have, and the rows of Operations.def with the operand, result and immediate
// tables they name. The verifier checks instructions against these rows and the Printer renders them
// from them, so adding an operation is adding a row to Operations.def.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Trap.hpp>

namespace isoline::compiler::intermediate
{

/// The type of an SSA value.
enum class Type : uint8_t
{
    I32,     ///< A 32-bit integer.
    I64,     ///< A 64-bit integer.
    F32,     ///< A 32-bit float.
    F64,     ///< A 64-bit float.
    Ref,     ///< A reference, as one 64-bit word: kind | heap.kind << 8 | address << 32.
    Pointer, ///< A host address: a guarded effective address, a memory's base, a table element's place.
};

/// The last Type enumerator, for checking TypeTable's size.
inline constexpr auto LastType = Type::Pointer;

/// One value type: its name in the Printer's text, and its width.
struct TypeRow
{
    Type type;             ///< The type; equal to the row's index.
    std::string_view name; ///< How the Printer spells it.
    uint8_t bytes;         ///< Its width.
};

/// Every value type, in Type order.
inline constexpr auto TypeTable = std::to_array<TypeRow>({
    { .type = Type::I32, .name = "i32", .bytes = 4 },
    { .type = Type::I64, .name = "i64", .bytes = 8 },
    { .type = Type::F32, .name = "f32", .bytes = 4 },
    { .type = Type::F64, .name = "f64", .bytes = 8 },
    { .type = Type::Ref, .name = "ref", .bytes = 8 },
    { .type = Type::Pointer, .name = "ptr", .bytes = 8 },
});

/// A set of Types: bit t is set when Type t is in it.
using TypeMask = uint8_t;

/// The set holding @p type alone.
/// @param type A Type enumerator.
/// @return Its bit.
[[nodiscard]] constexpr TypeMask maskOf(Type type) noexcept
{
    return static_cast<TypeMask>(1U << std::to_underlying(type));
}

/// Every Type.
inline constexpr auto AnyType = static_cast<TypeMask>((1U << TypeTable.size()) - 1);
/// The types of an index into a 32- or 64-bit memory or table: I32 and I64.
inline constexpr auto IndexTypes = static_cast<TypeMask>(maskOf(Type::I32) | maskOf(Type::I64));
/// The numeric types, which guest memory holds: I32, I64, F32 and F64.
inline constexpr auto NumberTypes = static_cast<TypeMask>(IndexTypes | maskOf(Type::F32) | maskOf(Type::F64));
/// The types a WebAssembly value takes once compiled: the numeric types and Ref.
inline constexpr auto WasmTypes = static_cast<TypeMask>(NumberTypes | maskOf(Type::Ref));

/// Whether @p mask holds @p type.
[[nodiscard]] constexpr bool holds(TypeMask mask, Type type) noexcept
{
    return (mask & maskOf(type)) != 0;
}

/// Whether @p type is a float type.
[[nodiscard]] constexpr bool isFloat(Type type) noexcept
{
    return type == Type::F32 || type == Type::F64;
}

/// A value of the execution or instance context that generated code keeps as an SSA value: what the
/// `field` immediate of loadContext and storeContext names. The rows of ContextValueTable.
enum class ContextValue : uint8_t
{
    StepCountdown, ///< The steps left of the lent budget (ExecutionState::stepsLeft).
    Memory0Base,   ///< Memory 0's base address.
    Memory0Bound,  ///< Memory 0's size in bytes.
};

/// The last ContextValue enumerator, for checking ContextValueTable's size.
inline constexpr auto LastContextValue = ContextValue::Memory0Bound;

/// Whether generated code may write a context value back.
enum class ContextAccess : uint8_t
{
    ReadOnly,  ///< Loaded only: the runtime owns it.
    ReadWrite, ///< Loaded and stored back (storeContext).
};

/// One context value: its name in the Printer's text, its type and whether it is stored back.
struct ContextValueRow
{
    ContextValue value;    ///< The value; equal to the row's index.
    std::string_view name; ///< How the Printer names it.
    Type type;             ///< Its type, which loadContext's `type` and storeContext's operand must be.
    ContextAccess access;  ///< Whether storeContext may write it.
};

/// Every context value, in ContextValue order.
inline constexpr auto ContextValueTable = std::to_array<ContextValueRow>({
    { .value = ContextValue::StepCountdown,
      .name = "step_countdown",
      .type = Type::I64,
      .access = ContextAccess::ReadWrite },
    { .value = ContextValue::Memory0Base,
      .name = "memory0_base",
      .type = Type::Pointer,
      .access = ContextAccess::ReadOnly },
    { .value = ContextValue::Memory0Bound,
      .name = "memory0_bound",
      .type = Type::I64,
      .access = ContextAccess::ReadOnly },
});

/// What an operation does besides computing its results: the column the passes and the selector read
/// to decide what may be removed, reordered or merged.
enum class Effect : uint8_t
{
    Pure,         ///< Nothing: removable when unused, freely movable.
    MayTrap,      ///< May trap: never removed, never moved across another effect but Pure.
    ReadsMemory,  ///< Reads guest memory, a table or a global.
    WritesMemory, ///< Writes guest memory, a table or a global.
    ReadsContext, ///< Reads a value of the execution or instance context, invalidated only by a Call.
    Call,         ///< Calls a function or a stub: reads and writes everything, may trap.
    /// Touches the execution budget or the frame accounting (poll, enterFrame, leaveFrame,
    /// storeContext): never removed, reordered across another, or merged.
    Poll,
    Control, ///< Ends its block: a terminator.
};

/// The type an operand must have.
enum class OperandType : uint8_t
{
    I32,            ///< Type::I32.
    I64,            ///< Type::I64.
    F32,            ///< Type::F32.
    F64,            ///< Type::F64.
    Ref,            ///< Type::Ref.
    Pointer,        ///< Type::Pointer.
    Integer,        ///< I32 or I64: an index into a 32- or 64-bit memory or table.
    Float,          ///< F32 or F64.
    Any,            ///< Any type.
    Wasm,           ///< A type a WebAssembly value takes: a numeric type or Ref (WasmTypes).
    SameAsOperand0, ///< The type of the instruction's first operand.
};

/// Whether an operand list ends after its fixed operands.
enum class OperandTail : uint8_t
{
    Fixed,    ///< Exactly the fixed operands.
    Variadic, ///< The fixed operands, then any number of further values of any type.
};

/// The operand lists of Operations.def's Operands column: the rows of OperandSignatureTable, in order.
enum class OperandSignature : uint8_t
{
    None,              ///< No operands.
    I32,               ///< One I32.
    I64,               ///< One I64.
    F32,               ///< One F32.
    F64,               ///< One F64.
    Ref,               ///< One Ref.
    Pointer,           ///< One Pointer.
    Integer,           ///< One I32 or I64.
    Float,             ///< One F32 or F64.
    Any,               ///< One value of any type.
    Wasm,              ///< One WebAssembly value: a numeric type or Ref.
    I32I32,            ///< Two I32.
    I64I64,            ///< Two I64.
    F32F32,            ///< Two F32.
    F64F64,            ///< Two F64.
    Select,            ///< Two values of one type, then an I32 condition.
    Store,             ///< A Pointer, then the value stored.
    StoreReference,    ///< A Pointer, then the Ref stored.
    CheckMemory,       ///< The index (I32 or I64), the memory's base (Pointer) and bound (I64).
    Base,              ///< A memory's base (Pointer): an address no guard returned, which no access takes.
    ConditionIndex,    ///< An I32 condition, then an index (I32 or I64): a trap's subject.
    RefIndex,          ///< A Ref, then an index (I32 or I64): a trap's subject.
    Variadic,          ///< Any number of values.
    PointerVariadic,   ///< A Pointer, then any number of values.
    ConditionVariadic, ///< An I32 condition or index, then any number of values.
};

/// The last OperandSignature enumerator, for checking OperandSignatureTable's size.
inline constexpr auto LastOperandSignature = OperandSignature::ConditionVariadic;

/// One operand list: its fixed operands' types, and whether further values follow.
///
/// Fixed-size storage with a count keeps the table a constant; fixed() views the used part.
struct OperandSignatureRow
{
    OperandSignature signature;            ///< The list; equal to the row's index.
    std::array<OperandType, 3> types {};   ///< The fixed operands' types, first operand first.
    uint8_t count = 0;                     ///< How many of types are used.
    OperandTail tail = OperandTail::Fixed; ///< Whether further values follow them.

    /// The fixed operands' types.
    /// @return The first count entries of types.
    [[nodiscard]] constexpr std::span<OperandType const> fixed() const noexcept
    {
        return std::span { types }.first(count);
    }
};

/// Every operand list, in OperandSignature order.
inline constexpr auto OperandSignatureTable = std::to_array<OperandSignatureRow>({
    { .signature = OperandSignature::None },
    { .signature = OperandSignature::I32, .types = { OperandType::I32 }, .count = 1 },
    { .signature = OperandSignature::I64, .types = { OperandType::I64 }, .count = 1 },
    { .signature = OperandSignature::F32, .types = { OperandType::F32 }, .count = 1 },
    { .signature = OperandSignature::F64, .types = { OperandType::F64 }, .count = 1 },
    { .signature = OperandSignature::Ref, .types = { OperandType::Ref }, .count = 1 },
    { .signature = OperandSignature::Pointer, .types = { OperandType::Pointer }, .count = 1 },
    { .signature = OperandSignature::Integer, .types = { OperandType::Integer }, .count = 1 },
    { .signature = OperandSignature::Float, .types = { OperandType::Float }, .count = 1 },
    { .signature = OperandSignature::Any, .types = { OperandType::Any }, .count = 1 },
    { .signature = OperandSignature::Wasm, .types = { OperandType::Wasm }, .count = 1 },
    { .signature = OperandSignature::I32I32, .types = { OperandType::I32, OperandType::I32 }, .count = 2 },
    { .signature = OperandSignature::I64I64, .types = { OperandType::I64, OperandType::I64 }, .count = 2 },
    { .signature = OperandSignature::F32F32, .types = { OperandType::F32, OperandType::F32 }, .count = 2 },
    { .signature = OperandSignature::F64F64, .types = { OperandType::F64, OperandType::F64 }, .count = 2 },
    { .signature = OperandSignature::Select,
      .types = { OperandType::Any, OperandType::SameAsOperand0, OperandType::I32 },
      .count = 3 },
    { .signature = OperandSignature::Store, .types = { OperandType::Pointer, OperandType::Any }, .count = 2 },
    { .signature = OperandSignature::StoreReference,
      .types = { OperandType::Pointer, OperandType::Ref },
      .count = 2 },
    { .signature = OperandSignature::CheckMemory,
      .types = { OperandType::Integer, OperandType::Pointer, OperandType::I64 },
      .count = 3 },
    { .signature = OperandSignature::Base, .types = { OperandType::Pointer }, .count = 1 },
    { .signature = OperandSignature::ConditionIndex,
      .types = { OperandType::I32, OperandType::Integer },
      .count = 2 },
    { .signature = OperandSignature::RefIndex,
      .types = { OperandType::Ref, OperandType::Integer },
      .count = 2 },
    { .signature = OperandSignature::Variadic, .tail = OperandTail::Variadic },
    { .signature = OperandSignature::PointerVariadic,
      .types = { OperandType::Pointer },
      .count = 1,
      .tail = OperandTail::Variadic },
    { .signature = OperandSignature::ConditionVariadic,
      .types = { OperandType::I32 },
      .count = 1,
      .tail = OperandTail::Variadic },
});

/// What results an operation has.
enum class ResultRule : uint8_t
{
    None,           ///< None.
    SameAsOperand0, ///< One, of its first operand's type.
    I32,            ///< One I32.
    I64,            ///< One I64.
    F32,            ///< One F32.
    F64,            ///< One F64.
    Ref,            ///< One Ref.
    Pointer,        ///< One Pointer.
    FromImmediate,  ///< One, of the Type its `type` immediate names.
    Multiple,       ///< Any number, of any types: a call's, which its callee's type gives.
};

/// How a load widens a narrower access to its result.
enum class Extension : uint8_t
{
    Zero, ///< Zero-extends; also every full-width access, which has nothing to extend.
    Sign, ///< Sign-extends.
};

/// The last Extension enumerator, for the range of an Extension immediate.
inline constexpr auto LastExtension = Extension::Sign;

/// What values an immediate field may hold.
enum class ImmediateValues : uint8_t
{
    UpTo,       ///< 0 to the row's maximum.
    PowerOfTwo, ///< A power of two up to the row's maximum: an access size.
};

/// One field of an instruction's immediates.
enum class ImmediateField : uint8_t
{
    Value,               ///< A constant's bits, zero-extended.
    Heap,                ///< A binary::AbstractHeapType: ref.null's heap.
    Function,            ///< A function index of the module.
    Memory,              ///< A memory index.
    Table,               ///< A table index.
    Global,              ///< A global index.
    TypeIndex,           ///< A type index of the module: call_indirect's expected type.
    Offset,              ///< A byte offset added to an address.
    Size,                ///< An access's width in bytes: 1, 2, 4 or 8.
    Extension,           ///< An Extension.
    Type,                ///< A Type: a FromImmediate result's.
    Trap,                ///< A runtime::TrapCode.
    Site,                ///< A runtime::PollSite.
    FrameCost,           ///< A frame's stack values (validate::ValidatedModule::frameCost).
    Stub,                ///< A runtime stub (the execution layer's RuntimeStubTable row).
    Field,               ///< A ContextValue: what loadContext and storeContext name.
    KindOffset,          ///< The byte offset of a table element's kind (runtime::RefLayout::kindOffset).
    HeapKindOffset,      ///< The byte offset of its heap's kind (runtime::RefLayout::heapKindOffset).
    HeapTypeIndexOffset, ///< The byte offset of its heap's type index
                         ///< (runtime::RefLayout::heapTypeIndexOffset).
    AddressOffset,       ///< The byte offset of its address (runtime::RefLayout::addressOffset).
    Target,              ///< A branch target's BlockId; an ArgumentCount always follows it.
    ArgumentCount,       ///< How many arguments the target before it takes.
};

/// The last ImmediateField enumerator, for checking ImmediateFieldTable's size.
inline constexpr auto LastImmediateField = ImmediateField::ArgumentCount;

/// One immediate field: its name in the Printer's text, and the values it may hold.
struct ImmediateFieldRow
{
    ImmediateField field;   ///< The field; equal to the row's index.
    std::string_view name;  ///< How the Printer names it.
    ImmediateValues values; ///< Which values up to maximum are allowed.
    uint64_t maximum;       ///< The largest value allowed.
};

namespace detail
{
    /// The largest value of a 32-bit index.
    inline constexpr auto IndexMaximum = uint64_t { std::numeric_limits<uint32_t>::max() };
} // namespace detail

/// Every immediate field, in ImmediateField order.
inline constexpr auto ImmediateFieldTable = std::to_array<ImmediateFieldRow>({
    { .field = ImmediateField::Value,
      .name = "value",
      .values = ImmediateValues::UpTo,
      .maximum = std::numeric_limits<uint64_t>::max() },
    { .field = ImmediateField::Heap,
      .name = "heap",
      .values = ImmediateValues::UpTo,
      .maximum = std::to_underlying(binary::AbstractHeapType::NoExn) },
    { .field = ImmediateField::Function,
      .name = "function",
      .values = ImmediateValues::UpTo,
      .maximum = detail::IndexMaximum },
    { .field = ImmediateField::Memory,
      .name = "memory",
      .values = ImmediateValues::UpTo,
      .maximum = detail::IndexMaximum },
    { .field = ImmediateField::Table,
      .name = "table",
      .values = ImmediateValues::UpTo,
      .maximum = detail::IndexMaximum },
    { .field = ImmediateField::Global,
      .name = "global",
      .values = ImmediateValues::UpTo,
      .maximum = detail::IndexMaximum },
    { .field = ImmediateField::TypeIndex,
      .name = "type_index",
      .values = ImmediateValues::UpTo,
      .maximum = detail::IndexMaximum },
    { .field = ImmediateField::Offset,
      .name = "offset",
      .values = ImmediateValues::UpTo,
      .maximum = std::numeric_limits<uint64_t>::max() },
    { .field = ImmediateField::Size, .name = "size", .values = ImmediateValues::PowerOfTwo, .maximum = 8 },
    { .field = ImmediateField::Extension,
      .name = "extension",
      .values = ImmediateValues::UpTo,
      .maximum = std::to_underlying(LastExtension) },
    { .field = ImmediateField::Type,
      .name = "type",
      .values = ImmediateValues::UpTo,
      .maximum = std::to_underlying(LastType) },
    { .field = ImmediateField::Trap,
      .name = "trap",
      .values = ImmediateValues::UpTo,
      .maximum = std::to_underlying(runtime::LastTrapCode) },
    { .field = ImmediateField::Site,
      .name = "site",
      .values = ImmediateValues::UpTo,
      .maximum = std::to_underlying(runtime::LastPollSite) },
    { .field = ImmediateField::FrameCost,
      .name = "frame_cost",
      .values = ImmediateValues::UpTo,
      .maximum = detail::IndexMaximum },
    { .field = ImmediateField::Stub,
      .name = "stub",
      .values = ImmediateValues::UpTo,
      .maximum = detail::IndexMaximum },
    { .field = ImmediateField::Field,
      .name = "field",
      .values = ImmediateValues::UpTo,
      .maximum = std::to_underlying(LastContextValue) },
    { .field = ImmediateField::KindOffset,
      .name = "kind_offset",
      .values = ImmediateValues::UpTo,
      .maximum = detail::IndexMaximum },
    { .field = ImmediateField::HeapKindOffset,
      .name = "heap_kind_offset",
      .values = ImmediateValues::UpTo,
      .maximum = detail::IndexMaximum },
    { .field = ImmediateField::HeapTypeIndexOffset,
      .name = "heap_type_index_offset",
      .values = ImmediateValues::UpTo,
      .maximum = detail::IndexMaximum },
    { .field = ImmediateField::AddressOffset,
      .name = "address_offset",
      .values = ImmediateValues::UpTo,
      .maximum = detail::IndexMaximum },
    { .field = ImmediateField::Target,
      .name = "target",
      .values = ImmediateValues::UpTo,
      .maximum = detail::IndexMaximum },
    { .field = ImmediateField::ArgumentCount,
      .name = "arguments",
      .values = ImmediateValues::UpTo,
      .maximum = detail::IndexMaximum },
});

/// The immediate layouts of Operations.def's ImmediateSchema column: the rows of ImmediateSchemaTable.
enum class ImmediateSchema : uint8_t
{
    None,         ///< No immediates.
    Value,        ///< {value}: a constant.
    Heap,         ///< {heap}: ref.null.
    Function,     ///< {function}: ref.func, call, call_import.
    Memory,       ///< {memory}: memoryBase, memoryBound.
    MemoryAccess, ///< {memory, offset, size}: checkMemory, inBoundsAddress.
    Load,         ///< {memory, size, extension, type}: load, whose result is a numeric type.
    StoreAccess,  ///< {memory, size}: store, of its value's width or narrower.
    Offset,       ///< {offset}: guardAddress.
    TableCheck,   ///< {table, trap}: checkTable.
    Table,        ///< {table}: tableSize.
    /// {table, kind_offset, heap_kind_offset, heap_type_index_offset, address_offset}: loadTableElement and
    /// storeTableElement, which read and write a table element's fields one by one at these offsets.
    TableElement,
    TypeIndex,    ///< {type_index}: typeId.
    GlobalType,   ///< {global, type}: loadGlobal, whose result is a numeric type or Ref.
    Global,       ///< {global}: storeGlobal.
    ContextType,  ///< {field, type}: loadContext, whose result is its ContextValue's type.
    Context,      ///< {field}: storeContext, of a ContextValue generated code may write.
    PollSite,     ///< {site}: poll.
    FrameCost,    ///< {frame_cost}: enterFrame, leaveFrame.
    CallIndirect, ///< {type_index, table}: callIndirect.
    Stub,         ///< {stub}: callStub.
    Trap,         ///< {trap}: trapIf, trap.
    Jump,         ///< One target: jump.
    Branch,       ///< Two targets, taken when the condition is non-zero and when it is zero: branch.
    Switch,       ///< The fallback target, then any number of targets: switch.
};

/// The last ImmediateSchema enumerator, for checking ImmediateSchemaTable's size.
inline constexpr auto LastImmediateSchema = ImmediateSchema::Switch;

/// Which type a memory access's `size` immediate is checked against: an access is at most as wide as its
/// type, a float access is as wide as its type, and only a narrower load extends with a sign.
enum class AccessWidth : uint8_t
{
    None,    ///< The layout has no access width to check.
    OfType,  ///< The type its `type` immediate names: a load's result.
    OfValue, ///< Its second operand's type: a store's value.
};

/// One immediate layout: its fixed fields, then a group of fields repeated any number of times, and
/// what its fields must agree on with the instruction's types.
struct ImmediateSchemaRow
{
    ImmediateSchema schema;                   ///< The layout; equal to the row's index.
    std::span<ImmediateField const> fixed;    ///< The fields every instruction of it has, in order.
    std::span<ImmediateField const> repeated; ///< The group that follows them zero or more times.
    /// The types its `type` immediate may name; every type when it has none. A `field` immediate narrows
    /// it further, to its ContextValue's type.
    TypeMask types = AnyType;
    AccessWidth width = AccessWidth::None; ///< Which type its `size` immediate must fit.
};

namespace detail
{
    using enum ImmediateField;
    inline constexpr auto NoFields = std::array<ImmediateField, 0> {};
    inline constexpr auto ValueFields = std::to_array({ Value });
    inline constexpr auto HeapFields = std::to_array({ Heap });
    inline constexpr auto FunctionFields = std::to_array({ Function });
    inline constexpr auto MemoryFields = std::to_array({ Memory });
    inline constexpr auto MemoryAccessFields = std::to_array({ Memory, Offset, Size });
    inline constexpr auto LoadFields = std::to_array({ Memory, Size, Extension, Type });
    inline constexpr auto StoreFields = std::to_array({ Memory, Size });
    inline constexpr auto OffsetFields = std::to_array({ Offset });
    inline constexpr auto TableCheckFields = std::to_array({ Table, Trap });
    inline constexpr auto TableFields = std::to_array({ Table });
    inline constexpr auto TableElementFields =
        std::to_array({ Table, KindOffset, HeapKindOffset, HeapTypeIndexOffset, AddressOffset });
    inline constexpr auto TypeIndexFields = std::to_array({ TypeIndex });
    inline constexpr auto GlobalTypeFields = std::to_array({ Global, Type });
    inline constexpr auto GlobalFields = std::to_array({ Global });
    inline constexpr auto ContextTypeFields = std::to_array({ Field, Type });
    inline constexpr auto ContextFields = std::to_array({ Field });
    inline constexpr auto PollSiteFields = std::to_array({ Site });
    inline constexpr auto FrameCostFields = std::to_array({ FrameCost });
    inline constexpr auto CallIndirectFields = std::to_array({ TypeIndex, Table });
    inline constexpr auto StubFields = std::to_array({ Stub });
    inline constexpr auto TrapFields = std::to_array({ Trap });
    inline constexpr auto TargetFields = std::to_array({ Target, ArgumentCount });
    inline constexpr auto TwoTargetFields = std::to_array({ Target, ArgumentCount, Target, ArgumentCount });
} // namespace detail

/// Every immediate layout, in ImmediateSchema order.
inline constexpr auto ImmediateSchemaTable = std::to_array<ImmediateSchemaRow>({
    { .schema = ImmediateSchema::None, .fixed = detail::NoFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::Value, .fixed = detail::ValueFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::Heap, .fixed = detail::HeapFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::Function, .fixed = detail::FunctionFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::Memory, .fixed = detail::MemoryFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::MemoryAccess,
      .fixed = detail::MemoryAccessFields,
      .repeated = detail::NoFields },
    { .schema = ImmediateSchema::Load,
      .fixed = detail::LoadFields,
      .repeated = detail::NoFields,
      .types = NumberTypes,
      .width = AccessWidth::OfType },
    { .schema = ImmediateSchema::StoreAccess,
      .fixed = detail::StoreFields,
      .repeated = detail::NoFields,
      .width = AccessWidth::OfValue },
    { .schema = ImmediateSchema::Offset, .fixed = detail::OffsetFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::TableCheck,
      .fixed = detail::TableCheckFields,
      .repeated = detail::NoFields },
    { .schema = ImmediateSchema::Table, .fixed = detail::TableFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::TableElement,
      .fixed = detail::TableElementFields,
      .repeated = detail::NoFields },
    { .schema = ImmediateSchema::TypeIndex, .fixed = detail::TypeIndexFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::GlobalType,
      .fixed = detail::GlobalTypeFields,
      .repeated = detail::NoFields,
      .types = WasmTypes },
    { .schema = ImmediateSchema::Global, .fixed = detail::GlobalFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::ContextType,
      .fixed = detail::ContextTypeFields,
      .repeated = detail::NoFields },
    { .schema = ImmediateSchema::Context, .fixed = detail::ContextFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::PollSite, .fixed = detail::PollSiteFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::FrameCost, .fixed = detail::FrameCostFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::CallIndirect,
      .fixed = detail::CallIndirectFields,
      .repeated = detail::NoFields },
    { .schema = ImmediateSchema::Stub, .fixed = detail::StubFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::Trap, .fixed = detail::TrapFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::Jump, .fixed = detail::TargetFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::Branch, .fixed = detail::TwoTargetFields, .repeated = detail::NoFields },
    { .schema = ImmediateSchema::Switch, .fixed = detail::TargetFields, .repeated = detail::TargetFields },
});

/// Every operation of the intermediate representation, generated from Operations.def. One byte holds
/// today's rows. A row past the 256th does not wrap: its enumerator lies outside uint8_t, which fails to
/// compile, and that is when P5b's SIMD rows widen it to uint16_t.
enum class Operation : uint8_t
{
#define ISOLINE_OPERATION(name, text, operands, result, effect, immediates) name,
#include <isoline/compiler/intermediate/Operations.def>
#undef ISOLINE_OPERATION
};

/// One operation: its text, its operands, results, effect and immediates.
struct OperationRow
{
    Operation operation;        ///< The operation; equal to the row's index.
    std::string_view text;      ///< Its name in the Printer's text.
    OperandSignature operands;  ///< Its operands.
    ResultRule result;          ///< Its results.
    Effect effect;              ///< What it does besides computing them.
    ImmediateSchema immediates; ///< Its immediates.
};

/// Every operation, in Operation order, generated from Operations.def.
inline constexpr auto OperationTable = std::to_array<OperationRow>({
#define ISOLINE_OPERATION(name, text, operands, result, effect, immediates) \
    { Operation::name,    text,           OperandSignature::operands,       \
      ResultRule::result, Effect::effect, ImmediateSchema::immediates },
#include <isoline/compiler/intermediate/Operations.def>
#undef ISOLINE_OPERATION
});

/// How many operations there are.
inline constexpr auto OperationCount = OperationTable.size();

namespace detail
{
    /// Whether every row of @p table names, through @p key, the enumerator of its own index.
    template <typename Table, typename Key>
    [[nodiscard]] constexpr bool isInEnumeratorOrder(Table const& table, Key key) noexcept
    {
        auto index = std::size_t { 0 };
        for (auto const& row: table)
            if (static_cast<std::size_t>(key(row)) != index++)
                return false;
        return true;
    }
} // namespace detail

static_assert(TypeTable.size() == std::to_underlying(LastType) + 1
                  && detail::isInEnumeratorOrder(TypeTable, [](TypeRow const& row) { return row.type; }),
              "TypeTable must hold one row per Type, at the index of its enumerator");
static_assert(ContextValueTable.size() == std::to_underlying(LastContextValue) + 1
                  && detail::isInEnumeratorOrder(ContextValueTable,
                                                 [](ContextValueRow const& row) { return row.value; }),
              "ContextValueTable must hold one row per ContextValue, at the index of its enumerator");
static_assert(OperandSignatureTable.size() == std::to_underlying(LastOperandSignature) + 1
                  && detail::isInEnumeratorOrder(OperandSignatureTable,
                                                 [](OperandSignatureRow const& row) {
                                                     return row.signature;
                                                 }),
              "OperandSignatureTable must hold one row per OperandSignature, at the index of its enumerator");
static_assert(ImmediateFieldTable.size() == std::to_underlying(LastImmediateField) + 1
                  && detail::isInEnumeratorOrder(ImmediateFieldTable,
                                                 [](ImmediateFieldRow const& row) { return row.field; }),
              "ImmediateFieldTable must hold one row per ImmediateField, at the index of its enumerator");
static_assert(ImmediateSchemaTable.size() == std::to_underlying(LastImmediateSchema) + 1
                  && detail::isInEnumeratorOrder(ImmediateSchemaTable,
                                                 [](ImmediateSchemaRow const& row) { return row.schema; }),
              "ImmediateSchemaTable must hold one row per ImmediateSchema, at the index of its enumerator");
static_assert(detail::isInEnumeratorOrder(OperationTable,
                                          [](OperationRow const& row) { return row.operation; }),
              "OperationTable must hold one row per Operation, at the index of its enumerator");
static_assert(Operation {} == Operation::Removed, "a value-initialized instruction is a removed one");

/// The row of @p operation.
/// @param operation An Operation enumerator.
/// @return Its row in OperationTable.
[[nodiscard]] constexpr OperationRow const& operationRow(Operation operation) noexcept
{
    return OperationTable[std::to_underlying(operation)];
}

/// Whether @p operation is one of Operations.def's rows, which a value cast from elsewhere need not be.
[[nodiscard]] constexpr bool isOperation(Operation operation) noexcept
{
    return std::to_underlying(operation) < OperationCount;
}

/// What @p operation does besides computing its results.
/// @param operation An Operation enumerator.
/// @return Its row's effect.
[[nodiscard]] constexpr Effect effectOf(Operation operation) noexcept
{
    return operationRow(operation).effect;
}

/// The name of @p operation in the Printer's text.
/// @param operation An Operation enumerator.
/// @return Its row's text.
[[nodiscard]] constexpr std::string_view nameOf(Operation operation) noexcept
{
    return operationRow(operation).text;
}

/// Whether @p operation ends a block.
/// @param operation An Operation enumerator.
/// @return Whether its effect is Control.
[[nodiscard]] constexpr bool isTerminator(Operation operation) noexcept
{
    return effectOf(operation) == Effect::Control;
}

/// The Type a single-result rule fixes.
/// @param rule A ResultRule.
/// @return It, for I32, I64, F32, F64, Ref and Pointer; none for the rules that fix no type.
[[nodiscard]] constexpr std::optional<Type> fixedTypeOf(ResultRule rule) noexcept
{
    switch (rule)
    {
        case ResultRule::I32: return Type::I32;
        case ResultRule::I64: return Type::I64;
        case ResultRule::F32: return Type::F32;
        case ResultRule::F64: return Type::F64;
        case ResultRule::Ref: return Type::Ref;
        case ResultRule::Pointer: return Type::Pointer;
        case ResultRule::None:
        case ResultRule::SameAsOperand0:
        case ResultRule::FromImmediate:
        case ResultRule::Multiple: return std::nullopt;
    }
    return std::nullopt; // uncovered(result-rule-without-case)
}

/// The row of @p type.
/// @param type A Type enumerator.
/// @return Its row in TypeTable.
[[nodiscard]] constexpr TypeRow const& typeRow(Type type) noexcept
{
    return TypeTable[std::to_underlying(type)];
}

/// Whether @p type is one of TypeTable's rows.
[[nodiscard]] constexpr bool isType(Type type) noexcept
{
    return std::to_underlying(type) <= std::to_underlying(LastType);
}

/// The row of @p value.
[[nodiscard]] constexpr ContextValueRow const& contextValueRow(ContextValue value) noexcept
{
    return ContextValueTable[std::to_underlying(value)];
}

/// The row of @p signature.
[[nodiscard]] constexpr OperandSignatureRow const& operandSignatureRow(OperandSignature signature) noexcept
{
    return OperandSignatureTable[std::to_underlying(signature)];
}

/// The row of @p schema.
[[nodiscard]] constexpr ImmediateSchemaRow const& immediateSchemaRow(ImmediateSchema schema) noexcept
{
    return ImmediateSchemaTable[std::to_underlying(schema)];
}

/// The row of @p field.
[[nodiscard]] constexpr ImmediateFieldRow const& immediateFieldRow(ImmediateField field) noexcept
{
    return ImmediateFieldTable[std::to_underlying(field)];
}

/// The field of the immediate at @p index of an instruction laid out as @p schema.
/// @param schema The layout.
/// @param index The immediate's index; past the fixed fields it must lie in a repeated group, which the
///              schema must have.
/// @return Its field.
[[nodiscard]] constexpr ImmediateField immediateFieldAt(ImmediateSchemaRow const& schema,
                                                        std::size_t index) noexcept
{
    if (index < schema.fixed.size())
        return schema.fixed[index];
    return schema.repeated[(index - schema.fixed.size()) % schema.repeated.size()];
}

} // namespace isoline::compiler::intermediate
