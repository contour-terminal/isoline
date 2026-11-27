// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/ValueCheck.hpp>

using isoline::binary::AbstractHeapType;
using isoline::binary::ExnRef;
using isoline::binary::ExternRef;
using isoline::binary::FuncRef;
using isoline::binary::FuncType;
using isoline::binary::HeapType;
using isoline::binary::Nullable;
using isoline::binary::NullExnRef;
using isoline::binary::ValueKind;
using isoline::binary::ValueType;
using isoline::runtime::Abrupt;
using isoline::runtime::checkValues;
using isoline::runtime::ExnAddr;
using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::FuncAddr;
using isoline::runtime::haveKinds;
using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::HostFailure;
using isoline::runtime::HostFailureKind;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::MismatchKind;
using isoline::runtime::Ref;
using isoline::runtime::RefKind;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::StoreServices;
using isoline::runtime::Thrown;
using isoline::runtime::TypeId;
using isoline::runtime::V128;
using isoline::runtime::Value;
using isoline::runtime::ValueMismatch;

namespace
{

/// A host function that does nothing.
constexpr auto NoHost =
    [](isoline::runtime::HostContext&,
       std::span<isoline::runtime::Value const>,
       std::span<isoline::runtime::Value>) noexcept -> std::expected<void, isoline::runtime::Abrupt> {
    return {};
};

/// @p items as the values a host passes.
template <typename... Items>
std::vector<Value> values(Items... items)
{
    return std::vector<Value> { Value { items }... };
}

/// @p items as the types the values must be of.
template <typename... Items>
std::vector<ValueType> types(Items... items)
{
    return std::vector<ValueType> { items... };
}

/// A non-null reference to the function at @p address.
Ref funcRef(FuncAddr address)
{
    return Ref { .kind = RefKind::Func, .heap = FuncRef.heap, .address = address };
}

/// A non-null reference to the host object @p address.
Ref externRef(uint32_t address)
{
    return Ref { .kind = RefKind::Extern, .heap = ExternRef.heap, .address = address };
}

/// A reference to the exception at @p address.
Ref exnRef(uint32_t address)
{
    return Ref { .kind = RefKind::Exn, .heap = ExnRef.heap, .address = address };
}

/// The null reference of @p heap.
Ref nullOf(AbstractHeapType heap)
{
    return Ref { .kind = RefKind::Null, .heap = HeapType { .kind = heap }, .address = 0 };
}

/// The null funcref.
Ref nullFunc()
{
    return nullOf(AbstractHeapType::Func);
}

/// The null externref.
Ref nullExtern()
{
    return nullOf(AbstractHeapType::Extern);
}

/// (ref func): a funcref that must not be null.
ValueType nonNullFunc()
{
    return isoline::binary::NonNullFuncRef;
}

/// (ref $t), $t the store type @p typeId.
ValueType refTo(TypeId typeId)
{
    return isoline::binary::concreteRef(typeId, Nullable::No);
}

/// The mismatch @p kind at @p index, as checkValues returns it.
std::expected<void, ValueMismatch> mismatch(MismatchKind kind, uint32_t index)
{
    return std::unexpected { ValueMismatch { .kind = kind, .index = index } };
}

/// A store with a function taking an i32, one taking an i64, and an exception.
struct CheckedStore
{
    CheckedStore():
        store { StoreLimits {}, StoreServices { .allocator = allocator } },
        takesI32 { store
                       .addHostFunction(FuncType { .params = { isoline::binary::I32 }, .results = {} },
                                        isoline::runtime::makeHostFunction(NoHost))
                       .value() },
        takesI64 { store
                       .addHostFunction(FuncType { .params = { isoline::binary::I64 }, .results = {} },
                                        isoline::runtime::makeHostFunction(NoHost))
                       .value() },
        typeTakingI32 { store.function(takesI32).typeId },
        exception { store.addException(store.addTag(store.internType(FuncType {})).value(), {}).value() }
    {
    }

    HeapMemoryAllocator allocator; ///< Allocates the store's memories; it has none.
    Store store;                   ///< The store.
    FuncAddr takesI32;             ///< A function of type [i32] -> [].
    FuncAddr takesI64;             ///< A function of type [i64] -> [].
    TypeId typeTakingI32;          ///< The type of takesI32.
    uint32_t exception;            ///< A live exception.
};

} // namespace

TEST_CASE("ValueCheck.haveKinds")
{
    static_assert(noexcept(haveKinds(std::span<Value const> {}, std::span<ValueType const> {})));
    CHECK(haveKinds(values(I32 {}, I64 {}), types(isoline::binary::I32, isoline::binary::I64)));
    CHECK_FALSE(haveKinds(values(I64 {}), types(isoline::binary::I32)));
    CHECK_FALSE(haveKinds(values(I32 {}), types(isoline::binary::I32, isoline::binary::I32)));
}

TEST_CASE("ValueCheck.checkValuesRejectsEveryMismatch")
{
    auto const fixture = CheckedStore {};
    auto const& store = fixture.store;
    auto const takesI32 = fixture.takesI32;
    auto const takesI64 = fixture.takesI64;
    auto const typeTakingI32 = fixture.typeTakingI32;

    CHECK(checkValues(store, values(externRef(1)), types(FuncRef)) == mismatch(MismatchKind::Heap, 0));
    CHECK(checkValues(store, values(funcRef(999)), types(FuncRef))
          == mismatch(MismatchKind::ForeignReference, 0));
    CHECK(checkValues(store, values(nullFunc()), types(nonNullFunc()))
          == mismatch(MismatchKind::NotNullable, 0));
    CHECK(checkValues(store, values(funcRef(takesI64)), types(refTo(typeTakingI32)))
          == mismatch(MismatchKind::Heap, 0));
    CHECK(checkValues(store, values(I32 {}, F32 {}), types(isoline::binary::I32, isoline::binary::I64))
          == mismatch(MismatchKind::Kind, 1));
    CHECK(checkValues(store, values(), types(isoline::binary::I32)) == mismatch(MismatchKind::Count, 0));
    CHECK(checkValues(store, values(funcRef(takesI32), nullExtern()), types(refTo(typeTakingI32), ExternRef))
              .has_value());
}

TEST_CASE("ValueCheck.countIsCheckedFirst")
{
    // Values that do not pair up with their types are not compared one by one: the count is the
    // mismatch, at the first position one list lacks.
    auto const fixture = CheckedStore {};
    CHECK(checkValues(fixture.store, values(F32 {}, I32 {}), types(isoline::binary::I32))
          == mismatch(MismatchKind::Count, 1));
    CHECK(checkValues(fixture.store, values(I32 {}), types()) == mismatch(MismatchKind::Count, 0));
    CHECK(checkValues(fixture.store, values(), types()).has_value());
}

TEST_CASE("ValueCheck.functionReferences")
{
    auto const fixture = CheckedStore {};
    auto const& store = fixture.store;
    auto const nullableTo = isoline::binary::concreteRef(fixture.typeTakingI32, Nullable::Yes);
    auto const noFunc = ValueType { .kind = ValueKind::Ref,
                                    .nullable = Nullable::Yes,
                                    .heap = { .kind = AbstractHeapType::NoFunc } };

    // Any function is a funcref, and a (ref null $t) of its type.
    CHECK(checkValues(store, values(funcRef(fixture.takesI64)), types(FuncRef)).has_value());
    CHECK(checkValues(store, values(funcRef(fixture.takesI32)), types(nullableTo)).has_value());
    CHECK(checkValues(store, values(funcRef(fixture.takesI32)), types(nonNullFunc())).has_value());
    // No function is in nofunc, whose only value is null.
    CHECK(checkValues(store, values(funcRef(fixture.takesI32)), types(noFunc))
          == mismatch(MismatchKind::Heap, 0));
    // A function's heap is checked before its address: an externref slot refuses any funcref.
    CHECK(checkValues(store, values(funcRef(999)), types(ExternRef)) == mismatch(MismatchKind::Heap, 0));
    // A missing function is foreign whichever function type is asked for.
    CHECK(checkValues(store, values(funcRef(999)), types(nullableTo))
          == mismatch(MismatchKind::ForeignReference, 0));
}

TEST_CASE("ValueCheck.nullReferences")
{
    auto const fixture = CheckedStore {};
    auto const& store = fixture.store;
    auto const nullableTo = isoline::binary::concreteRef(fixture.typeTakingI32, Nullable::Yes);

    // A null of any heap of the type's hierarchy will do: its own type is the hierarchy's bottom.
    CHECK(checkValues(store, values(nullOf(AbstractHeapType::NoFunc)), types(FuncRef)).has_value());
    CHECK(checkValues(store, values(nullFunc()), types(nullableTo)).has_value());
    CHECK(checkValues(store, values(nullOf(AbstractHeapType::Exn)), types(NullExnRef)).has_value());
    CHECK(checkValues(store, values(nullOf(AbstractHeapType::NoExtern)), types(ExternRef)).has_value());
    // A null of another hierarchy will not.
    CHECK(checkValues(store, values(nullExtern()), types(FuncRef)) == mismatch(MismatchKind::Heap, 0));
    CHECK(checkValues(store, values(nullFunc()), types(ExnRef)) == mismatch(MismatchKind::Heap, 0));
    // bot belongs to no hierarchy, and a host's out-of-range heap to none either: neither reaches a
    // precondition.
    CHECK(checkValues(store, values(nullOf(AbstractHeapType::Bot)), types(FuncRef))
          == mismatch(MismatchKind::Heap, 0));
    CHECK(checkValues(store, values(nullOf(static_cast<AbstractHeapType>(0xFF))), types(FuncRef))
          == mismatch(MismatchKind::Heap, 0));
    // A non-nullable type refuses null before its heap is looked at.
    CHECK(checkValues(store, values(nullExtern()), types(refTo(fixture.typeTakingI32)))
          == mismatch(MismatchKind::NotNullable, 0));
}

TEST_CASE("ValueCheck.hostAndExceptionReferences")
{
    auto const fixture = CheckedStore {};
    auto const& store = fixture.store;
    auto const nonNullExtern = ValueType { .kind = ValueKind::Ref,
                                           .nullable = Nullable::No,
                                           .heap = { .kind = AbstractHeapType::Extern } };

    // A host object's address is the embedder's: any is accepted, in extern only.
    CHECK(checkValues(store, values(externRef(0xFFFF'FFFF)), types(nonNullExtern)).has_value());
    CHECK(checkValues(store, values(externRef(1)), types(ExnRef)) == mismatch(MismatchKind::Heap, 0));
    // The bottom of extern, (ref noextern), holds no host object: only null, which it refuses too.
    auto const nonNullNoExtern = ValueType { .kind = ValueKind::Ref,
                                             .nullable = Nullable::No,
                                             .heap = { .kind = AbstractHeapType::NoExtern } };
    CHECK(checkValues(store, values(externRef(1)), types(nonNullNoExtern))
          == mismatch(MismatchKind::Heap, 0));

    // An exception must be live in this store: one it never allocated is foreign (carried from P2).
    CHECK(checkValues(store, values(exnRef(fixture.exception)), types(ExnRef)).has_value());
    CHECK(store.isLiveException(fixture.exception));
    CHECK_FALSE(store.isLiveException(fixture.exception + 1));
    CHECK(checkValues(store, values(exnRef(fixture.exception + 1)), types(ExnRef))
          == mismatch(MismatchKind::ForeignReference, 0));
    // In exn only: nullexnref holds no exception.
    CHECK(checkValues(store, values(exnRef(fixture.exception)), types(NullExnRef))
          == mismatch(MismatchKind::Heap, 0));
    CHECK(checkValues(store, values(exnRef(fixture.exception)), types(FuncRef))
          == mismatch(MismatchKind::Heap, 0));
}

TEST_CASE("ValueCheck.reclaimedExceptionIsForeign")
{
    // Two exceptions nothing refers to, in a store of two: the third allocation's sweep reclaims both
    // and reuses the lowest address, so an exnref to the other one is stale: refused, not read.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits { .maxExceptions = 2 }, StoreServices { .allocator = allocator } };
    auto const tag = store.addTag(store.internType(FuncType {})).value();
    auto const first = store.addException(tag, {}).value();
    auto const second = store.addException(tag, {}).value();
    CHECK(store.addException(tag, {}).value() == first);
    CHECK(store.liveExceptions() == 1);
    CHECK_FALSE(store.isLiveException(second));
    CHECK(checkValues(store, values(exnRef(second)), types(ExnRef))
          == mismatch(MismatchKind::ForeignReference, 0));
}

TEST_CASE("ValueCheck.kindsBeforeReferences")
{
    auto const fixture = CheckedStore {};
    auto const& store = fixture.store;
    // A reference where a number is due, and a number where a reference is, are kinds apart.
    CHECK(checkValues(store, values(nullFunc()), types(isoline::binary::I32))
          == mismatch(MismatchKind::Kind, 0));
    CHECK(checkValues(store, values(I32 {}), types(FuncRef)) == mismatch(MismatchKind::Kind, 0));
    // A reference of a kind no RefKind names is no reference at all.
    auto const unnamed = Ref { .kind = static_cast<RefKind>(0xFF), .heap = FuncRef.heap, .address = 0 };
    CHECK(checkValues(store, values(unnamed), types(FuncRef)) == mismatch(MismatchKind::Kind, 0));
    // The first mismatch is reported.
    CHECK(checkValues(store,
                      values(I32 {}, externRef(1), funcRef(999)),
                      types(isoline::binary::I32, FuncRef, FuncRef))
          == mismatch(MismatchKind::Heap, 1));
}

TEST_CASE("ValueCheck.exceptionPayloadsAreChecked")
{
    // Store::addException checks its payload as host values are, so a payload of the tag's types
    // is accepted (one of other types is a precondition violation, which hosts are kept from).
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const function =
        store.addHostFunction(FuncType {}, isoline::runtime::makeHostFunction(NoHost)).value();
    auto const tag =
        store.addTag(store.internType(FuncType { .params = { FuncRef }, .results = {} })).value();
    CHECK(store.addException(tag, values(funcRef(function))).has_value());
    auto const last = store.addException(tag, values(nullFunc()));
    REQUIRE(last.has_value());

    // A host allocates exceptions through its context, which checks the payload instead of failing the
    // store's precondition: a payload of another type is the host's failure, and the process lives on.
    auto interpreter = isoline::interpreter::ReferenceInterpreter {};
    auto const host =
        store
            .addHostFunction(
                FuncType {},
                isoline::runtime::makeHostFunction([tag](isoline::runtime::HostContext& context,
                                                         std::span<Value const>,
                                                         std::span<Value>) noexcept
                                                       -> std::expected<void, Abrupt> {
                    return context.addException(tag, values(externRef(1))).and_then([](ExnAddr exception) {
                        return std::expected<void, Abrupt> { std::unexpected {
                            Abrupt { Thrown { .exception = exception } } } };
                    });
                }))
            .value();
    CHECK(isoline::runtime::invokeChecked(store, interpreter, host, {}, {}).outcome
          == std::unexpected { Abrupt {
              HostFailure { .kind = HostFailureKind::ArgumentMismatch,
                            .code = 0,
                            .mismatch = ValueMismatch { .kind = MismatchKind::Heap, .index = 0 } } } });
    CHECK_FALSE(store.isLiveException(*last + 1)); // nothing was allocated after it
}

TEST_CASE("ValueCheck.nanPayloadsPassThrough")
{
    // A float's bits are its value: a NaN of any payload, signalling ones included, is of its type.
    auto const fixture = CheckedStore {};
    CHECK(checkValues(fixture.store,
                      values(F32 { 0x7FA0'0001 },
                             F64 { 0xFFF0'0000'0000'0001 },
                             V128 { .lanes = { 0x7FA0'0001'FFC0'0002, 0x7FF0'0000'0000'0003 } }),
                      types(isoline::binary::F32, isoline::binary::F64, isoline::binary::V128))
              .has_value());
}
