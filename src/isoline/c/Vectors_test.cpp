// SPDX-License-Identifier: Apache-2.0
//
// What a vector owns (wasm.h's WASM_DECLARE_VEC families, detail/Vector.hpp): delete deletes its elements,
// copy deep-copies them, and vec_new takes them. The elements are real ones, made through the C API as a
// caller makes them, for every family of pointers whose element type exists (the types); ASan holds a copy
// that is not deep, or an element deleted twice or never, to the memory. (A copy that fails midway is
// VectorHostMemory_test.cpp's, under a refusing allocator.)
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <isoline/c/detail/Handles.hpp>
#include <isoline/c/detail/Vector.hpp>
#include <isoline/c/isoline.h>
#include <isoline/c/testing/TypeElements.hpp>

using isoline::c::detail::ElementOwnership;
using isoline::c::detail::Object;
using isoline::c::testing::makeElement;

namespace
{

/// The reference values a vector owns: a test double, the one left, since wasm_ref_t stays abstract until
/// the store's references exist (Task 14). It counts itself, so that the test sees each reference deleted
/// once and copied deeply.
class CountedReference final: public wasm_ref_t
{
  public:
    explicit CountedReference(int& live) noexcept: _live { &live } { ++*_live; }
    CountedReference(CountedReference const& other) noexcept: wasm_ref_t(other), _live { other._live }
    {
        ++*_live;
    }
    CountedReference(CountedReference&&) = delete;
    CountedReference& operator=(CountedReference const&) = delete;
    CountedReference& operator=(CountedReference&&) = delete;
    ~CountedReference() override { --*_live; }

    [[nodiscard]] auto clone() const -> std::unique_ptr<Object> override
    {
        return std::make_unique<CountedReference>(*this);
    }

  private:
    int* _live; ///< The references alive.
};

/// One vector family of wasm.h, from its row: its types, its ownership and its five functions.
template <typename VectorType,
          ElementOwnership ElementRule,
          auto NewEmptyFunction,
          auto NewUninitializedFunction,
          auto NewFunction,
          auto CopyFunction,
          auto DeleteFunction>
struct Family
{
    using Vector = VectorType;
    using Element = isoline::c::detail::ElementOf<Vector>;
    static constexpr auto Ownership = ElementRule;
    static constexpr auto NewEmpty = NewEmptyFunction;
    static constexpr auto NewUninitialized = NewUninitializedFunction;
    static constexpr auto New = NewFunction;
    static constexpr auto Copy = CopyFunction;
    static constexpr auto Delete = DeleteFunction;
};

#define ISOLINE_VECTOR_FAMILY(name, ownership)   \
    Family<wasm_##name##_vec_t,                  \
           ElementOwnership::ownership,          \
           &wasm_##name##_vec_new_empty,         \
           &wasm_##name##_vec_new_uninitialized, \
           &wasm_##name##_vec_new,               \
           &wasm_##name##_vec_copy,              \
           &wasm_##name##_vec_delete>

// Every family, and the families of pointers, as type lists for the template cases below.
#define ISOLINE_VECTOR_ROW(name, ownership) \
    , std::declval<std::tuple<ISOLINE_VECTOR_FAMILY(name, ownership)>>()
using AllFamilies = decltype(std::tuple_cat(std::declval<std::tuple<>>()
#include <isoline/c/detail/VectorRows.def>
                                                ));
#undef ISOLINE_VECTOR_ROW

#define ISOLINE_VECTOR_ROW(name, ownership) ISOLINE_VECTOR_ROW_##ownership(name)
#define ISOLINE_VECTOR_ROW_Plain(name)
#define ISOLINE_VECTOR_ROW_Value(name)
#define ISOLINE_VECTOR_ROW_Owned(name) , std::declval<std::tuple<ISOLINE_VECTOR_FAMILY(name, Owned)>>()
using PointerFamilies = decltype(std::tuple_cat(std::declval<std::tuple<>>()
#include <isoline/c/detail/VectorRows.def>
                                                    ));
#undef ISOLINE_VECTOR_ROW
#undef ISOLINE_VECTOR_ROW_Plain
#undef ISOLINE_VECTOR_ROW_Value
#undef ISOLINE_VECTOR_ROW_Owned

/// The names of the rows the families above were made from: the lists cannot lose a row unnoticed.
constexpr auto RowNames = std::to_array<std::string_view>({
#define ISOLINE_VECTOR_ROW(name, ownership) std::string_view { #name },
#include <isoline/c/detail/VectorRows.def>
#undef ISOLINE_VECTOR_ROW
});
static_assert(std::tuple_size_v<AllFamilies> == RowNames.size());

/// An element of a family no task makes yet (UnmadeElementRows), whose copy runs out of host memory: a test
/// double over the abstract wasm_extern_t and wasm_frame_t, which goes when the store's externs exist (Task
/// 13); frames are never made.
template <typename Base>
class Unclonable final: public Base
{
  public:
    Unclonable() = default;
    Unclonable(Unclonable const&) = delete;
    Unclonable(Unclonable&&) = delete;
    Unclonable& operator=(Unclonable const&) = delete;
    Unclonable& operator=(Unclonable&&) = delete;
    ~Unclonable() override = default;

    [[nodiscard]] auto clone() const -> std::unique_ptr<Object> override { throw std::bad_alloc {}; }
};

// The families whose element type exists: the types. Every other row of pointers is listed by name, so that a
// new row is either made here or named as not yet made.
using TypeFamilies = std::tuple<ISOLINE_VECTOR_FAMILY(valtype, Owned),
                                ISOLINE_VECTOR_FAMILY(functype, Owned),
                                ISOLINE_VECTOR_FAMILY(globaltype, Owned),
                                ISOLINE_VECTOR_FAMILY(tabletype, Owned),
                                ISOLINE_VECTOR_FAMILY(memorytype, Owned),
                                ISOLINE_VECTOR_FAMILY(tagtype, Owned),
                                ISOLINE_VECTOR_FAMILY(externtype, Owned),
                                ISOLINE_VECTOR_FAMILY(importtype, Owned),
                                ISOLINE_VECTOR_FAMILY(exporttype, Owned)>;
/// The rows of pointers whose elements no task makes yet: externs (the store's, Task 13) and frames (never;
/// Isoline records no backtraces).
constexpr auto UnmadeElementRows = std::to_array<std::string_view>({ "extern", "frame" });
/// Their families, as a type list.
using UnmadeFamilies = std::tuple<ISOLINE_VECTOR_FAMILY(extern, Owned), ISOLINE_VECTOR_FAMILY(frame, Owned)>;
static_assert(std::tuple_size_v<UnmadeFamilies> == UnmadeElementRows.size());
static_assert(std::tuple_size_v<TypeFamilies> + UnmadeElementRows.size()
              == std::tuple_size_v<PointerFamilies>);

/// A use of @p element that reads all it owns: a copy of it, deleted again. Of an element deleted already,
/// or one sharing what it owns with another that is, the sanitizers report it.
template <typename Element>
void touch(Element element)
{
    delete isoline::c::detail::cloneOf(*element).release();
}

} // namespace

TEMPLATE_LIST_TEST_CASE("Vectors.everyRowIsAFamilyOfItsOwnership", "[c-api][vectors]", AllFamilies)
{
    // wasm.h's `*` (an own pointer) is exactly the Owned rows.
    STATIC_REQUIRE(std::is_pointer_v<typename TestType::Element>
                   == (TestType::Ownership == ElementOwnership::Owned));
    // The value row is wasm_val_t's, whose elements may hold references.
    STATIC_REQUIRE(std::is_same_v<typename TestType::Element, wasm_val_t>
                   == (TestType::Ownership == ElementOwnership::Value));
}

TEMPLATE_LIST_TEST_CASE("Vectors.emptyIsEmptyInEveryFamily", "[c-api][vectors]", AllFamilies)
{
    // new_empty, and vec_new of nothing, write the empty vector over whatever the caller's struct held.
    auto element = typename TestType::Element {};
    typename TestType::Vector vector { .size = 7, .data = &element };
    TestType::NewEmpty(&vector);
    CHECK(vector.size == 0);
    CHECK(vector.data == nullptr);
    vector = { .size = 7, .data = &element };
    TestType::New(&vector, 0, nullptr);
    CHECK(vector.size == 0);
    CHECK(vector.data == nullptr);
    TestType::Delete(&vector);
    CHECK(vector.size == 0);
}

TEMPLATE_LIST_TEST_CASE("Vectors.copyIsDeep", "[c-api][vectors]", TypeFamilies)
{
    using Element = TestType::Element;
    auto const elements = std::array { makeElement<Element>(), Element {}, makeElement<Element>() };
    REQUIRE(elements[0] != nullptr);
    REQUIRE(elements[2] != nullptr);
    typename TestType::Vector original {};
    TestType::New(&original, elements.size(), elements.data());
    REQUIRE(original.size == 3);
    // vec_new takes the elements it is given: no copy is made.
    CHECK(original.data[0] == elements[0]);
    CHECK(original.data[2] == elements[2]);

    typename TestType::Vector copy {};
    TestType::Copy(&copy, &original);
    REQUIRE(copy.size == 3);
    CHECK(copy.data[0] != original.data[0]);
    CHECK(copy.data[2] != original.data[2]);
    // A slot not yet filled is copied as one.
    CHECK(copy.data[1] == nullptr);

    // The copy survives its original, whose delete deletes the original's elements and only those.
    TestType::Delete(&original);
    CHECK(original.size == 0);
    CHECK(original.data == nullptr);
    touch(copy.data[0]);
    touch(copy.data[2]);
    TestType::Delete(&copy);
}

TEMPLATE_LIST_TEST_CASE("Vectors.refusedNewTakesItsElements", "[c-api][vectors]", TypeFamilies)
{
    using Element = TestType::Element;

    // No vector to make: the elements are taken all the same, and deleted (LeakSanitizer holds the test to
    // it; the caller deletes nothing).
    auto const taken = std::array { makeElement<Element>(), Element {}, makeElement<Element>() };
    REQUIRE(taken[0] != nullptr);
    TestType::New(nullptr, taken.size(), taken.data());

    // A size no vector has cannot be walked: those elements are left untouched, and the caller still owns
    // them.
    auto const element = makeElement<Element>();
    REQUIRE(element != nullptr);
    auto const kept = std::array { element };
    typename TestType::Vector vector {};
    TestType::New(&vector, isoline::c::detail::MaxVectorSize<Element> + 1, kept.data());
    CHECK(vector.size == 0);
    CHECK(vector.data == nullptr);
    touch(element);
    delete element;
}

TEMPLATE_LIST_TEST_CASE("Vectors.copyOntoItselfIsACopy", "[c-api][vectors]", AllFamilies)
{
    // wasm_<name>_vec_copy(&v, &v): the copy replaces the vector it was made from, whose data the caller
    // still owns through the pointer it read before.
    typename TestType::Vector vector {};
    TestType::NewUninitialized(&vector, 2);
    auto before = vector;
    TestType::Copy(&vector, &vector);
    CHECK(vector.size == 2);
    CHECK(vector.data != before.data);
    TestType::Delete(&before);
    TestType::Delete(&vector);
}

TEMPLATE_LIST_TEST_CASE("Vectors.unmadeFamiliesCopyOutOfMemoryIsEmpty", "[c-api][vectors]", UnmadeFamilies)
{
    // The copy of an element runs out of host memory: the family's wasm_<name>_vec_copy() gives the empty
    // vector, and the original is as it was.
    using Element = TestType::Element;
    auto const elements = std::array<Element, 1> { new Unclonable<std::remove_pointer_t<Element>> {} };
    typename TestType::Vector original {};
    TestType::New(&original, elements.size(), elements.data());
    REQUIRE(original.size == 1);
    typename TestType::Vector copy { .size = 7, .data = nullptr };
    TestType::Copy(&copy, &original);
    CHECK(copy.size == 0);
    CHECK(copy.data == nullptr);
    CHECK(original.data[0] == elements[0]);
    TestType::Delete(&original);
}

TEST_CASE("Vectors.valuesOwnTheirReferences", "[c-api][vectors]")
{
    // A value of every reference kind (detail/EnumRows.hpp: externref, funcref, exnref) owns its reference: a
    // copy copies it, and delete deletes it. A null reference is copied as one.
    // The reference kinds, spelled out rather than read from ReferenceValueKinds, which this checks.
    constexpr auto ReferenceKinds =
        std::to_array<wasm_valkind_t>({ WASM_EXTERNREF, WASM_FUNCREF, ISOLINE_VALKIND_EXNREF });
    constexpr auto References = ReferenceKinds.size();
    auto live = 0;
    // Each reference kind twice, a reference then null, and three values whose bits are no pointer: a
    // number's, v128's and those of a kind no row knows.
    auto values = std::array<wasm_val_t, (2 * References) + 3> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, References))
    {
        auto const kind = ReferenceKinds[index];
        values[2 * index].kind = kind;
        values[2 * index].of.ref = new CountedReference(live);
        values[(2 * index) + 1].kind = kind;
        values[(2 * index) + 1].of.ref = nullptr;
    }
    values[2 * References] = wasm_val_t { .kind = WASM_I64, .of = { .i64 = 0x1234 } };
    values[(2 * References) + 1] = wasm_val_t { .kind = ISOLINE_VALKIND_V128, .of = { .i64 = 0x5678 } };
    values[(2 * References) + 2] = wasm_val_t { .kind = 77, .of = { .i64 = 0x9ABC } };
    auto const references = static_cast<int>(References);
    REQUIRE(live == references);

    wasm_val_vec_t original {};
    wasm_val_vec_new(&original, values.size(), values.data());
    REQUIRE(original.size == values.size());

    wasm_val_vec_t copy {};
    wasm_val_vec_copy(&copy, &original);
    REQUIRE(copy.size == values.size());
    CHECK(live == 2 * references);
    for (auto const index: std::views::iota(std::size_t { 0 }, References))
    {
        CHECK(copy.data[2 * index].of.ref != original.data[2 * index].of.ref);
        CHECK(copy.data[(2 * index) + 1].of.ref == nullptr);
    }
    auto const numbers = std::span { copy.data, copy.size }.last(3);
    CHECK(numbers[0].of.i64 == 0x1234);
    CHECK(numbers[1].of.i64 == 0x5678);
    CHECK(numbers[2].of.i64 == 0x9ABC);

    wasm_val_vec_delete(&original);
    CHECK(live == references);
    wasm_val_vec_delete(&copy);
    CHECK(live == 0);
}
